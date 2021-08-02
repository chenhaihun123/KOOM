/*
 * Copyright (c) 2021. Kwai, Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Created by lbtrace on 2021.
 *
 */

#include <ctype.h>
#include <elf.h>
#include <inttypes.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <vector>
#include <dlfcn.h>
#include <cxxabi.h>
#include <utils/log_util.h>

#include "memory_map.h"

#if defined(__LP64__)
#define PAD_PTR "016" PRIxPTR
#else
#define PAD_PTR "08" PRIxPTR
#endif

// Format of /proc/<PID>/maps:
// 6f000000-6f01e000 rwxp 00000000 00:0c 16389419   /system/lib/libcomposer.so
static MapEntry *ParseLine(char *line) {
  uintptr_t start;
  uintptr_t end;
  uintptr_t offset;
  int flags;
  char permissions[5];
  int name_pos;
  if (sscanf(line, "%" PRIxPTR "-%" PRIxPTR " %4s %" PRIxPTR " %*x:%*x %*d %n", &start, &end,
             permissions, &offset, &name_pos) < 2) {
    return nullptr;
  }

  const char *name = line + name_pos;
  size_t name_len = strlen(name);
  if (name_len && name[name_len - 1] == '\n') {
    name_len -= 1;
  }

  flags = 0;
  if (permissions[0] == 'r') {
    flags |= PROT_READ;
  }
  if (permissions[2] == 'x') {
    flags |= PROT_EXEC;
  }

  MapEntry *entry = new MapEntry(start, end, offset, name, name_len, flags);
  if (!(flags & PROT_READ)) {
    // Any unreadable map will just get a zero load bias.
    entry->load_bias = 0;
    entry->init = true;
    entry->valid = false;
  }
  return entry;
}

template<typename T>
static inline bool GetVal(MapEntry *entry, uintptr_t addr, T *store) {
  if (!(entry->flags & PROT_READ) || addr < entry->start || addr + sizeof(T) > entry->end) {
    return false;
  }
  // Make sure the address is aligned properly.
  if (addr & (sizeof(T) - 1)) {
    return false;
  }
  *store = *reinterpret_cast<T *>(addr);
  return true;
}

static bool ValidElf(MapEntry *entry) {
  uintptr_t addr = entry->start;
  uintptr_t end;
  if (__builtin_add_overflow(addr, SELFMAG, &end) || end >= entry->end) {
    return false;
  }

  return memcmp(reinterpret_cast<void *>(addr), ELFMAG, SELFMAG) == 0;
}

static void ReadLoadbias(MapEntry *entry) {
  entry->load_bias = 0;
  uintptr_t addr = entry->start;
  ElfW(Ehdr) ehdr;
  if (!GetVal<ElfW(Half)>(entry, addr + offsetof(ElfW(Ehdr), e_phnum), &ehdr.e_phnum)) {
    return;
  }
  if (!GetVal<ElfW(Off)>(entry, addr + offsetof(ElfW(Ehdr), e_phoff), &ehdr.e_phoff)) {
    return;
  }
  addr += ehdr.e_phoff;
  for (size_t i = 0; i < ehdr.e_phnum; i++) {
    ElfW(Phdr) phdr;
    if (!GetVal<ElfW(Word)>(entry, addr + offsetof(ElfW(Phdr), p_type), &phdr.p_type)) {
      return;
    }
    if (!GetVal<ElfW(Word)>(entry, addr + offsetof(ElfW(Phdr), p_flags), &phdr.p_flags)) {
      return;
    }
    if (!GetVal<ElfW(Off)>(entry, addr + offsetof(ElfW(Phdr), p_offset), &phdr.p_offset)) {
      return;
    }
    if ((phdr.p_type == PT_LOAD) && (phdr.p_flags & PF_X)) {
      if (!GetVal<ElfW(Addr)>(entry, addr + offsetof(ElfW(Phdr), p_vaddr), &phdr.p_vaddr)) {
        return;
      }
      entry->load_bias = phdr.p_vaddr - phdr.p_offset;
      return;
    }
    addr += sizeof(phdr);
  }
}

static void inline Init(MapEntry *entry) {
  if (entry->init) {
    return;
  }
  entry->init = true;
  if (ValidElf(entry)) {
    entry->valid = true;
    ReadLoadbias(entry);
  }
}

bool MemoryMap::ReadMaps() {
  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr) {
    return false;
  }

  std::vector<char> buffer(1024);
  while (fgets(buffer.data(), buffer.size(), fp) != nullptr) {
    MapEntry *entry = ParseLine(buffer.data());
    if (entry == nullptr) {
      fclose(fp);
      return false;
    }

    auto it = entries_.find(entry);
    if (it == entries_.end()) {
      entries_.insert(entry);
    } else {
      delete entry;
    }
  }
  fclose(fp);
  return true;
}

MemoryMap::~MemoryMap() {
  for (auto *entry : entries_) {
    delete entry;
  }
  entries_.clear();
}

MapEntry *MemoryMap::CalculateRelPc(uintptr_t pc, uintptr_t *rel_pc) {
  MapEntry pc_entry(pc);

  auto it = entries_.find(&pc_entry);
  if (it == entries_.end()) {
    ReadMaps();
  }
  it = entries_.find(&pc_entry);
  if (it == entries_.end()) {
    return nullptr;
  }

  MapEntry *entry = *it;
  Init(entry);

  if (rel_pc != nullptr) {
    // Need to check to see if this is a read-execute map and the read-only
    // map is the previous one.
    if (!entry->valid && it != entries_.begin()) {
      MapEntry *prev_entry = *--it;
      if (prev_entry->flags == PROT_READ && prev_entry->offset < entry->offset &&
          prev_entry->name == entry->name) {
        Init(prev_entry);

        if (prev_entry->valid) {
          entry->elf_start_offset = prev_entry->offset;
          *rel_pc = pc - entry->start + entry->offset + prev_entry->load_bias;
          return entry;
        }
      }
    }
    *rel_pc = pc - entry->start + entry->load_bias;
  }
  return entry;
}

std::string MemoryMap::FormatBacktrace(const uintptr_t* frames, size_t frame_count) {
  std::string str;
  uintptr_t offset = 0;
  const char *symbol = nullptr;

  for (size_t cursor = 0; cursor < frame_count; cursor++) {
    Dl_info info;
    DLOGI("frame %p %d", frames[cursor], frame_count);
    if (dladdr(reinterpret_cast<void *>(frames[cursor]), &info) != 0) {
      offset = reinterpret_cast<uintptr_t>(info.dli_saddr);
      symbol = info.dli_sname;
    } else {
      info.dli_fname = nullptr;
    }

    uintptr_t rel_pc = offset;
    MapEntry *entry = CalculateRelPc(frames[cursor], &rel_pc);
    if (entry->NeedIgnore()) {
      break;
    }

    const char *soname = (entry != nullptr) ? entry->name.c_str() : info.dli_fname;
    if (soname == nullptr) {
      soname = "<unknown>";
    }

    char offset_buf[128];
    if (entry != nullptr && entry->elf_start_offset != 0) {
      snprintf(offset_buf, sizeof(offset_buf), " (offset 0x%" PRIxPTR ")", entry->elf_start_offset);
    } else {
      offset_buf[0] = '\0';
    }

    char buf[1024];
    if (symbol != nullptr) {
      char *demangled_name = abi::__cxa_demangle(symbol, nullptr, nullptr, nullptr);
      const char *name;
      if (demangled_name != nullptr) {
        name = demangled_name;
      } else {
        name = symbol;
      }
      snprintf(buf, sizeof(buf),
               "          #%02zd  pc %"
               PAD_PTR
               "  %s%s (%s+%" PRIuPTR ")\n",
               cursor, rel_pc, soname, offset_buf, name,
               frames[cursor] - offset);
      free(demangled_name);
    } else {
      snprintf(buf, sizeof(buf), "          #%02zd  pc %"
                                 PAD_PTR
                                 "  %s%s\n",
               cursor, rel_pc, soname, offset_buf);
    }
    str += buf;
  }

  return std::move(str);
}


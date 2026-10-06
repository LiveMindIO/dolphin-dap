// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/ElfTypes.h"

namespace File
{
class IOFile;
}

enum KnownElfTypes
{
  KNOWNELF_PSP = 0,
  KNOWNELF_DS = 1,
  KNOWNELF_GBA = 2,
  KNOWNELF_GC = 3,
};

typedef int SectionID;

class ElfReader final : public BootExecutableReader
{
public:
  explicit ElfReader(const std::string& filename);
  explicit ElfReader(File::IOFile file);
  explicit ElfReader(std::vector<u8> buffer);
  ~ElfReader() override;
  // Quick accessors
  ElfType GetType() const { return header ? static_cast<ElfType>(header->e_type) : ET_NONE; }
  ElfMachine GetMachine() const
  {
    return header ? static_cast<ElfMachine>(header->e_machine) : EM_NONE;
  }
  u32 GetEntryPoint() const override { return entryPoint; }
  u32 GetFlags() const { return header ? header->e_flags : 0; }
  bool LoadIntoMemory(Core::System& system, bool only_in_mem1 = false) const override;
  bool LoadSymbols(const Core::CPUThreadGuard& guard, PPCSymbolDB& ppc_symbol_db,
                   const std::string& filename) const override;
  bool IsValid() const override { return m_is_valid; }
  bool IsPPCExecutable() const;
  bool IsWii() const override;

  int GetNumSegments() const { return header ? static_cast<int>(header->e_phnum) : 0; }
  int GetNumSections() const { return header ? static_cast<int>(header->e_shnum) : 0; }
  const u8* GetPtr(size_t offset) const
  {
    return offset <= m_bytes.size() ? reinterpret_cast<const u8*>(base) + offset : nullptr;
  }
  const char* GetSectionName(int section) const;
  const u8* GetSectionDataPtr(int section) const;
  bool IsCodeSegment(int segment) const;
  const u8* GetSegmentPtr(int segment) const;
  u32 GetSegmentSize(int segment) const;
  u32 GetSectionSize(SectionID section) const;
  SectionID GetSectionByName(const char* name, int firstSection = 0) const;  //-1 for not found

  bool DidRelocate() const { return bRelocate; }

private:
  bool Initialize();

  char* base = nullptr;

  Elf32_Ehdr* header = nullptr;
  Elf32_Phdr* segments = nullptr;
  Elf32_Shdr* sections = nullptr;

  bool m_is_valid = false;
  bool bRelocate = false;
  u32 entryPoint = 0;
};

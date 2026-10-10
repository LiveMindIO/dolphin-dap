// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Boot/ElfReader.h"

#include <cstring>
#include <span>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "Core/Debugger/DWARF/DwarfImport.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"

static void bswap(u32& w)
{
  w = Common::swap32(w);
}
static void bswap(u16& w)
{
  w = Common::swap16(w);
}

static void byteswapHeader(Elf32_Ehdr& ELF_H)
{
  bswap(ELF_H.e_type);
  bswap(ELF_H.e_machine);
  bswap(ELF_H.e_ehsize);
  bswap(ELF_H.e_phentsize);
  bswap(ELF_H.e_phnum);
  bswap(ELF_H.e_shentsize);
  bswap(ELF_H.e_shnum);
  bswap(ELF_H.e_shstrndx);
  bswap(ELF_H.e_version);
  bswap(ELF_H.e_entry);
  bswap(ELF_H.e_phoff);
  bswap(ELF_H.e_shoff);
  bswap(ELF_H.e_flags);
}

static void byteswapSegment(Elf32_Phdr& sec)
{
  bswap(sec.p_align);
  bswap(sec.p_filesz);
  bswap(sec.p_flags);
  bswap(sec.p_memsz);
  bswap(sec.p_offset);
  bswap(sec.p_paddr);
  bswap(sec.p_vaddr);
  bswap(sec.p_type);
}

static void byteswapSection(Elf32_Shdr& sec)
{
  bswap(sec.sh_addr);
  bswap(sec.sh_addralign);
  bswap(sec.sh_entsize);
  bswap(sec.sh_flags);
  bswap(sec.sh_info);
  bswap(sec.sh_link);
  bswap(sec.sh_name);
  bswap(sec.sh_offset);
  bswap(sec.sh_size);
  bswap(sec.sh_type);
}

static bool IsRangeValid(size_t offset, size_t size, size_t container_size)
{
  return offset <= container_size && size <= container_size - offset;
}

ElfReader::ElfReader(std::vector<u8> buffer) : BootExecutableReader(std::move(buffer))
{
  m_is_valid = Initialize();
}

ElfReader::ElfReader(File::IOFile file) : BootExecutableReader(std::move(file))
{
  m_is_valid = Initialize();
}

ElfReader::ElfReader(const std::string& filename) : BootExecutableReader(filename)
{
  m_is_valid = Initialize();
}

ElfReader::~ElfReader() = default;

bool ElfReader::Initialize()
{
  if (m_bytes.size() < sizeof(Elf32_Ehdr))
    return false;

  base = reinterpret_cast<char*>(m_bytes.data());
  header = reinterpret_cast<Elf32_Ehdr*>(m_bytes.data());

  const u8* ident = header->e_ident;
  if (ident[EI_MAG0] != ELFMAG0 || ident[EI_MAG1] != ELFMAG1 || ident[EI_MAG2] != ELFMAG2 ||
      ident[EI_MAG3] != ELFMAG3 || ident[EI_CLASS] != ELFCLASS32 || ident[EI_DATA] != ELFDATA2MSB ||
      ident[EI_VERSION] != EV_CURRENT)
  {
    header = nullptr;
    return false;
  }

  byteswapHeader(*header);

  if (header->e_version != EV_CURRENT || header->e_ehsize != sizeof(Elf32_Ehdr) ||
      (header->e_phnum != 0 && header->e_phentsize != sizeof(Elf32_Phdr)) ||
      (header->e_shnum != 0 && header->e_shentsize != sizeof(Elf32_Shdr)) ||
      !IsRangeValid(header->e_phoff, static_cast<size_t>(header->e_phnum) * sizeof(Elf32_Phdr),
                    m_bytes.size()) ||
      !IsRangeValid(header->e_shoff, static_cast<size_t>(header->e_shnum) * sizeof(Elf32_Shdr),
                    m_bytes.size()) ||
      (header->e_shnum == 0 && header->e_shstrndx != SHN_UNDEF) ||
      (header->e_shstrndx != SHN_UNDEF && header->e_shstrndx >= header->e_shnum))
  {
    header = nullptr;
    return false;
  }

  segments = reinterpret_cast<Elf32_Phdr*>(base + header->e_phoff);
  sections = reinterpret_cast<Elf32_Shdr*>(base + header->e_shoff);

  for (int i = 0; i < GetNumSegments(); i++)
  {
    byteswapSegment(segments[i]);
    const Elf32_Phdr& segment = segments[i];
    if (!IsRangeValid(segment.p_offset, segment.p_filesz, m_bytes.size()) ||
        (segment.p_type == PT_LOAD && segment.p_filesz > segment.p_memsz))
    {
      header = nullptr;
      segments = nullptr;
      sections = nullptr;
      return false;
    }
  }

  for (int i = 0; i < GetNumSections(); i++)
  {
    byteswapSection(sections[i]);
    const Elf32_Shdr& section = sections[i];
    if (section.sh_type != SHT_NOBITS &&
        !IsRangeValid(section.sh_offset, section.sh_size, m_bytes.size()))
    {
      header = nullptr;
      segments = nullptr;
      sections = nullptr;
      return false;
    }
  }
  entryPoint = header->e_entry;

  bRelocate = (header->e_type != ET_EXEC);
  return true;
}

const char* ElfReader::GetSectionName(int section) const
{
  if (!m_is_valid || section < 0 || section >= GetNumSections() || header->e_shstrndx == SHN_UNDEF)
  {
    return nullptr;
  }

  if (sections[section].sh_type == SHT_NULL)
    return nullptr;

  const u32 name_offset = sections[section].sh_name;
  const Elf32_Shdr& string_section = sections[header->e_shstrndx];
  const char* ptr = reinterpret_cast<const char*>(GetSectionDataPtr(header->e_shstrndx));

  if (!ptr || name_offset >= string_section.sh_size ||
      !std::memchr(ptr + name_offset, '\0', string_section.sh_size - name_offset))
  {
    return nullptr;
  }
  return ptr + name_offset;
}

const u8* ElfReader::GetSectionDataPtr(int section) const
{
  if (!m_is_valid || section < 0 || section >= GetNumSections() ||
      sections[section].sh_type == SHT_NOBITS)
  {
    return nullptr;
  }
  return GetPtr(sections[section].sh_offset);
}

bool ElfReader::IsCodeSegment(int segment) const
{
  return m_is_valid && segment >= 0 && segment < GetNumSegments() &&
         (segments[segment].p_flags & PF_X) != 0;
}

const u8* ElfReader::GetSegmentPtr(int segment) const
{
  if (!m_is_valid || segment < 0 || segment >= GetNumSegments())
    return nullptr;
  return GetPtr(segments[segment].p_offset);
}

u32 ElfReader::GetSegmentSize(int segment) const
{
  if (!m_is_valid || segment < 0 || segment >= GetNumSegments())
    return 0;
  return segments[segment].p_filesz;
}

u32 ElfReader::GetSectionSize(SectionID section) const
{
  if (!m_is_valid || section < 0 || section >= GetNumSections())
    return 0;
  return sections[section].sh_size;
}

bool ElfReader::IsPPCExecutable() const
{
  if (!m_is_valid || GetType() != ET_EXEC || GetMachine() != EM_PPC)
    return false;

  for (int i = 0; i < GetNumSegments(); ++i)
  {
    const Elf32_Phdr& segment = segments[i];
    const u64 end = static_cast<u64>(segment.p_vaddr) + segment.p_filesz;
    if (segment.p_type == PT_LOAD && (segment.p_flags & PF_X) != 0 && segment.p_filesz != 0 &&
        entryPoint >= segment.p_vaddr && entryPoint < end)
    {
      return true;
    }
  }
  return false;
}

// This is just a simple elf loader, good enough to load elfs generated by devkitPPC
bool ElfReader::LoadIntoMemory(Core::System& system, bool only_in_mem1) const
{
  if (!m_is_valid)
    return false;

  INFO_LOG_FMT(BOOT, "String section: {}", header->e_shstrndx);

  if (bRelocate)
  {
    PanicAlertFmt("Error: Dolphin doesn't know how to load a relocatable elf.");
    return false;
  }

  INFO_LOG_FMT(BOOT, "{} segments:", header->e_phnum);

  auto& memory = system.GetMemory();

  // Validate every destination before copying so a bad segment cannot leave a partial image.
  for (int i = 0; i < GetNumSegments(); ++i)
  {
    const Elf32_Phdr& segment = segments[i];
    if (segment.p_type != PT_LOAD || segment.p_memsz == 0)
      continue;

    const u64 physical_address = segment.p_vaddr & 0x3fffffff;
    if (only_in_mem1)
    {
      if (physical_address + segment.p_memsz > memory.GetRamSizeReal())
        continue;
    }
    else if (memory.GetSpanForAddress(segment.p_vaddr).size() < segment.p_memsz)
    {
      ERROR_LOG_FMT(BOOT, "ELF segment at {:08x} does not fit in emulated memory", segment.p_vaddr);
      return false;
    }
  }

  // Copy segments into RAM.
  for (int i = 0; i < header->e_phnum; i++)
  {
    Elf32_Phdr* p = segments + i;

    INFO_LOG_FMT(BOOT, "Type: {} Vaddr: {:08x} Paddr: {:08x} Filesz: {} Memsz: {}", p->p_type,
                 p->p_vaddr, p->p_paddr, p->p_filesz, p->p_memsz);

    if (p->p_type == PT_LOAD)
    {
      // Check LMA (paddr) first - some are nonsense, so fall back to VMA (vaddr) if invalid
      u32 writeAddr = p->p_paddr;
      if (writeAddr)
        writeAddr |= 0x80000000;  // map to virtual address
      else
        writeAddr = p->p_vaddr;  // LMA is empty, fall back to VMA

      const u8* src = GetSegmentPtr(i);
      u32 srcSize = p->p_filesz;
      u32 dstSize = p->p_memsz;

      const u64 physical_address = writeAddr & 0x3fffffff;
      if (only_in_mem1 && physical_address + p->p_memsz > memory.GetRamSizeReal())
        continue;

      memory.CopyToEmu(writeAddr, src, srcSize);
      if (srcSize < dstSize)
        memory.Memset(writeAddr + srcSize, 0, dstSize - srcSize);  // zero out bss

      INFO_LOG_FMT(BOOT, "Loadable Segment Copied to {:08x}, size {:08x}", writeAddr, p->p_memsz);
    }
  }

  INFO_LOG_FMT(BOOT, "Done loading.");
  return true;
}

SectionID ElfReader::GetSectionByName(const char* name, int firstSection) const
{
  if (!m_is_valid || !name)
    return -1;

  for (int i = firstSection; i < header->e_shnum; i++)
  {
    const char* secname = GetSectionName(i);

    if (secname != nullptr && strcmp(name, secname) == 0)
      return i;
  }
  return -1;
}

bool ElfReader::LoadSymbols(const Core::CPUThreadGuard& guard, PPCSymbolDB& ppc_symbol_db,
                            const std::string& filename) const
{
  if (!m_is_valid || bRelocate)
    return false;

  size_t loaded_symbols = 0;
  SectionID sec = GetSectionByName(".symtab");
  if (sec != -1)
  {
    const u8* symtab = GetSectionDataPtr(sec);
    const u32 string_section = sections[sec].sh_link;
    const char* string_base = string_section < static_cast<u32>(GetNumSections()) ?
                                  reinterpret_cast<const char*>(GetSectionDataPtr(string_section)) :
                                  nullptr;
    if (sections[sec].sh_type == SHT_SYMTAB && symtab && string_base &&
        sections[sec].sh_size % sizeof(Elf32_Sym) == 0)
    {
      const size_t string_size = sections[string_section].sh_size;
      const u32 num_symbols = sections[sec].sh_size / sizeof(Elf32_Sym);
      for (u32 sym = 0; sym < num_symbols; ++sym)
      {
        Elf32_Sym symbol;
        std::memcpy(&symbol, symtab + sym * sizeof(Elf32_Sym), sizeof(symbol));
        const u32 size = Common::swap32(symbol.st_size);
        if (size == 0)
          continue;

        const int type = symbol.st_info & 0xF;
        const u32 value = Common::swap32(symbol.st_value);
        const u32 name_offset = Common::swap32(symbol.st_name);
        if (name_offset >= string_size ||
            !std::memchr(string_base + name_offset, '\0', string_size - name_offset))
        {
          continue;
        }
        const char* name = string_base + name_offset;

        auto symtype = Common::Symbol::Type::Data;
        switch (type)
        {
        case STT_OBJECT:
          symtype = Common::Symbol::Type::Data;
          break;
        case STT_FUNC:
          symtype = Common::Symbol::Type::Function;
          break;
        default:
          continue;
        }
        ppc_symbol_db.AddKnownSymbol(guard, value, size, name, filename, symtype);
        ++loaded_symbols;
      }
    }
  }
  ppc_symbol_db.Index();
  NOTICE_LOG_FMT(SYMBOLS, "{} symbols loaded from ELF file '{}'.", loaded_symbols, filename);

  bool dwarf_loaded = false;
  const SectionID debug_section = GetSectionByName(".debug");
  if (debug_section >= 0)
  {
    const u8* debug_data = GetSectionDataPtr(debug_section);
    const SectionID line_section = GetSectionByName(".line");
    const u8* line_data = line_section >= 0 ? GetSectionDataPtr(line_section) : nullptr;
    if (debug_data)
    {
      const size_t debug_size = GetSectionSize(debug_section);
      const size_t line_size = line_data ? GetSectionSize(line_section) : 0;
      dwarf_loaded = Core::Debug::ImportDwarf(
          guard, ppc_symbol_db, {debug_data, debug_size},
          line_data ? std::span<const u8>{line_data, line_size} : std::span<const u8>{}, filename);
    }
  }

  return loaded_symbols != 0 || dwarf_loaded;
}

bool ElfReader::IsWii() const
{
  if (!m_is_valid)
    return false;

  // Use the same method as the DOL loader uses: search for mfspr from HID4,
  // which should only be used in Wii ELFs.
  //
  // Likely to have some false positives/negatives, patches implementing a
  // better heuristic are welcome.

  // Swap these once, instead of swapping every word in the file.
  u32 HID4_pattern = Common::swap32(0x7c13fba6);
  u32 HID4_mask = Common::swap32(0xfc1fffff);

  for (int i = 0; i < GetNumSegments(); ++i)
  {
    if (IsCodeSegment(i))
    {
      const u8* code = GetSegmentPtr(i);
      for (u32 j = 0; j < GetSegmentSize(i) / sizeof(u32); ++j)
      {
        u32 instruction;
        std::memcpy(&instruction, code + j * sizeof(u32), sizeof(instruction));
        if ((instruction & HID4_mask) == HID4_pattern)
          return true;
      }
    }
  }

  return false;
}

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Boot/ElfReader.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "../Debugger/DWARF/DwarfTestFixture.h"

namespace
{
constexpr size_t ELF_HEADER_OFFSET = 0;
constexpr size_t PROGRAM_HEADER_OFFSET = sizeof(Elf32_Ehdr);
constexpr size_t TEXT_OFFSET = 0x100;
constexpr std::array<u8, 8> TEXT = {0x60, 0x00, 0x00, 0x00, 0x4e, 0x80, 0x00, 0x20};
constexpr std::string_view SECTION_NAMES{"\0.text\0.debug\0.line\0.shstrtab\0.symtab\0", 38};
constexpr size_t DEBUG_OFFSET = TEXT_OFFSET + TEXT.size();
constexpr size_t LINE_OFFSET = DEBUG_OFFSET + DwarfTestFixture::kDebugSection.size();
constexpr size_t NAMES_OFFSET = LINE_OFFSET + DwarfTestFixture::kLineSection.size();
constexpr size_t SECTION_HEADERS_OFFSET = (NAMES_OFFSET + SECTION_NAMES.size() + 3) & ~size_t{3};

void Write16(std::vector<u8>& bytes, size_t offset, u16 value)
{
  bytes[offset] = static_cast<u8>(value >> 8);
  bytes[offset + 1] = static_cast<u8>(value);
}

void Write32(std::vector<u8>& bytes, size_t offset, u32 value)
{
  bytes[offset] = static_cast<u8>(value >> 24);
  bytes[offset + 1] = static_cast<u8>(value >> 16);
  bytes[offset + 2] = static_cast<u8>(value >> 8);
  bytes[offset + 3] = static_cast<u8>(value);
}

void WriteSection(std::vector<u8>& bytes, size_t offset, u32 name, u32 type, u32 flags, u32 address,
                  u32 file_offset, u32 size, u32 alignment)
{
  Write32(bytes, offset, name);
  Write32(bytes, offset + 4, type);
  Write32(bytes, offset + 8, flags);
  Write32(bytes, offset + 12, address);
  Write32(bytes, offset + 16, file_offset);
  Write32(bytes, offset + 20, size);
  Write32(bytes, offset + 32, alignment);
}

std::vector<u8> MakeElf(ElfMachine machine = EM_PPC)
{
  std::vector<u8> bytes(SECTION_HEADERS_OFFSET + 6 * sizeof(Elf32_Shdr));

  bytes[EI_MAG0] = ELFMAG0;
  bytes[EI_MAG1] = ELFMAG1;
  bytes[EI_MAG2] = ELFMAG2;
  bytes[EI_MAG3] = ELFMAG3;
  bytes[EI_CLASS] = ELFCLASS32;
  bytes[EI_DATA] = ELFDATA2MSB;
  bytes[EI_VERSION] = EV_CURRENT;
  Write16(bytes, ELF_HEADER_OFFSET + 16, ET_EXEC);
  Write16(bytes, ELF_HEADER_OFFSET + 18, machine);
  Write32(bytes, ELF_HEADER_OFFSET + 20, EV_CURRENT);
  Write32(bytes, ELF_HEADER_OFFSET + 24, DwarfTestFixture::kFunctionAddress);
  Write32(bytes, ELF_HEADER_OFFSET + 28, PROGRAM_HEADER_OFFSET);
  Write32(bytes, ELF_HEADER_OFFSET + 32, static_cast<u32>(SECTION_HEADERS_OFFSET));
  Write16(bytes, ELF_HEADER_OFFSET + 40, sizeof(Elf32_Ehdr));
  Write16(bytes, ELF_HEADER_OFFSET + 42, sizeof(Elf32_Phdr));
  Write16(bytes, ELF_HEADER_OFFSET + 44, 1);
  Write16(bytes, ELF_HEADER_OFFSET + 46, sizeof(Elf32_Shdr));
  Write16(bytes, ELF_HEADER_OFFSET + 48, 6);
  Write16(bytes, ELF_HEADER_OFFSET + 50, 4);

  Write32(bytes, PROGRAM_HEADER_OFFSET, PT_LOAD);
  Write32(bytes, PROGRAM_HEADER_OFFSET + 4, TEXT_OFFSET);
  Write32(bytes, PROGRAM_HEADER_OFFSET + 8, DwarfTestFixture::kFunctionAddress);
  Write32(bytes, PROGRAM_HEADER_OFFSET + 12, DwarfTestFixture::kFunctionAddress);
  Write32(bytes, PROGRAM_HEADER_OFFSET + 16, TEXT.size());
  Write32(bytes, PROGRAM_HEADER_OFFSET + 20, 12);
  Write32(bytes, PROGRAM_HEADER_OFFSET + 24, PF_R | PF_X);
  Write32(bytes, PROGRAM_HEADER_OFFSET + 28, 4);

  std::ranges::copy(TEXT, bytes.begin() + TEXT_OFFSET);
  std::ranges::copy(DwarfTestFixture::kDebugSection, bytes.begin() + DEBUG_OFFSET);
  std::ranges::copy(DwarfTestFixture::kLineSection, bytes.begin() + LINE_OFFSET);
  std::ranges::copy(SECTION_NAMES, bytes.begin() + NAMES_OFFSET);

  WriteSection(bytes, SECTION_HEADERS_OFFSET + sizeof(Elf32_Shdr), 1, SHT_PROGBITS,
               SHF_ALLOC | SHF_EXECINSTR, DwarfTestFixture::kFunctionAddress, TEXT_OFFSET,
               TEXT.size(), 4);
  WriteSection(bytes, SECTION_HEADERS_OFFSET + 2 * sizeof(Elf32_Shdr), 7, SHT_PROGBITS, 0, 0,
               DEBUG_OFFSET, DwarfTestFixture::kDebugSection.size(), 1);
  WriteSection(bytes, SECTION_HEADERS_OFFSET + 3 * sizeof(Elf32_Shdr), 14, SHT_PROGBITS, 0, 0,
               LINE_OFFSET, DwarfTestFixture::kLineSection.size(), 1);
  WriteSection(bytes, SECTION_HEADERS_OFFSET + 4 * sizeof(Elf32_Shdr), 20, SHT_STRTAB, 0, 0,
               NAMES_OFFSET, SECTION_NAMES.size(), 1);
  const size_t symtab_header = SECTION_HEADERS_OFFSET + 5 * sizeof(Elf32_Shdr);
  WriteSection(bytes, symtab_header, 30, SHT_SYMTAB, 0, 0, 0, 0, 4);
  Write32(bytes, symtab_header + 24, 4);
  Write32(bytes, symtab_header + 36, sizeof(Elf32_Sym));
  return bytes;
}

class ElfReaderTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    auto& system = Core::System::GetInstance();
    system.GetMemory().Init();
    AddressSpace::Init();
    system.GetCoreTiming().Init();
    Core::DeclareAsCPUThread();
    system.GetCPU().Init(PowerPC::CPUCore::Interpreter);
    system.GetPPCSymbolDB().Clear();
  }

  void TearDown() override
  {
    auto& system = Core::System::GetInstance();
    system.GetCPU().Shutdown();
    AddressSpace::Shutdown();
    system.GetMemory().Shutdown();
    system.GetCoreTiming().Shutdown();
    Core::UndeclareAsCPUThread();
  }
};

TEST_F(ElfReaderTest, LoadsExecutableSegmentAndZeroFillsBss)
{
  ElfReader reader(MakeElf());
  ASSERT_TRUE(reader.IsValid());
  ASSERT_TRUE(reader.IsPPCExecutable());
  EXPECT_EQ(reader.GetEntryPoint(), DwarfTestFixture::kFunctionAddress);

  auto& system = Core::System::GetInstance();
  system.GetMemory().Memset(DwarfTestFixture::kFunctionAddress, 0xff, 12);
  ASSERT_TRUE(reader.LoadIntoMemory(system));

  std::array<u8, 12> loaded;
  system.GetMemory().CopyFromEmu(loaded.data(), DwarfTestFixture::kFunctionAddress, loaded.size());
  EXPECT_TRUE(std::equal(TEXT.begin(), TEXT.end(), loaded.begin()));
  EXPECT_TRUE(
      std::all_of(loaded.begin() + TEXT.size(), loaded.end(), [](u8 value) { return value == 0; }));
}

TEST_F(ElfReaderTest, ImportsEmbeddedDwarf)
{
  ElfReader reader(MakeElf());
  auto& system = Core::System::GetInstance();
  system.GetMemory().Memset(DwarfTestFixture::kFunctionAddress, 0x5a, 12);
  system.GetPPCState().pc = 0x80001000;

  Core::CPUThreadGuard guard(system);
  auto& symbols = system.GetPPCSymbolDB();
  ASSERT_TRUE(reader.LoadSymbols(guard, symbols, "fixture.elf"));
  const auto line = symbols.GetSourceLine(DwarfTestFixture::kLineTwoAddress);
  ASSERT_TRUE(line);
  EXPECT_EQ(line->file, DwarfTestFixture::kCompileUnitName);
  EXPECT_EQ(line->line, 2u);

  std::array<u8, 12> memory;
  system.GetMemory().CopyFromEmu(memory.data(), DwarfTestFixture::kFunctionAddress, memory.size());
  EXPECT_TRUE(std::ranges::all_of(memory, [](u8 value) { return value == 0x5a; }));
  EXPECT_EQ(system.GetPPCState().pc, 0x80001000u);
}

TEST_F(ElfReaderTest, MalformedOptionalSymbolTableDoesNotSuppressEmbeddedDwarf)
{
  std::vector<u8> bytes = MakeElf();
  const size_t symtab_header = SECTION_HEADERS_OFFSET + 5 * sizeof(Elf32_Shdr);
  Write32(bytes, symtab_header + 4, SHT_NOBITS);
  Write32(bytes, symtab_header + 20, sizeof(Elf32_Sym));
  ElfReader reader(std::move(bytes));

  auto& system = Core::System::GetInstance();
  ASSERT_TRUE(reader.LoadIntoMemory(system));
  Core::CPUThreadGuard guard(system);
  ASSERT_TRUE(reader.LoadSymbols(guard, system.GetPPCSymbolDB(), "fixture.elf"));
  EXPECT_TRUE(system.GetPPCSymbolDB().GetSourceLine(DwarfTestFixture::kLineTwoAddress));
}

TEST_F(ElfReaderTest, LoadsStructurallyValidArmElfForIOS)
{
  ElfReader reader(MakeElf(EM_ARM));
  ASSERT_TRUE(reader.IsValid());
  EXPECT_FALSE(reader.IsPPCExecutable());
  EXPECT_TRUE(reader.LoadIntoMemory(Core::System::GetInstance(), true));
}

TEST(ElfReaderValidationTest, RejectsMalformedAndNonPPCExecutables)
{
  std::vector<u8> malformed(sizeof(Elf32_Ehdr));
  EXPECT_FALSE(ElfReader(std::move(malformed)).IsValid());

  ElfReader reader(MakeElf(EM_ARM));
  EXPECT_TRUE(reader.IsValid());
  EXPECT_FALSE(reader.IsPPCExecutable());
}

TEST(ElfReaderValidationTest, RejectsWrongEncodingAndInvalidLoadSizes)
{
  std::vector<u8> wrong_encoding = MakeElf();
  wrong_encoding[EI_DATA] = ELFDATA2LSB;
  EXPECT_FALSE(ElfReader(std::move(wrong_encoding)).IsValid());

  std::vector<u8> oversized_file_data = MakeElf();
  Write32(oversized_file_data, PROGRAM_HEADER_OFFSET + 16, 13);
  EXPECT_FALSE(ElfReader(std::move(oversized_file_data)).IsValid());
}

TEST(ElfReaderValidationTest, RejectsEntryPointInZeroFillTail)
{
  std::vector<u8> bytes = MakeElf();
  Write32(bytes, ELF_HEADER_OFFSET + 24,
          DwarfTestFixture::kFunctionAddress + static_cast<u32>(TEXT.size()));
  ElfReader reader(std::move(bytes));
  EXPECT_TRUE(reader.IsValid());
  EXPECT_FALSE(reader.IsPPCExecutable());
}
}  // namespace

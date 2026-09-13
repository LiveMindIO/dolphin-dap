// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/SymbolDB.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/DWARF/DwarfImport.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DwarfTestFixture.h"

namespace
{
class PPCSymbolDBLineTest : public ::testing::Test
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

  static PPCSymbolDB& SymbolDB() { return Core::System::GetInstance().GetPPCSymbolDB(); }
};

TEST_F(PPCSymbolDBLineTest, GetSourceLineUsesNearestPrecedingEntry)
{
  const u32 file_index = SymbolDB().AddSourceFile("foo.c");
  SymbolDB().AddLineEntry(0x00004100, file_index, 1);
  SymbolDB().AddLineEntry(0x00004108, file_index, 3);

  const std::optional<PPCSymbolDB::SourceLine> line = SymbolDB().GetSourceLine(0x00004104);
  ASSERT_TRUE(line);
  EXPECT_EQ(line->file, "foo.c");
  EXPECT_EQ(line->line, 1U);
  EXPECT_EQ(line->address, 0x00004100U);
}

TEST_F(PPCSymbolDBLineTest, GetLineAddressFindsExactLine)
{
  const u32 file_index = SymbolDB().AddSourceFile("bar.c");
  SymbolDB().AddLineEntry(0x80002000, file_index, 10);

  const std::optional<u32> address = SymbolDB().GetLineAddress("bar.c", 10);
  ASSERT_TRUE(address);
  EXPECT_EQ(*address, 0x80002000U);
}

TEST_F(PPCSymbolDBLineTest, ClearRemovesSourceLineInfo)
{
  SymbolDB().AddSourceFile("baz.c");
  SymbolDB().AddLineEntry(0x80003000, 0, 1);
  ASSERT_TRUE(SymbolDB().HasSourceLineInfo());
  SymbolDB().Clear();
  EXPECT_FALSE(SymbolDB().HasSourceLineInfo());
}

TEST_F(PPCSymbolDBLineTest, MalformedMapPreservesExistingSymbols)
{
  constexpr u32 function_address = 0x00004100;
  const std::array<u8, 8> code{{0x60, 0x00, 0x00, 0x00, 0x4e, 0x80, 0x00, 0x20}};
  Core::System::GetInstance().GetMemory().CopyToEmu(function_address, code.data(), code.size());
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  SymbolDB().AddKnownSymbol(guard, function_address, code.size(), "existing", "existing.o");

  const std::string temp_dir = File::CreateTempDir();
  ASSERT_FALSE(temp_dir.empty());
  const std::string map_path = temp_dir + "/malformed.map";
  ASSERT_TRUE(File::WriteStringToFile(map_path, "not a symbol map\n"));

  EXPECT_FALSE(SymbolDB().LoadMap(guard, map_path));
  ASSERT_NE(SymbolDB().GetSymbolFromAddr(function_address), nullptr);
  EXPECT_EQ(SymbolDB().GetSymbolFromAddr(function_address)->name, "existing");

  File::DeleteDirRecursively(temp_dir);
}

TEST_F(PPCSymbolDBLineTest, DwarfDebugInfoUsesSharedImmutableStorage)
{
  SymbolDB().SetDwarfDebugInfo(DwarfTestFixture::MakeTypedParseResult());

  const auto first = SymbolDB().GetDwarfDebugInfo();
  const auto second = SymbolDB().GetDwarfDebugInfo();
  ASSERT_TRUE(first);
  EXPECT_EQ(first.get(), second.get());
}

TEST_F(PPCSymbolDBLineTest, ImportDwarfPopulatesLineTable)
{
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  ASSERT_TRUE(Core::Debug::ImportDwarf(guard, SymbolDB(), DwarfTestFixture::kDebugSection,
                                       DwarfTestFixture::kLineSection));
  EXPECT_TRUE(SymbolDB().HasSourceLineInfo());
  const std::optional<PPCSymbolDB::SourceLine> line =
      SymbolDB().GetSourceLine(DwarfTestFixture::kLineTwoAddress);
  ASSERT_TRUE(line);
  EXPECT_EQ(line->file, DwarfTestFixture::kCompileUnitName);
  EXPECT_EQ(line->line, 2U);
}

TEST_F(PPCSymbolDBLineTest, GetSourceLineReturnsNulloptBeforeFirstEntry)
{
  const u32 file_index = SymbolDB().AddSourceFile("foo.c");
  SymbolDB().AddLineEntry(0x00004100, file_index, 1);
  EXPECT_FALSE(SymbolDB().GetSourceLine(0x00004000).has_value());
}

TEST_F(PPCSymbolDBLineTest, GetSourceLineAtExactEntryAddress)
{
  const u32 file_index = SymbolDB().AddSourceFile("foo.c");
  SymbolDB().AddLineEntry(0x00004100, file_index, 5);
  const std::optional<PPCSymbolDB::SourceLine> line = SymbolDB().GetSourceLine(0x00004100);
  ASSERT_TRUE(line);
  EXPECT_EQ(line->line, 5U);
  EXPECT_EQ(line->address, 0x00004100U);
}

TEST_F(PPCSymbolDBLineTest, GetSourceLineDoesNotEscapeContainingFunction)
{
  constexpr u32 first_function = 0x00004100;
  constexpr u32 source_less_function = 0x00004200;
  const u32 file_index = SymbolDB().AddSourceFile("foo.c");
  SymbolDB().AddLineEntry(first_function, file_index, 5);
  const std::array<u8, 8> code{{0x60, 0x00, 0x00, 0x00, 0x4e, 0x80, 0x00, 0x20}};
  Core::System::GetInstance().GetMemory().CopyToEmu(first_function, code.data(), code.size());
  Core::System::GetInstance().GetMemory().CopyToEmu(source_less_function, code.data(), code.size());
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  SymbolDB().AddKnownSymbol(guard, first_function, code.size(), "with_source", "foo.c");
  SymbolDB().AddKnownSymbol(guard, source_less_function, code.size(), "without_source", "asm.o");

  EXPECT_TRUE(SymbolDB().GetSourceLine(first_function + 4).has_value());
  EXPECT_FALSE(SymbolDB().GetSourceLine(source_less_function).has_value());
}

TEST_F(PPCSymbolDBLineTest, GetLineAddressReturnsNearestPrecedingLine)
{
  const u32 file_index = SymbolDB().AddSourceFile("foo.c");
  SymbolDB().AddLineEntry(0x00004100, file_index, 10);
  SymbolDB().AddLineEntry(0x00004110, file_index, 20);

  const std::optional<u32> address = SymbolDB().GetLineAddress("foo.c", 15);
  ASSERT_TRUE(address);
  EXPECT_EQ(*address, 0x00004100U);
}

TEST_F(PPCSymbolDBLineTest, GetLineAddressReturnsNulloptWhenLineBeforeFirstEntry)
{
  const u32 file_index = SymbolDB().AddSourceFile("foo.c");
  SymbolDB().AddLineEntry(0x00004100, file_index, 10);
  EXPECT_FALSE(SymbolDB().GetLineAddress("foo.c", 5).has_value());
}

TEST_F(PPCSymbolDBLineTest, GetLineAddressReturnsNulloptForUnknownFile)
{
  const u32 file_index = SymbolDB().AddSourceFile("foo.c");
  SymbolDB().AddLineEntry(0x00004100, file_index, 1);
  EXPECT_FALSE(SymbolDB().GetLineAddress("missing.c", 1).has_value());
}

TEST_F(PPCSymbolDBLineTest, ExactLineAddressesAreOrderedAndNeverUseNearestLine)
{
  const u32 first_file = SymbolDB().AddSourceFile("first.c");
  const u32 second_file = SymbolDB().AddSourceFile("second.c");
  SymbolDB().AddLineEntry(0x80001008, first_file, 12);
  SymbolDB().AddLineEntry(0x80001000, first_file, 12);
  SymbolDB().AddLineEntry(0x80001010, first_file, 20);
  SymbolDB().AddLineEntry(0x80002000, second_file, 12);

  const std::map<u32, std::vector<u32>> lines = SymbolDB().GetExactLineAddresses(first_file);
  EXPECT_EQ(lines.size(), 2U);
  EXPECT_EQ(lines.at(12), (std::vector<u32>{0x80001000U, 0x80001008U}));
  EXPECT_EQ(lines.at(20), (std::vector<u32>{0x80001010U}));
  EXPECT_FALSE(lines.contains(13));
  EXPECT_TRUE(SymbolDB().GetExactLineAddresses(99).empty());
}

TEST_F(PPCSymbolDBLineTest, GetLineAddressForQueryMatchesFullEditorPath)
{
  const u32 file_index = SymbolDB().AddSourceFile("gm_16AE.c");
  SymbolDB().AddLineEntry(0x80012340, file_index, 1126);

  const std::optional<u32> address =
      SymbolDB().GetLineAddressForQuery("/home/dev/melee/src/melee/gm/gm_16AE.c", 1126);
  ASSERT_TRUE(address);
  EXPECT_EQ(*address, 0x80012340U);
}

TEST_F(PPCSymbolDBLineTest, AddSourceFileDeduplicatesPaths)
{
  const u32 first = SymbolDB().AddSourceFile("foo.c");
  const u32 second = SymbolDB().AddSourceFile("foo.c");
  EXPECT_EQ(first, second);
  EXPECT_EQ(SymbolDB().GetSourceFiles().size(), 1U);
}

TEST_F(PPCSymbolDBLineTest, GetSourceFilesReturnsStableSnapshot)
{
  SymbolDB().AddSourceFile("first.c");
  const std::vector<std::string> snapshot = SymbolDB().GetSourceFiles();
  SymbolDB().AddSourceFile("second.c");

  ASSERT_EQ(snapshot.size(), 1U);
  EXPECT_EQ(snapshot[0], "first.c");
  EXPECT_EQ(SymbolDB().GetSourceFiles().size(), 2U);
}

TEST_F(PPCSymbolDBLineTest, DuplicateSourceFileInstancesRemainDistinctAndAmbiguousByName)
{
  const u32 first = SymbolDB().AddSourceFileInstance("foo.c");
  const u32 second = SymbolDB().AddSourceFileInstance("foo.c");
  SymbolDB().AddLineEntry(0x80001000, first, 7);
  SymbolDB().AddLineEntry(0x80002000, second, 7);

  EXPECT_NE(first, second);
  EXPECT_FALSE(SymbolDB().FindSourceFileIndex("foo.c"));
  EXPECT_FALSE(SymbolDB().GetLineAddressForQuery("/workspace/src/foo.c", 7));
  EXPECT_EQ(SymbolDB().GetLineAddress(first, 7), 0x80001000U);
  EXPECT_EQ(SymbolDB().GetLineAddress(second, 7), 0x80002000U);
  ASSERT_TRUE(SymbolDB().GetSourceLine(0x80002000));
  EXPECT_EQ(SymbolDB().GetSourceLine(0x80002000)->file_index, second);
}

TEST_F(PPCSymbolDBLineTest, SourceFileLookupPrefersUniqueQualifiedSuffix)
{
  const u32 first = SymbolDB().AddSourceFileInstance("first/other/foo.c");
  const u32 second = SymbolDB().AddSourceFileInstance("second/src/foo.c");
  SymbolDB().AddLineEntry(0x80001000, first, 7);
  SymbolDB().AddLineEntry(0x80002000, second, 7);

  EXPECT_FALSE(SymbolDB().FindSourceFileIndex("foo.c"));
  EXPECT_EQ(SymbolDB().FindSourceFileIndex("/workspace/src/foo.c"), second);
  EXPECT_EQ(SymbolDB().FindSourceFileIndex("C:\\workspace\\src\\foo.c"), second);
  EXPECT_EQ(SymbolDB().GetLineAddressForQuery("/workspace/src/foo.c", 7), 0x80002000U);
}

TEST_F(PPCSymbolDBLineTest, ImportDwarfReturnsFalseForEmptyDebugSection)
{
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  const std::vector<u8> empty;
  EXPECT_FALSE(Core::Debug::ImportDwarf(guard, SymbolDB(), empty, empty));
}

TEST_F(PPCSymbolDBLineTest, ImportDwarfAddsFunctionSymbol)
{
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  ASSERT_TRUE(Core::Debug::ImportDwarf(guard, SymbolDB(), DwarfTestFixture::kDebugSection,
                                       DwarfTestFixture::kLineSection));
  const Common::Symbol* symbol = SymbolDB().GetSymbolFromAddr(DwarfTestFixture::kFunctionAddress);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(symbol->name, DwarfTestFixture::kFunctionName);
}

TEST_F(PPCSymbolDBLineTest, ImportDwarfPreservesDuplicateCompileUnitOccurrences)
{
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  const std::vector<u8> debug = DwarfTestFixture::MakeDuplicateCuNameDebugSection();
  ASSERT_TRUE(
      Core::Debug::ImportDwarf(guard, SymbolDB(), debug, DwarfTestFixture::kMultiCuLineSection));

  const auto& files = SymbolDB().GetSourceFiles();
  ASSERT_EQ(files.size(), 2U);
  EXPECT_EQ(files[0], DwarfTestFixture::kFirstCompileUnitName);
  EXPECT_EQ(files[1], DwarfTestFixture::kFirstCompileUnitName);
  ASSERT_TRUE(SymbolDB().GetSourceLine(DwarfTestFixture::kSecondFunctionAddress));
  EXPECT_EQ(SymbolDB().GetSourceLine(DwarfTestFixture::kSecondFunctionAddress)->file_index, 1U);
}
}  // namespace

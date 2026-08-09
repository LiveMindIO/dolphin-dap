// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/DWARF/DwarfImport.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Common/SymbolDB.h"
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
  const Common::Symbol* symbol =
      SymbolDB().GetSymbolFromAddr(DwarfTestFixture::kFunctionAddress);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(symbol->name, DwarfTestFixture::kFunctionName);
}
}  // namespace

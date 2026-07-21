// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>

#include <gtest/gtest.h>

#include "Core/Debugger/DWARF/DwarfReader.h"
#include "DwarfTestFixture.h"

namespace
{
TEST(DwarfReaderTest, ParseGoldenFixtureExtractsFunctionsAndLines)
{
  const std::optional<Core::Debug::Dwarf::ParseResult> result = Core::Debug::Dwarf::Parse(
      DwarfTestFixture::kDebugSection, DwarfTestFixture::kLineSection, true);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->files.size(), 1U);
  EXPECT_EQ(result->files[0], DwarfTestFixture::kCompileUnitName);
  ASSERT_EQ(result->functions.size(), 1U);
  EXPECT_EQ(result->functions[0].name, DwarfTestFixture::kFunctionName);
  EXPECT_EQ(result->functions[0].low_pc, DwarfTestFixture::kFunctionAddress);
  ASSERT_GE(result->lines.size(), 2U);
  EXPECT_EQ(result->lines[0].address, DwarfTestFixture::kFunctionAddress);
  EXPECT_EQ(result->lines[0].line, 1U);
  EXPECT_EQ(result->lines[1].address, DwarfTestFixture::kLineTwoAddress);
  EXPECT_EQ(result->lines[1].line, 2U);
}

TEST(DwarfReaderTest, ParseRejectsEmptyDebugSection)
{
  const std::vector<u8> empty;
  EXPECT_FALSE(Core::Debug::Dwarf::Parse(empty, DwarfTestFixture::kLineSection, true));
}

TEST(DwarfReaderTest, ParseRejectsTruncatedDebugSection)
{
  const std::vector<u8> truncated(DwarfTestFixture::kDebugSection.begin(),
                                  DwarfTestFixture::kDebugSection.begin() + 8);
  EXPECT_FALSE(Core::Debug::Dwarf::Parse(truncated, DwarfTestFixture::kLineSection, true));
}

TEST(DwarfReaderTest, ParseHandlesMissingLineSectionWithFunctionsOnly)
{
  const std::vector<u8> empty_line;
  const std::optional<Core::Debug::Dwarf::ParseResult> result =
      Core::Debug::Dwarf::Parse(DwarfTestFixture::kDebugSection, empty_line, true);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->functions.size(), 1U);
  EXPECT_TRUE(result->lines.empty());
}

TEST(DwarfReaderTest, ParseToleratesUnknownAttributeFormAndContinues)
{
  // DESNOTE(jbarber, 2026-07-21): An unknown DWARF 1.1 attribute form used
  // to abort the entire CU walk via `return false` -> caller `break`,
  // losing functions/line info from subsequent DIEs. The parser now logs a
  // warning, skips the rest of that DIE's attributes, and continues the CU
  // walk (advancing via the already-read length/sibling). Corrupting byte 7
  // (the CU's first attribute) yields an unknown form; the CU's own
  // name/stmt_list are lost, but the CU walk still descends into children
  // via `length` and the function DIE -- whose attributes are uncorrupted --
  // is still extracted.
  std::vector<u8> corrupted(DwarfTestFixture::kDebugSection.begin(),
                            DwarfTestFixture::kDebugSection.end());
  corrupted[7] = 0x3F;
  const std::optional<Core::Debug::Dwarf::ParseResult> result =
      Core::Debug::Dwarf::Parse(corrupted, DwarfTestFixture::kLineSection, true);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->functions.size(), 1U);
  EXPECT_EQ(result->functions[0].name, DwarfTestFixture::kFunctionName);
  EXPECT_EQ(result->functions[0].low_pc, DwarfTestFixture::kFunctionAddress);
  // The CU header's AT_name was unreadable (we bailed out of its attribute
  // loop early), so no file entry is recorded for it.
  EXPECT_TRUE(result->files.empty());
}

TEST(DwarfReaderTest, ParseIgnoresLineTableWhenStmtListOutOfBounds)
{
  std::vector<u8> short_line(4, 0);
  const std::optional<Core::Debug::Dwarf::ParseResult> result =
      Core::Debug::Dwarf::Parse(DwarfTestFixture::kDebugSection, short_line, true);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->functions.size(), 1U);
  EXPECT_TRUE(result->lines.empty());
}

TEST(DwarfReaderTest, ParseReturnsNulloptWhenNoFunctionsOrLines)
{
  const std::vector<u8> padding_only = {
      0x00, 0x00, 0x00, 0x06, 0x00, 0x00,
  };
  EXPECT_FALSE(Core::Debug::Dwarf::Parse(padding_only, {}, true));
}

TEST(DwarfReaderTest, ParseMultiCompileUnitSiblingChain)
{
  const std::optional<Core::Debug::Dwarf::ParseResult> result = Core::Debug::Dwarf::Parse(
      DwarfTestFixture::kMultiCuDebugSection, DwarfTestFixture::kMultiCuLineSection, true);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->files.size(), 2U);
  EXPECT_EQ(result->files[0], DwarfTestFixture::kFirstCompileUnitName);
  EXPECT_EQ(result->files[1], DwarfTestFixture::kSecondCompileUnitName);
  ASSERT_EQ(result->functions.size(), 2U);
  EXPECT_EQ(result->functions[0].name, DwarfTestFixture::kFirstFunctionName);
  EXPECT_EQ(result->functions[1].name, DwarfTestFixture::kSecondFunctionName);
  EXPECT_EQ(result->functions[1].low_pc, DwarfTestFixture::kSecondFunctionAddress);
  ASSERT_GE(result->lines.size(), 4U);
}
}  // namespace

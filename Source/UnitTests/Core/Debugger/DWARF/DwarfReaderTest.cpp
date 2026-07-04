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
}  // namespace

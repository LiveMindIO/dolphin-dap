// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <limits>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Debugger/DWARF/DwarfReader.h"
#include "DwarfTestFixture.h"

namespace
{
void AppendU16(std::vector<u8>* bytes, u16 value)
{
  bytes->push_back(static_cast<u8>(value >> 8));
  bytes->push_back(static_cast<u8>(value));
}

void AppendU32(std::vector<u8>* bytes, u32 value)
{
  bytes->push_back(static_cast<u8>(value >> 24));
  bytes->push_back(static_cast<u8>(value >> 16));
  bytes->push_back(static_cast<u8>(value >> 8));
  bytes->push_back(static_cast<u8>(value));
}

void PatchU32(std::vector<u8>* bytes, size_t offset, u32 value)
{
  (*bytes)[offset] = static_cast<u8>(value >> 24);
  (*bytes)[offset + 1] = static_cast<u8>(value >> 16);
  (*bytes)[offset + 2] = static_cast<u8>(value >> 8);
  (*bytes)[offset + 3] = static_cast<u8>(value);
}

void AppendString(std::vector<u8>* bytes, std::string_view value)
{
  bytes->insert(bytes->end(), value.begin(), value.end());
  bytes->push_back(0);
}

size_t BeginDie(std::vector<u8>* bytes, u16 tag)
{
  const size_t start = bytes->size();
  AppendU32(bytes, 0);
  AppendU16(bytes, tag);
  return start;
}

void FinishDie(std::vector<u8>* bytes, size_t start)
{
  PatchU32(bytes, start, static_cast<u32>(bytes->size() - start));
}

std::vector<u8> MakeTypedDebugSection()
{
  std::vector<u8> bytes;
  const size_t compile_unit = BeginDie(&bytes, 0x0011);
  AppendU16(&bytes, 0x0012);
  const size_t compile_unit_sibling = bytes.size();
  AppendU32(&bytes, 0);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "typed.c");
  AppendU16(&bytes, 0x0111);
  AppendU32(&bytes, DwarfTestFixture::kFunctionAddress);
  AppendU16(&bytes, 0x0121);
  AppendU32(&bytes, DwarfTestFixture::kFunctionAddress + 0x20);
  FinishDie(&bytes, compile_unit);

  const size_t structure = BeginDie(&bytes, 0x0013);
  AppendU16(&bytes, 0x0012);
  const size_t structure_sibling = bytes.size();
  AppendU32(&bytes, 0);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "Point");
  AppendU16(&bytes, 0x00b6);
  AppendU32(&bytes, 8);
  FinishDie(&bytes, structure);

  const size_t member = BeginDie(&bytes, 0x000d);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "x");
  AppendU16(&bytes, 0x0055);
  AppendU16(&bytes, 7);
  AppendU16(&bytes, 0x0023);
  AppendU16(&bytes, 6);
  bytes.push_back(0x04);
  AppendU32(&bytes, 0);
  bytes.push_back(0x07);
  FinishDie(&bytes, member);

  const size_t pointer_member = BeginDie(&bytes, 0x000d);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "next");
  AppendU16(&bytes, 0x0083);
  AppendU16(&bytes, 5);
  bytes.push_back(1);
  AppendU32(&bytes, static_cast<u32>(structure));
  AppendU16(&bytes, 0x0023);
  AppendU16(&bytes, 6);
  bytes.push_back(0x04);
  AppendU32(&bytes, 4);
  bytes.push_back(0x07);
  FinishDie(&bytes, pointer_member);
  PatchU32(&bytes, structure_sibling, static_cast<u32>(bytes.size()));

  // MWCC emits four-byte padding DIEs between some declaration groups.
  AppendU32(&bytes, 4);

  const size_t union_type = BeginDie(&bytes, 0x0017);
  AppendU16(&bytes, 0x0012);
  const size_t union_sibling = bytes.size();
  AppendU32(&bytes, 0);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "Value");
  AppendU16(&bytes, 0x00b6);
  AppendU32(&bytes, 4);
  FinishDie(&bytes, union_type);

  const size_t implicit_union_member = BeginDie(&bytes, 0x000d);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "integer");
  AppendU16(&bytes, 0x0055);
  AppendU16(&bytes, 7);
  FinishDie(&bytes, implicit_union_member);
  PatchU32(&bytes, union_sibling, static_cast<u32>(bytes.size()));

  const size_t array = BeginDie(&bytes, 0x0001);
  AppendU16(&bytes, 0x00a3);
  AppendU16(&bytes, 16);
  bytes.push_back(0);
  AppendU16(&bytes, 7);
  AppendU32(&bytes, 0);
  AppendU32(&bytes, 2);
  bytes.push_back(8);
  AppendU16(&bytes, 0x0055);
  AppendU16(&bytes, 7);
  FinishDie(&bytes, array);

  const size_t function = BeginDie(&bytes, 0x0006);
  AppendU16(&bytes, 0x0012);
  const size_t function_sibling = bytes.size();
  AppendU32(&bytes, 0);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "typed");
  AppendU16(&bytes, 0x0111);
  AppendU32(&bytes, DwarfTestFixture::kFunctionAddress);
  AppendU16(&bytes, 0x0121);
  AppendU32(&bytes, DwarfTestFixture::kFunctionAddress + 0x20);
  FinishDie(&bytes, function);

  const size_t parameter = BeginDie(&bytes, 0x0005);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "argument");
  AppendU16(&bytes, 0x0055);
  AppendU16(&bytes, 7);
  AppendU16(&bytes, 0x0023);
  AppendU16(&bytes, 5);
  bytes.push_back(0x01);
  AppendU32(&bytes, 3);
  FinishDie(&bytes, parameter);

  const size_t local = BeginDie(&bytes, 0x000c);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "local_point");
  AppendU16(&bytes, 0x0072);
  AppendU32(&bytes, static_cast<u32>(structure));
  AppendU16(&bytes, 0x0023);
  AppendU16(&bytes, 11);
  bytes.push_back(0x02);
  AppendU32(&bytes, 1);
  bytes.push_back(0x04);
  AppendU32(&bytes, static_cast<u32>(-8));
  bytes.push_back(0x07);
  FinishDie(&bytes, local);

  const size_t lexical_block = BeginDie(&bytes, 0x000b);
  AppendU16(&bytes, 0x0012);
  const size_t lexical_sibling = bytes.size();
  AppendU32(&bytes, 0);
  AppendU16(&bytes, 0x0111);
  AppendU32(&bytes, DwarfTestFixture::kFunctionAddress + 8);
  AppendU16(&bytes, 0x0121);
  AppendU32(&bytes, DwarfTestFixture::kFunctionAddress + 0x10);
  FinishDie(&bytes, lexical_block);

  const size_t scoped_local = BeginDie(&bytes, 0x000c);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "scoped");
  AppendU16(&bytes, 0x0055);
  AppendU16(&bytes, 7);
  AppendU16(&bytes, 0x02c6);
  AppendU32(&bytes, 0x0c);
  AppendU16(&bytes, 0x0023);
  AppendU16(&bytes, 5);
  bytes.push_back(0x03);
  AppendU32(&bytes, DwarfTestFixture::kTypedDataAddress);
  FinishDie(&bytes, scoped_local);
  PatchU32(&bytes, lexical_sibling, static_cast<u32>(bytes.size()));
  PatchU32(&bytes, function_sibling, static_cast<u32>(bytes.size()));

  const size_t global = BeginDie(&bytes, 0x0007);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "global_point");
  AppendU16(&bytes, 0x0072);
  AppendU32(&bytes, static_cast<u32>(structure));
  AppendU16(&bytes, 0x0023);
  AppendU16(&bytes, 5);
  bytes.push_back(0x03);
  AppendU32(&bytes, DwarfTestFixture::kTypedDataAddress);
  FinishDie(&bytes, global);

  const size_t global_array = BeginDie(&bytes, 0x0007);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "numbers");
  AppendU16(&bytes, 0x0072);
  AppendU32(&bytes, static_cast<u32>(array));
  AppendU16(&bytes, 0x0023);
  AppendU16(&bytes, 5);
  bytes.push_back(0x03);
  AppendU32(&bytes, DwarfTestFixture::kTypedDataAddress + 0x10);
  FinishDie(&bytes, global_array);
  PatchU32(&bytes, compile_unit_sibling, static_cast<u32>(bytes.size()));
  return bytes;
}

std::vector<u8> MakeFunctionWithoutSiblingSection()
{
  std::vector<u8> bytes;
  const size_t compile_unit = BeginDie(&bytes, 0x0011);
  AppendU16(&bytes, 0x0012);
  const size_t compile_unit_sibling = bytes.size();
  AppendU32(&bytes, 0);
  FinishDie(&bytes, compile_unit);

  const size_t function = BeginDie(&bytes, 0x0006);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "last_function");
  AppendU16(&bytes, 0x0111);
  AppendU32(&bytes, DwarfTestFixture::kFunctionAddress);
  AppendU16(&bytes, 0x0121);
  AppendU32(&bytes, DwarfTestFixture::kFunctionAddress + 4);
  FinishDie(&bytes, function);

  const size_t local = BeginDie(&bytes, 0x000c);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "last_local");
  AppendU16(&bytes, 0x0055);
  AppendU16(&bytes, 7);
  FinishDie(&bytes, local);
  PatchU32(&bytes, compile_unit_sibling, static_cast<u32>(bytes.size()));
  return bytes;
}

std::vector<u8> MakeEmptyNameLineTableDebugSection()
{
  std::vector<u8> bytes;
  const size_t compile_unit = BeginDie(&bytes, 0x0011);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "");
  AppendU16(&bytes, 0x0111);
  AppendU32(&bytes, DwarfTestFixture::kFunctionAddress);
  AppendU16(&bytes, 0x0121);
  AppendU32(&bytes, DwarfTestFixture::kFunctionAddress + 0x20);
  AppendU16(&bytes, 0x0106);
  AppendU32(&bytes, 0);
  FinishDie(&bytes, compile_unit);
  return bytes;
}

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
  // The CU header's AT_name was unreadable, but its empty occurrence remains
  // reserved so later compilation-unit file indices stay stable.
  ASSERT_EQ(result->files.size(), 1U);
  EXPECT_TRUE(result->files[0].empty());
}

TEST(DwarfReaderTest, ParseReservesIdentityForEmptyNameLineTable)
{
  const std::vector<u8> debug = MakeEmptyNameLineTableDebugSection();
  const auto result = Core::Debug::Dwarf::Parse(debug, DwarfTestFixture::kLineSection, true);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->files.size(), 1U);
  EXPECT_TRUE(result->files[0].empty());
  ASSERT_FALSE(result->lines.empty());
  EXPECT_EQ(result->lines[0].file_index, 0U);
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
  EXPECT_EQ(std::ranges::count(result->lines, 0U, &Core::Debug::Dwarf::LineEntry::file_index), 2);
  EXPECT_EQ(std::ranges::count(result->lines, 1U, &Core::Debug::Dwarf::LineEntry::file_index), 2);
}

TEST(DwarfReaderTest, ParsePreservesDuplicateCompileUnitOccurrences)
{
  const std::vector<u8> debug = DwarfTestFixture::MakeDuplicateCuNameDebugSection();
  const auto result = Core::Debug::Dwarf::Parse(debug, DwarfTestFixture::kMultiCuLineSection, true);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->files.size(), 2U);
  EXPECT_EQ(result->files[0], DwarfTestFixture::kFirstCompileUnitName);
  EXPECT_EQ(result->files[1], DwarfTestFixture::kFirstCompileUnitName);
  EXPECT_EQ(std::ranges::count(result->lines, 0U, &Core::Debug::Dwarf::LineEntry::file_index), 2);
  EXPECT_EQ(std::ranges::count(result->lines, 1U, &Core::Debug::Dwarf::LineEntry::file_index), 2);
}

TEST(DwarfReaderTest, ParseTypedVariablesAcrossPaddingDiesAndLeafSiblings)
{
  const std::vector<u8> debug = MakeTypedDebugSection();
  const auto result = Core::Debug::Dwarf::Parse(debug, {}, true);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->types.size(), 3U);
  EXPECT_EQ(result->types[0].name, "Point");
  ASSERT_EQ(result->types[0].members.size(), 2U);
  EXPECT_EQ(result->types[0].members[0].name, "x");
  EXPECT_EQ(result->types[0].members[0].location.kind,
            Core::Debug::Dwarf::LocationKind::MemberOffset);
  ASSERT_EQ(result->types[0].members[1].type.modifiers.size(), 1U);
  EXPECT_EQ(result->types[0].members[1].type.modifiers[0],
            Core::Debug::Dwarf::TypeModifier::Pointer);
  EXPECT_EQ(result->types[1].kind, Core::Debug::Dwarf::TypeKind::Union);
  ASSERT_EQ(result->types[1].members.size(), 1U);
  EXPECT_EQ(result->types[1].members[0].location.kind,
            Core::Debug::Dwarf::LocationKind::MemberOffset);
  EXPECT_EQ(result->types[1].members[0].location.value, 0U);
  EXPECT_EQ(result->types[2].kind, Core::Debug::Dwarf::TypeKind::Array);
  ASSERT_TRUE(result->types[2].array_count);
  EXPECT_EQ(*result->types[2].array_count, 3U);

  ASSERT_EQ(result->variables.size(), 5U);
  EXPECT_EQ(result->variables[0].kind, Core::Debug::Dwarf::VariableKind::Parameter);
  EXPECT_EQ(result->variables[0].location.kind, Core::Debug::Dwarf::LocationKind::Register);
  EXPECT_EQ(result->variables[1].location.kind,
            Core::Debug::Dwarf::LocationKind::BaseRegisterOffset);
  EXPECT_EQ(result->variables[1].location.offset, -8);
  EXPECT_EQ(result->variables[1].low_pc, DwarfTestFixture::kFunctionAddress);
  EXPECT_EQ(result->variables[2].name, "scoped");
  EXPECT_EQ(result->variables[2].low_pc, DwarfTestFixture::kFunctionAddress + 0xc);
  EXPECT_EQ(result->variables[2].high_pc, DwarfTestFixture::kFunctionAddress + 0x10);
  EXPECT_EQ(result->variables[3].kind, Core::Debug::Dwarf::VariableKind::Global);
  EXPECT_EQ(result->variables[3].location.kind, Core::Debug::Dwarf::LocationKind::Address);
}

TEST(DwarfReaderTest, ParseRejectsOverflowingDieAndLineBounds)
{
  const std::vector<u8> oversized_die = {0xff, 0xff, 0xff, 0xff, 0x00, 0x11};
  EXPECT_FALSE(Core::Debug::Dwarf::Parse(oversized_die, {}, true));

  std::vector<u8> oversized_line(8, 0);
  PatchU32(&oversized_line, 0, std::numeric_limits<u32>::max());
  const auto result =
      Core::Debug::Dwarf::Parse(DwarfTestFixture::kDebugSection, oversized_line, true);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->lines.empty());
}

TEST(DwarfReaderTest, ParseDescendsIntoLastParentWithoutSiblingAttribute)
{
  const auto result = Core::Debug::Dwarf::Parse(MakeFunctionWithoutSiblingSection(), {}, true);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->variables.size(), 1U);
  EXPECT_EQ(result->variables[0].name, "last_local");
  EXPECT_EQ(result->variables[0].low_pc, DwarfTestFixture::kFunctionAddress);
  EXPECT_EQ(result->variables[0].high_pc, DwarfTestFixture::kFunctionAddress + 4);
}

TEST(DwarfReaderTest, ParseEnumerationElementsAndBitfieldMetadata)
{
  std::vector<u8> bytes;
  const size_t compile_unit = BeginDie(&bytes, 0x0011);
  AppendU16(&bytes, 0x0012);
  const size_t compile_unit_sibling = bytes.size();
  AppendU32(&bytes, 0);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "types.c");
  FinishDie(&bytes, compile_unit);

  const size_t enumeration = BeginDie(&bytes, 0x0004);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "Mode");
  AppendU16(&bytes, 0x00b6);
  AppendU32(&bytes, 4);
  AppendU16(&bytes, 0x00f4);
  const size_t element_list_size = bytes.size();
  AppendU32(&bytes, 0);
  const size_t element_list = bytes.size();
  AppendU32(&bytes, static_cast<u32>(-1));
  AppendString(&bytes, "Previous");
  AppendU32(&bytes, 2);
  AppendString(&bytes, "Next");
  PatchU32(&bytes, element_list_size, static_cast<u32>(bytes.size() - element_list));
  FinishDie(&bytes, enumeration);

  const size_t unsigned_enumeration = BeginDie(&bytes, 0x0004);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "ByteMode");
  AppendU16(&bytes, 0x00b6);
  AppendU32(&bytes, 1);
  AppendU16(&bytes, 0x00f4);
  const size_t unsigned_element_list_size = bytes.size();
  AppendU32(&bytes, 0);
  const size_t unsigned_element_list = bytes.size();
  AppendU32(&bytes, 255);
  AppendString(&bytes, "Maximum");
  PatchU32(&bytes, unsigned_element_list_size,
           static_cast<u32>(bytes.size() - unsigned_element_list));
  FinishDie(&bytes, unsigned_enumeration);

  const size_t structure = BeginDie(&bytes, 0x0013);
  AppendU16(&bytes, 0x0012);
  const size_t structure_sibling = bytes.size();
  AppendU32(&bytes, 0);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "Flags");
  AppendU16(&bytes, 0x00b6);
  AppendU32(&bytes, 4);
  FinishDie(&bytes, structure);

  const size_t member = BeginDie(&bytes, 0x000d);
  AppendU16(&bytes, 0x0038);
  AppendString(&bytes, "enabled");
  AppendU16(&bytes, 0x0055);
  AppendU16(&bytes, 9);
  AppendU16(&bytes, 0x0023);
  AppendU16(&bytes, 6);
  bytes.push_back(0x04);
  AppendU32(&bytes, 0);
  bytes.push_back(0x07);
  AppendU16(&bytes, 0x00c5);
  AppendU16(&bytes, 31);
  AppendU16(&bytes, 0x00d6);
  AppendU32(&bytes, 1);
  FinishDie(&bytes, member);

  PatchU32(&bytes, structure_sibling, static_cast<u32>(bytes.size()));
  PatchU32(&bytes, compile_unit_sibling, static_cast<u32>(bytes.size()));

  const auto result = Core::Debug::Dwarf::Parse(bytes, {});
  ASSERT_TRUE(result);
  ASSERT_EQ(result->types.size(), 3U);
  EXPECT_EQ(result->types[0].kind, Core::Debug::Dwarf::TypeKind::Enumeration);
  EXPECT_FALSE(result->types[0].enumeration_is_unsigned);
  ASSERT_EQ(result->types[0].enumerators.size(), 2U);
  EXPECT_EQ(result->types[0].enumerators[0].name, "Previous");
  EXPECT_EQ(result->types[0].enumerators[0].value, -1);
  EXPECT_EQ(result->types[0].enumerators[1].name, "Next");
  EXPECT_EQ(result->types[0].enumerators[1].value, 2);
  EXPECT_EQ(result->types[1].kind, Core::Debug::Dwarf::TypeKind::Enumeration);
  EXPECT_TRUE(result->types[1].enumeration_is_unsigned);
  ASSERT_EQ(result->types[1].enumerators.size(), 1U);
  EXPECT_EQ(result->types[1].enumerators[0].value, 255);
  ASSERT_EQ(result->types[2].members.size(), 1U);
  EXPECT_EQ(result->types[2].members[0].bit_offset, 31U);
  EXPECT_EQ(result->types[2].members[0].bit_size, 1U);
}
}  // namespace

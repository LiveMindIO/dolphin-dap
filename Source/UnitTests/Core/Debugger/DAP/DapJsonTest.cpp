// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Core/Debugger/DAP/DapJson.h"

namespace
{
using namespace DAP;

TEST(DapJson, ExtractIntFieldParsesValue)
{
  EXPECT_EQ(Json::ExtractIntField(R"({"seq":42})", "seq"), 42);
}

TEST(DapJson, ExtractIntFieldSkipsWhitespace)
{
  EXPECT_EQ(Json::ExtractIntField(R"({"seq":   7})", "seq"), 7);
}

TEST(DapJson, ExtractIntFieldParsesNegative)
{
  EXPECT_EQ(Json::ExtractIntField(R"({"offset":-16})", "offset"), -16);
}

TEST(DapJson, ExtractIntFieldMissingReturnsNullopt)
{
  EXPECT_FALSE(Json::ExtractIntField(R"({"seq":1})", "count").has_value());
}

TEST(DapJson, ExtractIntFieldNonNumericReturnsNullopt)
{
  EXPECT_FALSE(Json::ExtractIntField(R"({"seq":"x"})", "seq").has_value());
}

TEST(DapJson, ExtractStringFieldParsesValue)
{
  const auto value = Json::ExtractStringField(R"({"command":"initialize"})", "command");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "initialize");
}

TEST(DapJson, ExtractStringFieldEmptyValue)
{
  const auto value = Json::ExtractStringField(R"({"command":""})", "command");
  ASSERT_TRUE(value.has_value());
  EXPECT_TRUE(value->empty());
}

TEST(DapJson, ExtractStringFieldMissingReturnsNullopt)
{
  EXPECT_FALSE(Json::ExtractStringField(R"({"command":"x"})", "path").has_value());
}

TEST(DapJson, ExtractStringFieldNonStringReturnsNullopt)
{
  // A numeric value is not a quoted string.
  EXPECT_FALSE(Json::ExtractStringField(R"({"seq":1})", "seq").has_value());
}

TEST(DapJson, ExtractStringFieldHonorsEscapedQuote)
{
  // The value contains an escaped quote; the boundary must be the unescaped one.
  const auto value = Json::ExtractStringField(R"({"path":"a\"b","next":"z"})", "path");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, R"(a\"b)");
}

TEST(DapJson, ExtractStringFieldHonorsTrailingBackslash)
{
  const auto value = Json::ExtractStringField(R"({"path":"C:\\game.iso"})", "path");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, R"(C:\\game.iso)");
}

TEST(DapJson, ExtractStringFieldUnterminatedReturnsNullopt)
{
  EXPECT_FALSE(Json::ExtractStringField(R"({"path":"abc)", "path").has_value());
}

TEST(DapJson, ParseHexAddressWithPrefix)
{
  EXPECT_EQ(Json::ParseHexAddress("0x80003100"), 0x80003100u);
}

TEST(DapJson, ParseHexAddressUppercasePrefix)
{
  EXPECT_EQ(Json::ParseHexAddress("0X8000"), 0x8000u);
}

TEST(DapJson, ParseHexAddressWithoutPrefix)
{
  EXPECT_EQ(Json::ParseHexAddress("deadbeef"), 0xdeadbeefu);
}

TEST(DapJson, ParseHexAddressRejectsGarbage)
{
  EXPECT_FALSE(Json::ParseHexAddress("nothex").has_value());
}

TEST(DapJson, ParseHexAddressRejectsEmpty)
{
  EXPECT_FALSE(Json::ParseHexAddress("").has_value());
}

TEST(DapJson, EscapeStringPassesThroughPlainText)
{
  EXPECT_EQ(Json::EscapeString("addi r3, r4, 8"), "addi r3, r4, 8");
}

TEST(DapJson, EscapeStringEscapesSpecialCharacters)
{
  EXPECT_EQ(Json::EscapeString("a\"b\\c\n\t\r"), R"(a\"b\\c\n\t\r)");
}

TEST(DapJson, EscapeStringEmpty)
{
  EXPECT_TRUE(Json::EscapeString("").empty());
}

TEST(DapJson, Base64EncodeEmpty)
{
  EXPECT_TRUE(Json::Base64Encode({}).empty());
}

TEST(DapJson, Base64EncodeKnownVectors)
{
  const std::array<u8, 1> one{{'M'}};
  EXPECT_EQ(Json::Base64Encode(one), "TQ==");

  const std::array<u8, 2> two{{'M', 'a'}};
  EXPECT_EQ(Json::Base64Encode(two), "TWE=");

  const std::array<u8, 3> three{{'M', 'a', 'n'}};
  EXPECT_EQ(Json::Base64Encode(three), "TWFu");
}
}  // namespace

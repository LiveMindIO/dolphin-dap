// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Core/Debugger/DAP/DapJson.h"

namespace
{
using namespace DAP;

TEST(DapJson, ParseObjectAcceptsObject)
{
  const auto parsed = Json::ParseObject(R"({"command":"initialize","seq":3})");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->at("command").to_str(), "initialize");
  EXPECT_EQ(parsed->at("seq").get<double>(), 3.0);
}

TEST(DapJson, ParseObjectRejectsNonObject)
{
  EXPECT_FALSE(Json::ParseObject(R"([1,2,3])").has_value());
  EXPECT_FALSE(Json::ParseObject(R"("string")").has_value());
}

TEST(DapJson, ParseObjectRejectsMalformed)
{
  EXPECT_FALSE(Json::ParseObject(R"({"unterminated":)").has_value());
  EXPECT_FALSE(Json::ParseObject("").has_value());
}

TEST(DapJson, ParseObjectHandlesEscapedQuotesInValues)
{
  // A value containing an escaped quote must not confuse the parser (the whole
  // motivation for moving off substring extraction).
  const auto parsed = Json::ParseObject(R"({"path":"a\"b","next":"z"})");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->at("path").to_str(), R"(a"b)");
  EXPECT_EQ(parsed->at("next").to_str(), "z");
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

TEST(DapJson, FormatAddressIsZeroPaddedHex)
{
  EXPECT_EQ(Json::FormatAddress(0x8000u), "0x00008000");
  EXPECT_EQ(Json::FormatAddress(0xdeadbeefu), "0xdeadbeef");
}

TEST(DapJson, FormatAddressRoundTripsThroughParse)
{
  for (const u32 address : {0u, 0x80003100u, 0xffffffffu})
    EXPECT_EQ(Json::ParseHexAddress(Json::FormatAddress(address)), address);
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

TEST(DapJson, Base64DecodeEmpty)
{
  const auto decoded = Json::Base64Decode("");
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->empty());
}

TEST(DapJson, Base64DecodeKnownVectors)
{
  EXPECT_EQ(Json::Base64Decode("TQ=="), (std::vector<u8>{'M'}));
  EXPECT_EQ(Json::Base64Decode("TWE="), (std::vector<u8>{'M', 'a'}));
  EXPECT_EQ(Json::Base64Decode("TWFu"), (std::vector<u8>{'M', 'a', 'n'}));
}

TEST(DapJson, Base64DecodeRejectsInvalid)
{
  EXPECT_FALSE(Json::Base64Decode("!!!!").has_value());
}

TEST(DapJson, Base64RoundTrip)
{
  const std::vector<u8> bytes{0x00, 0x01, 0x02, 0xfe, 0xff, 0x80, 0x7f};
  const std::string encoded = Json::Base64Encode(bytes);
  const auto decoded = Json::Base64Decode(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, bytes);
}
}  // namespace

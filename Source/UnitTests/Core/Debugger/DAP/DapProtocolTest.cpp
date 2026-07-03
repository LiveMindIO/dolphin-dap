// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/Debugger/DAP/DapProtocol.h"

namespace
{
using namespace DAP;

picojson::object ParseObjectOrDie(const std::string& text)
{
  const auto parsed = Json::ParseObject(text);
  EXPECT_TRUE(parsed.has_value());
  return parsed.value_or(picojson::object{});
}

TEST(DapProtocol, ParseRequestExtractsEnvelope)
{
  const auto message = ParseObjectOrDie(R"({
    "seq": 7,
    "type": "request",
    "command": "continue",
    "arguments": {"x": 1}
  })");
  const auto request = Protocol::ParseRequest(message);
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->seq, 7);
  EXPECT_EQ(request->command, "continue");
  EXPECT_EQ(request->arguments.at("x").get<double>(), 1.0);
}

TEST(DapProtocol, ParseRequestAllowsMissingArguments)
{
  const auto message = ParseObjectOrDie(R"({
    "seq": 1,
    "command": "configurationDone"
  })");
  const auto request = Protocol::ParseRequest(message);
  ASSERT_TRUE(request.has_value());
  EXPECT_TRUE(request->arguments.empty());
}

TEST(DapProtocol, ParseRequestRejectsMissingCommand)
{
  const auto message = ParseObjectOrDie(R"({"seq":1})");
  EXPECT_FALSE(Protocol::ParseRequest(message).has_value());
}

TEST(DapProtocol, ParseRequestRejectsMissingSeq)
{
  const auto message = ParseObjectOrDie(R"({"command":"pause"})");
  EXPECT_FALSE(Protocol::ParseRequest(message).has_value());
}

TEST(DapProtocol, ParseReadMemoryResolvesFields)
{
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x80003100",
    "offset": 16,
    "count": 64
  })");
  const auto read = Protocol::ParseReadMemory(args);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->address, 0x80003100u);
  EXPECT_EQ(read->offset, 16);
  EXPECT_EQ(read->count, 64u);
}

TEST(DapProtocol, ParseReadMemoryDefaultsOffset)
{
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x80003100",
    "count": 4
  })");
  const auto read = Protocol::ParseReadMemory(args);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->offset, 0);
}

TEST(DapProtocol, ParseReadMemoryAllowsNegativeOffset)
{
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x80003100",
    "offset": -8,
    "count": 4
  })");
  const auto read = Protocol::ParseReadMemory(args);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->offset, -8);
}

TEST(DapProtocol, ParseReadMemoryRejectsMissingReference)
{
  const auto args = ParseObjectOrDie(R"({"count":4})");
  EXPECT_FALSE(Protocol::ParseReadMemory(args).has_value());
}

TEST(DapProtocol, ParseReadMemoryRejectsMissingCount)
{
  const auto args = ParseObjectOrDie(R"({"memoryReference":"0x80003100"})");
  EXPECT_FALSE(Protocol::ParseReadMemory(args).has_value());
}

TEST(DapProtocol, ParseWriteMemoryDecodesData)
{
  // "TWFu" decodes to "Man".
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x80003100",
    "data": "TWFu",
    "allowPartial": true
  })");
  const auto write = Protocol::ParseWriteMemory(args);
  ASSERT_TRUE(write.has_value());
  EXPECT_EQ(write->address, 0x80003100u);
  EXPECT_TRUE(write->allow_partial);
  EXPECT_EQ(write->data, (std::vector<u8>{'M', 'a', 'n'}));
}

TEST(DapProtocol, ParseWriteMemoryDefaultsAllowPartial)
{
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x80003100",
    "data": "TWFu"
  })");
  const auto write = Protocol::ParseWriteMemory(args);
  ASSERT_TRUE(write.has_value());
  EXPECT_FALSE(write->allow_partial);
}

TEST(DapProtocol, ParseWriteMemoryRejectsInvalidBase64)
{
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x80003100",
    "data": "!!!!"
  })");
  EXPECT_FALSE(Protocol::ParseWriteMemory(args).has_value());
}

TEST(DapProtocol, ParseWriteMemoryRejectsMissingData)
{
  const auto args = ParseObjectOrDie(R"({"memoryReference":"0x80003100"})");
  EXPECT_FALSE(Protocol::ParseWriteMemory(args).has_value());
}

TEST(DapProtocol, ParseDisassembleDefaults)
{
  const auto args = ParseObjectOrDie(R"({"memoryReference":"0x80003100"})");
  const auto disasm = Protocol::ParseDisassemble(args);
  ASSERT_TRUE(disasm.has_value());
  EXPECT_EQ(disasm->address, 0x80003100u);
  EXPECT_EQ(disasm->instruction_count, 1u);
  EXPECT_EQ(disasm->instruction_offset, 0);
}

TEST(DapProtocol, ParseSetBreakpointsResolvesLinesAgainstBase)
{
  const auto args = ParseObjectOrDie(R"({
    "source": {"name": "0x80003000"},
    "breakpoints": [{"line": 0}, {"line": 2}]
  })");
  const auto parsed = Protocol::ParseSetBreakpoints(args);
  ASSERT_EQ(parsed.breakpoints.size(), 2u);
  ASSERT_TRUE(parsed.breakpoints[0].address.has_value());
  EXPECT_EQ(*parsed.breakpoints[0].address, 0x80003000u);
  ASSERT_TRUE(parsed.breakpoints[1].address.has_value());
  EXPECT_EQ(*parsed.breakpoints[1].address, 0x80003008u);
}

TEST(DapProtocol, ParseSetBreakpointsLeavesLinesUnresolvedWithoutBase)
{
  const auto args = ParseObjectOrDie(R"({
    "source": {"name": "main.c"},
    "breakpoints": [{"line": 3}]
  })");
  const auto parsed = Protocol::ParseSetBreakpoints(args);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  EXPECT_FALSE(parsed.breakpoints[0].address.has_value());
}

TEST(DapProtocol, ParseSetBreakpointsUsesPathWhenNameNotHex)
{
  const auto args = ParseObjectOrDie(R"({
    "source": {"name": "game.c", "path": "0x80004000"},
    "breakpoints": [{"line": 1}]
  })");
  const auto parsed = Protocol::ParseSetBreakpoints(args);
  ASSERT_TRUE(parsed.base.has_value());
  EXPECT_EQ(*parsed.base, 0x80004000u);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  ASSERT_TRUE(parsed.breakpoints[0].address.has_value());
  EXPECT_EQ(*parsed.breakpoints[0].address, 0x80004004u);
}

TEST(DapProtocol, ParseSetVariableExtractsArguments)
{
  const auto message = ParseObjectOrDie(R"({
    "variablesReference": 1000,
    "name": "r3",
    "value": "0x12345678"
  })");
  const auto arguments = Protocol::ParseSetVariable(message);
  ASSERT_TRUE(arguments.has_value());
  EXPECT_EQ(arguments->variables_reference, 1000);
  EXPECT_EQ(arguments->name, "r3");
  EXPECT_EQ(arguments->value, "0x12345678");
}

TEST(DapProtocol, ParseSetVariableRejectsMissingName)
{
  const auto message = ParseObjectOrDie(R"({
    "variablesReference": 1000,
    "value": "0"
  })");
  EXPECT_FALSE(Protocol::ParseSetVariable(message).has_value());
}

TEST(DapProtocol, MakeResponseSerializesEnvelope)
{
  picojson::object body;
  body.emplace("allThreadsContinued", true);
  const std::string serialized =
      Protocol::Serialize(Protocol::MakeResponse(5, 4, "continue", true, std::move(body)));

  const auto parsed = Json::ParseObject(serialized);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->at("seq").get<double>(), 5.0);
  EXPECT_EQ(parsed->at("request_seq").get<double>(), 4.0);
  EXPECT_EQ(parsed->at("type").to_str(), "response");
  EXPECT_EQ(parsed->at("command").to_str(), "continue");
  EXPECT_TRUE(parsed->at("success").get<bool>());
  EXPECT_TRUE(parsed->at("body").get<picojson::object>().at("allThreadsContinued").get<bool>());
}

TEST(DapProtocol, MakeEventSerializesEnvelope)
{
  picojson::object body;
  body.emplace("reason", std::string("breakpoint"));
  const std::string serialized =
      Protocol::Serialize(Protocol::MakeEvent(9, "stopped", std::move(body)));

  const auto parsed = Json::ParseObject(serialized);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->at("type").to_str(), "event");
  EXPECT_EQ(parsed->at("event").to_str(), "stopped");
  EXPECT_EQ(parsed->at("body").get<picojson::object>().at("reason").to_str(), "breakpoint");
}

TEST(DapProtocol, DisassemblyInstructionTextIsEscapedOnSerialize)
{
  // Instruction text with a quote must serialize into valid JSON.
  picojson::object instruction;
  instruction.emplace("instruction", std::string(R"(li r3, "x")"));
  picojson::array instructions;
  instructions.emplace_back(std::move(instruction));
  picojson::object body;
  body.emplace("instructions", std::move(instructions));

  const std::string serialized =
      Protocol::Serialize(Protocol::MakeResponse(1, 1, "disassemble", true, std::move(body)));
  const auto reparsed = Json::ParseObject(serialized);
  ASSERT_TRUE(reparsed.has_value());
  const auto& out =
      reparsed->at("body").get<picojson::object>().at("instructions").get<picojson::array>();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].get<picojson::object>().at("instruction").to_str(), R"(li r3, "x")");
}
}  // namespace

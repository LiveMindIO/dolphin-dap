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

TEST(DapProtocol, ParseSetBreakpointsExtractsCondition)
{
  const auto message = ParseObjectOrDie(R"({
    "source": {"name": "0x80001000"},
    "breakpoints": [{"line": 1, "condition": "r3 == 0"}]
  })");
  const auto parsed = Protocol::ParseSetBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  ASSERT_TRUE(parsed.breakpoints[0].condition.has_value());
  EXPECT_EQ(*parsed.breakpoints[0].condition, "r3 == 0");
}

TEST(DapProtocol, ParseSetDataBreakpointsResolvesAccessType)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [
      {"dataId": "0x80003100", "accessType": "write"},
      {"dataId": "0x80003200", "accessType": "read"}
    ]
  })");
  const auto parsed = Protocol::ParseSetDataBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 2u);
  ASSERT_TRUE(parsed.breakpoints[0].address.has_value());
  EXPECT_EQ(*parsed.breakpoints[0].address, 0x80003100u);
  EXPECT_FALSE(parsed.breakpoints[0].read);
  EXPECT_TRUE(parsed.breakpoints[0].write);
  EXPECT_TRUE(parsed.breakpoints[1].read);
  EXPECT_FALSE(parsed.breakpoints[1].write);
}

TEST(DapProtocol, ParseSetDataBreakpointsDefaultsToReadWrite)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"dataId": "0x80003100"}]
  })");
  const auto parsed = Protocol::ParseSetDataBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  EXPECT_TRUE(parsed.breakpoints[0].read);
  EXPECT_TRUE(parsed.breakpoints[0].write);
}

TEST(DapProtocol, ParseSetDataBreakpointsUnknownAccessTypeIsReadWrite)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"dataId": "0x80003100", "accessType": "readWrite"}]
  })");
  const auto parsed = Protocol::ParseSetDataBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  EXPECT_TRUE(parsed.breakpoints[0].read);
  EXPECT_TRUE(parsed.breakpoints[0].write);
}

TEST(DapProtocol, ParseSetDataBreakpointsLeavesNonHexDataIdUnresolved)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"dataId": "not-hex", "accessType": "write"}]
  })");
  const auto parsed = Protocol::ParseSetDataBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  EXPECT_FALSE(parsed.breakpoints[0].address.has_value());
}

TEST(DapProtocol, ParseSetDataBreakpointsExtractsCondition)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"dataId": "0x80003100", "condition": "r3 == 1"}]
  })");
  const auto parsed = Protocol::ParseSetDataBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  ASSERT_TRUE(parsed.breakpoints[0].condition.has_value());
  EXPECT_EQ(*parsed.breakpoints[0].condition, "r3 == 1");
}

TEST(DapProtocol, ParseSetDataBreakpointsReadsLengthExtension)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"dataId": "0x80003100", "accessType": "write", "length": 256}]
  })");
  const auto parsed = Protocol::ParseSetDataBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  ASSERT_TRUE(parsed.breakpoints[0].address.has_value());
  EXPECT_EQ(parsed.breakpoints[0].length, 256u);
}

TEST(DapProtocol, ParseSetDataBreakpointsLengthDefaultsToOne)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"dataId": "0x80003100"}]
  })");
  const auto parsed = Protocol::ParseSetDataBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  EXPECT_EQ(parsed.breakpoints[0].length, 1u);
}

TEST(DapProtocol, ParseSetDataBreakpointsZeroLengthBecomesOne)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"dataId": "0x80003100", "length": 0}]
  })");
  const auto parsed = Protocol::ParseSetDataBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  EXPECT_EQ(parsed.breakpoints[0].length, 1u);
}

TEST(DapProtocol, ParseEvaluateRejectsMissingExpression)
{
  const auto message = ParseObjectOrDie(R"({"context": "watch"})");
  EXPECT_FALSE(Protocol::ParseEvaluate(message).has_value());
}

TEST(DapProtocol, ParseEvaluateExtractsExpression)
{
  const auto message = ParseObjectOrDie(R"({"expression": "r3"})");
  const auto parsed = Protocol::ParseEvaluate(message);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->expression, "r3");
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

TEST(DapProtocol, ParseSetInstructionBreakpointsResolvesReference)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"instructionReference": "0x80001000"}]
  })");
  const auto parsed = Protocol::ParseSetInstructionBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  ASSERT_TRUE(parsed.breakpoints[0].address.has_value());
  EXPECT_EQ(*parsed.breakpoints[0].address, 0x80001000u);
  EXPECT_FALSE(parsed.breakpoints[0].condition.has_value());
}

TEST(DapProtocol, ParseSetInstructionBreakpointsAppliesOffsetAndCondition)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"instructionReference": "0x80001000", "offset": 8, "condition": "r3 == 1"}]
  })");
  const auto parsed = Protocol::ParseSetInstructionBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  ASSERT_TRUE(parsed.breakpoints[0].address.has_value());
  EXPECT_EQ(*parsed.breakpoints[0].address, 0x80001008u);
  ASSERT_TRUE(parsed.breakpoints[0].condition.has_value());
  EXPECT_EQ(*parsed.breakpoints[0].condition, "r3 == 1");
}

TEST(DapProtocol, ParseSetInstructionBreakpointsAppliesNegativeOffset)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"instructionReference": "0x80001000", "offset": -4}]
  })");
  const auto parsed = Protocol::ParseSetInstructionBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  ASSERT_TRUE(parsed.breakpoints[0].address.has_value());
  EXPECT_EQ(*parsed.breakpoints[0].address, 0x80000ffcu);
}

TEST(DapProtocol, ParseSetInstructionBreakpointsLeavesNonHexReferenceUnresolved)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": [{"instructionReference": "not-hex"}]
  })");
  const auto parsed = Protocol::ParseSetInstructionBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  EXPECT_FALSE(parsed.breakpoints[0].address.has_value());
}

TEST(DapProtocol, ParseSetInstructionBreakpointsEmptyWhenNoBreakpointsKey)
{
  const auto message = ParseObjectOrDie(R"({})");
  const auto parsed = Protocol::ParseSetInstructionBreakpoints(message);
  EXPECT_TRUE(parsed.breakpoints.empty());
}

TEST(DapProtocol, ParseSetInstructionBreakpointsClearsWithEmptyArray)
{
  const auto message = ParseObjectOrDie(R"({"breakpoints": []})");
  const auto parsed = Protocol::ParseSetInstructionBreakpoints(message);
  EXPECT_TRUE(parsed.breakpoints.empty());
}

TEST(DapProtocol, ParseSetInstructionBreakpointsSkipsNonObjectEntries)
{
  const auto message = ParseObjectOrDie(R"({
    "breakpoints": ["nonsense", {"instructionReference": "0x80001000"}]
  })");
  const auto parsed = Protocol::ParseSetInstructionBreakpoints(message);
  ASSERT_EQ(parsed.breakpoints.size(), 1u);
  ASSERT_TRUE(parsed.breakpoints[0].address.has_value());
  EXPECT_EQ(*parsed.breakpoints[0].address, 0x80001000u);
}

TEST(DapProtocol, ParseGotoTargetsResolvesSourceAndLine)
{
  const auto message = ParseObjectOrDie(R"({
    "source": {"name": "0x80001000"},
    "line": 2
  })");
  const auto parsed = Protocol::ParseGotoTargets(message);
  ASSERT_TRUE(parsed.address.has_value());
  EXPECT_EQ(*parsed.address, 0x80001008u);
}

TEST(DapProtocol, ParseGotoTargetsWithoutSourceIsUnresolved)
{
  const auto message = ParseObjectOrDie(R"({"line": 2})");
  const auto parsed = Protocol::ParseGotoTargets(message);
  EXPECT_FALSE(parsed.address.has_value());
}

TEST(DapProtocol, ParseGotoTargetsWithoutLineUsesSourceBase)
{
  const auto message = ParseObjectOrDie(R"({"source": {"name": "0x80001000"}})");
  const auto parsed = Protocol::ParseGotoTargets(message);
  ASSERT_TRUE(parsed.address.has_value());
  EXPECT_EQ(*parsed.address, 0x80001000u);
}

TEST(DapProtocol, ParseGotoExtractsThreadAndTarget)
{
  const auto message = ParseObjectOrDie(R"({"threadId": 1, "targetId": 2147487744})");
  const auto parsed = Protocol::ParseGoto(message);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->thread_id, 1);
  EXPECT_EQ(parsed->target, 0x80001000u);
}

TEST(DapProtocol, ParseGotoRejectsMissingTarget)
{
  const auto message = ParseObjectOrDie(R"({"threadId": 1})");
  EXPECT_FALSE(Protocol::ParseGoto(message).has_value());
}

TEST(DapProtocol, ParseSourceRequestFromSourceReference)
{
  const auto message = ParseObjectOrDie(R"({
    "sourceReference": 2147487744,
    "startLine": 2,
    "endLine": 3
  })");
  const auto parsed = Protocol::ParseSourceRequest(message);
  ASSERT_TRUE(parsed.base.has_value());
  EXPECT_EQ(*parsed.base, 0x80001000u);
  EXPECT_EQ(parsed.start_line, 2);
  EXPECT_EQ(parsed.end_line, 3);
}

TEST(DapProtocol, ParseSourceRequestFromSourceObject)
{
  const auto message = ParseObjectOrDie(R"({
    "source": {"name": "0x80001000"},
    "startLine": 1
  })");
  const auto parsed = Protocol::ParseSourceRequest(message);
  ASSERT_TRUE(parsed.base.has_value());
  EXPECT_EQ(*parsed.base, 0x80001000u);
  EXPECT_EQ(parsed.start_line, 1);
  EXPECT_EQ(parsed.end_line, -1);
}

TEST(DapProtocol, ParseBreakpointLocationsResolvesLineRange)
{
  const auto message = ParseObjectOrDie(R"({
    "source": {"name": "0x80001000"},
    "line": 1,
    "endLine": 3
  })");
  const auto parsed = Protocol::ParseBreakpointLocations(message);
  ASSERT_TRUE(parsed.base.has_value());
  EXPECT_EQ(*parsed.base, 0x80001000u);
  EXPECT_EQ(parsed.start_line, 1);
  EXPECT_EQ(parsed.end_line, 3);
}

TEST(DapProtocol, ParseBreakpointLocationsWithoutSourceIsUnresolved)
{
  const auto message = ParseObjectOrDie(R"({"line": 1})");
  const auto parsed = Protocol::ParseBreakpointLocations(message);
  EXPECT_FALSE(parsed.base.has_value());
}

TEST(DapProtocol, ParseBreakpointLocationsResolvesSourceReference)
{
  const auto message = ParseObjectOrDie(R"({
    "sourceReference": 1,
    "line": 2,
    "endLine": 5
  })");
  const auto parsed = Protocol::ParseBreakpointLocations(message);
  ASSERT_TRUE(parsed.base.has_value());
  EXPECT_EQ(*parsed.base, 1u);
  EXPECT_EQ(parsed.start_line, 2);
  EXPECT_EQ(parsed.end_line, 5);
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

TEST(DapProtocol, ParseLaunchOmitsStopOnEntryByDefault)
{
  const auto args = ParseObjectOrDie(R"({})");
  const auto parsed = Protocol::ParseLaunch(args);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->stop_on_entry.has_value());
}

TEST(DapProtocol, ParseLaunchStopOnEntryFalse)
{
  const auto args = ParseObjectOrDie(R"({"stopOnEntry": false})");
  const auto parsed = Protocol::ParseLaunch(args);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->stop_on_entry.has_value());
  EXPECT_FALSE(*parsed->stop_on_entry);
}

TEST(DapProtocol, ParseLaunchStopOnEntryTrue)
{
  const auto args = ParseObjectOrDie(R"({"stopOnEntry": true})");
  const auto parsed = Protocol::ParseLaunch(args);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->stop_on_entry.has_value());
  EXPECT_TRUE(*parsed->stop_on_entry);
}

TEST(DapProtocol, ParseFreezeStandaloneFormResolvesAddressCountAndValue)
{
  // DESNOTE(jbarber, 2026-07-21): `data` is base64-encoded per the DAP
  // `writeMemory` convention. "AAAAAQ==" decodes to {0x00, 0x00, 0x00, 0x01}
  // (4 bytes). This form creates a new frozen subscription at the resolved
  // address.
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x00003100",
    "count": 4,
    "data": "AAAAAQ=="
  })");
  const auto parsed = Protocol::ParseFreeze(args);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->watch_id.has_value());
  ASSERT_TRUE(parsed->address.has_value());
  EXPECT_EQ(*parsed->address, 0x00003100u);
  ASSERT_TRUE(parsed->count.has_value());
  EXPECT_EQ(*parsed->count, 4u);
  EXPECT_EQ(parsed->value, (std::vector<u8>{0x00, 0x00, 0x00, 0x01}));
}

TEST(DapProtocol, ParseFreezeExistingWatchForm)
{
  const auto args = ParseObjectOrDie(R"({
    "watchId": 7,
    "data": "AAAAAQ=="
  })");
  const auto parsed = Protocol::ParseFreeze(args);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->watch_id.has_value());
  EXPECT_EQ(*parsed->watch_id, 7);
  EXPECT_FALSE(parsed->address.has_value());
  EXPECT_FALSE(parsed->count.has_value());
  EXPECT_EQ(parsed->value, (std::vector<u8>{0x00, 0x00, 0x00, 0x01}));
}

TEST(DapProtocol, ParseFreezeRejectsValueLengthMismatchWithCount)
{
  // DESNOTE(jbarber, 2026-07-21): "AA==" decodes to one byte (0x00). count
  // is 4 -- the frozen canon must cover the entire watched region.
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x00003100",
    "count": 4,
    "data": "AA=="
  })");
  EXPECT_FALSE(Protocol::ParseFreeze(args).has_value());
}

TEST(DapProtocol, ParseFreezeRejectsMissingData)
{
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x00003100",
    "count": 4
  })");
  EXPECT_FALSE(Protocol::ParseFreeze(args).has_value());
}

TEST(DapProtocol, ParseFreezeRejectsInvalidBase64)
{
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x00003100",
    "count": 4,
    "data": "this is not base64!!!"
  })");
  EXPECT_FALSE(Protocol::ParseFreeze(args).has_value());
}

TEST(DapProtocol, ParseFreezeRejectsMixedWatchIdAndAddress)
{
  // Form 1 and form 2 are mutually exclusive; the sampler can't tell which
  // width the client wants if both `watchId` and `memoryReference`+`count`
  // are present, so the parser rejects.
  const auto args = ParseObjectOrDie(R"({
    "watchId": 7,
    "memoryReference": "0x00003100",
    "count": 4,
    "data": "AAAAAAE="
  })");
  EXPECT_FALSE(Protocol::ParseFreeze(args).has_value());
}

TEST(DapProtocol, ParseFreezeRejectsCountZero)
{
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0x00003100",
    "count": 0,
    "data": ""
  })");
  EXPECT_FALSE(Protocol::ParseFreeze(args).has_value());
}

TEST(DapProtocol, ParseFreezeRejectsAddressCountOverflow)
{
  // address + count wraps past u32 max -- the sampler would compute
  // out-of-range addresses in Tick(); reject at parse time.
  const auto args = ParseObjectOrDie(R"({
    "memoryReference": "0xfffffff0",
    "count": 32,
    "data": "AAAAAAE="
  })");
  EXPECT_FALSE(Protocol::ParseFreeze(args).has_value());
}

TEST(DapProtocol, ParseUnfreezeExtractsWatchId)
{
  const auto args = ParseObjectOrDie(R"({"watchId": 42})");
  const auto parsed = Protocol::ParseUnfreeze(args);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->watch_id, 42);
}

TEST(DapProtocol, ParseUnfreezeRejectsMissingWatchId)
{
  const auto args = ParseObjectOrDie(R"({})");
  EXPECT_FALSE(Protocol::ParseUnfreeze(args).has_value());
}

// --- findFreeMemory / injectCode / detour parsers ---

TEST(DapProtocol, ParseFindFreeMemoryExtractsCount)
{
  const auto args = ParseObjectOrDie(R"({"count": 64})");
  const auto parsed = Protocol::ParseFindFreeMemory(args);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->count, 64u);
}

TEST(DapProtocol, ParseFindFreeMemoryRejectsMissingCount)
{
  const auto args = ParseObjectOrDie(R"({})");
  EXPECT_FALSE(Protocol::ParseFindFreeMemory(args).has_value());
}

TEST(DapProtocol, ParseFindFreeMemoryRejectsZeroCount)
{
  const auto args = ParseObjectOrDie(R"({"count": 0})");
  EXPECT_FALSE(Protocol::ParseFindFreeMemory(args).has_value());
}

TEST(DapProtocol, ParseInjectCodeStandaloneFormResolvesCode)
{
  // "AAAAAQ==" base64-decodes to {0x00,0x00,0x00,0x01}.
  const auto args = ParseObjectOrDie(
      R"({"memoryReference": "0x80001000", "code": "AAAAAQ=="})");
  const auto parsed = Protocol::ParseInjectCode(args);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->address.has_value());
  EXPECT_EQ(*parsed->address, 0x80001000u);
  ASSERT_EQ(parsed->code.size(), 4u);
  EXPECT_EQ(parsed->code, (std::vector<u8>{0x00, 0x00, 0x00, 0x01}));
}

TEST(DapProtocol, ParseInjectCodeAllowsMissingAddress)
{
  // Server allocates via FindFreeMemory when `memoryReference` is omitted.
  const auto args = ParseObjectOrDie(R"({"code": "AAAAAQ=="})");
  const auto parsed = Protocol::ParseInjectCode(args);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->address.has_value());
  EXPECT_EQ(parsed->code.size(), 4u);
}

TEST(DapProtocol, ParseInjectCodeRejectsMissingData)
{
  const auto args = ParseObjectOrDie(R"({"memoryReference": "0x80001000"})");
  EXPECT_FALSE(Protocol::ParseInjectCode(args).has_value());
}

TEST(DapProtocol, ParseInjectCodeRejectsInvalidBase64)
{
  // "AAAA" is missing padding; reject the parse (strict base64).
  const auto args = ParseObjectOrDie(
      R"({"memoryReference": "0x80001000", "code": "AAAA"})");
  EXPECT_FALSE(Protocol::ParseInjectCode(args).has_value());
}

TEST(DapProtocol, ParseInjectCodeRejectsNonMultipleOfFourLength)
{
  // "AAE=" base64-decodes to a single byte (0x01) -- not a 4-byte instruction.
  const auto args = ParseObjectOrDie(
      R"({"memoryReference": "0x80001000", "code": "AAE="})");
  EXPECT_FALSE(Protocol::ParseInjectCode(args).has_value());
}

TEST(DapProtocol, ParseDetourExtractsTargetAddressAndBody)
{
  // detourBody: "fGMbeA==" base64-decodes to {0x7C,0x63,0x1B,0x78}
  // (`mr r3,r3`). target via memoryReference.
  const auto args = ParseObjectOrDie(
      R"({"memoryReference": "0x80001000", "detourBody": "fGMbeA=="})");
  const auto parsed = Protocol::ParseDetour(args);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->target_address, 0x80001000u);
  EXPECT_FALSE(parsed->detour_address.has_value());
  ASSERT_EQ(parsed->detour_body.size(), 4u);
  EXPECT_EQ(parsed->detour_body, (std::vector<u8>{0x7C, 0x63, 0x1B, 0x78}));
}

TEST(DapProtocol, ParseDetourAcceptsExplicitDetourAddress)
{
  const auto args = ParseObjectOrDie(
      R"({"memoryReference": "0x80001000", "detourAddress": "0x8000C000",
        "detourBody": "fGMbeA=="})");
  const auto parsed = Protocol::ParseDetour(args);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->target_address, 0x80001000u);
  ASSERT_TRUE(parsed->detour_address.has_value());
  EXPECT_EQ(*parsed->detour_address, 0x8000C000u);
}

TEST(DapProtocol, ParseDetourRejectsMissingTargetAddress)
{
  const auto args = ParseObjectOrDie(R"({"detourBody": "fGMbeA=="})");
  EXPECT_FALSE(Protocol::ParseDetour(args).has_value());
}

TEST(DapProtocol, ParseDetourRejectsMissingBody)
{
  const auto args = ParseObjectOrDie(R"({"memoryReference": "0x80001000"})");
  EXPECT_FALSE(Protocol::ParseDetour(args).has_value());
}

TEST(DapProtocol, ParseDetourRejectsNonMultipleOfFourBodyLength)
{
  // "AAE=" decodes to one byte -- not a 4-byte aligned instruction sequence.
  const auto args = ParseObjectOrDie(
      R"({"memoryReference": "0x80001000", "detourBody": "AAE="})");
  EXPECT_FALSE(Protocol::ParseDetour(args).has_value());
}

}  // namespace

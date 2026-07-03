// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

// Structured models for the subset of the Debug Adapter Protocol the Dolphin
// DAP server speaks. Requests are decoded from picojson objects into typed
// structs; responses and events are built as picojson objects so that string
// escaping and number formatting are handled by the serializer.
namespace DAP::Protocol
{
// A decoded DAP request envelope. `arguments` is empty when the request carries
// none.
struct Request
{
  int seq = 0;
  std::string command;
  picojson::object arguments;
};

std::optional<Request> ParseRequest(const picojson::object& message);

// Arguments of a `readMemory` request. `address` is the resolved base from the
// `memoryReference` hex string; `offset` may be negative per the DAP spec.
struct ReadMemoryArguments
{
  u32 address = 0;
  s64 offset = 0;
  u32 count = 0;
};

std::optional<ReadMemoryArguments> ParseReadMemory(const picojson::object& arguments);

// Arguments of a `writeMemory` request. `data` is the already base64-decoded
// payload to store starting at `address + offset`.
struct WriteMemoryArguments
{
  u32 address = 0;
  s64 offset = 0;
  bool allow_partial = false;
  std::vector<u8> data;
};

std::optional<WriteMemoryArguments> ParseWriteMemory(const picojson::object& arguments);

// Arguments of a `disassemble` request.
struct DisassembleArguments
{
  u32 address = 0;
  s64 offset = 0;
  s64 instruction_offset = 0;
  u32 instruction_count = 1;
};

std::optional<DisassembleArguments> ParseDisassemble(const picojson::object& arguments);

// A single requested source breakpoint, resolved to an absolute address when a
// base address and line are both available.
struct RequestedBreakpoint
{
  std::optional<u32> address;
};

// Arguments of a `setBreakpoints` request. The base address is taken from the
// `source.name`/`source.path` hex string (Dolphin models a "source" as a code
// region anchored at an address). Each breakpoint's address is base + line*4.
struct SetBreakpointsArguments
{
  std::optional<u32> base;
  std::vector<RequestedBreakpoint> breakpoints;
};

SetBreakpointsArguments ParseSetBreakpoints(const picojson::object& arguments);

struct SetVariableArguments
{
  int variables_reference = 0;
  std::string name;
  std::string value;
};

std::optional<SetVariableArguments> ParseSetVariable(const picojson::object& arguments);

// Envelope builders. `seq` is the server's monotonically increasing sequence
// number; the caller owns its allocation.
picojson::object MakeResponse(int seq, int request_seq, std::string_view command, bool success,
                              picojson::object body);
picojson::object MakeErrorResponse(int seq, int request_seq, std::string_view command,
                                   std::string_view message);
picojson::object MakeEvent(int seq, std::string_view event, picojson::object body);

std::string Serialize(const picojson::object& object);
}  // namespace DAP::Protocol

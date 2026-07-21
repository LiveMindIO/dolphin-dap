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
  std::optional<std::string> condition;
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

struct RequestedDataBreakpoint
{
  std::optional<u32> address;
  // Dolphin extension: number of bytes to watch starting at `address`.
  // Defaults to 1 (standard DAP single-byte data breakpoint). When > 1 the
  // controller installs a ranged PPC memcheck over [address, address+length-1].
  u32 length = 1;
  bool read = true;
  bool write = true;
  std::optional<std::string> condition;
};

struct SetDataBreakpointsArguments
{
  std::vector<RequestedDataBreakpoint> breakpoints;
};

SetDataBreakpointsArguments ParseSetDataBreakpoints(const picojson::object& arguments);

struct EvaluateArguments
{
  std::string expression;
};

std::optional<EvaluateArguments> ParseEvaluate(const picojson::object& arguments);

// A single requested instruction breakpoint, resolved from the hex
// `instructionReference` string plus an optional signed byte `offset`.
struct RequestedInstructionBreakpoint
{
  std::optional<u32> address;
  std::optional<std::string> condition;
};

struct SetInstructionBreakpointsArguments
{
  std::vector<RequestedInstructionBreakpoint> breakpoints;
};

SetInstructionBreakpointsArguments
ParseSetInstructionBreakpoints(const picojson::object& arguments);

// Arguments of a `gotoTargets` request. The base address is taken from the
// `source.name`/`source.path` hex string; `line` is a 4-byte instruction index.
struct GotoTargetsArguments
{
  std::optional<u32> address;
};

GotoTargetsArguments ParseGotoTargets(const picojson::object& arguments);

// Arguments of a `goto` request. `target` is the resolved address (gotoTargets
// reports each target's address as its id).
struct GotoArguments
{
  int thread_id = 0;
  u32 target = 0;
};

std::optional<GotoArguments> ParseGoto(const picojson::object& arguments);

struct SourceRequestArguments
{
  std::optional<u32> base;
  int start_line = 0;
  int end_line = -1;
};

SourceRequestArguments ParseSourceRequest(const picojson::object& arguments);

struct BreakpointLocationsArguments
{
  std::optional<u32> base;
  int start_line = 0;
  int end_line = -1;
};

BreakpointLocationsArguments ParseBreakpointLocations(const picojson::object& arguments);

// Arguments of a `dolphin_realtimeWatch` custom request. `address` is resolved
// from the hex `memoryReference`; `count` is the number of bytes to watch
// starting at `address`.
struct RealtimeWatchArguments
{
  u32 address = 0;
  u32 count = 0;
};

std::optional<RealtimeWatchArguments> ParseRealtimeWatch(const picojson::object& arguments);

// Arguments of a `dolphin_realtimeWatchCancel` custom request.
struct RealtimeWatchCancelArguments
{
  int watch_id = 0;
};

std::optional<RealtimeWatchCancelArguments>
ParseRealtimeWatchCancel(const picojson::object& arguments);

// Arguments of a `launch`/`attach` request. `stop_on_entry` is nullopt when
// the client omitted the field; the session resolves the effective policy
// against the `Dolphin.General.DAPStopOnEntry` config default. When present
// (true or false) it overrides the config for this session.
struct LaunchArguments
{
  std::optional<bool> stop_on_entry;
};

std::optional<LaunchArguments> ParseLaunch(const picojson::object& arguments);

// Envelope builders. `seq` is the server's monotonically increasing sequence
// number; the caller owns its allocation.
picojson::object MakeResponse(int seq, int request_seq, std::string_view command, bool success,
                              picojson::object body);
picojson::object MakeErrorResponse(int seq, int request_seq, std::string_view command,
                                   std::string_view message);
picojson::object MakeEvent(int seq, std::string_view event, picojson::object body);

std::string Serialize(const picojson::object& object);
}  // namespace DAP::Protocol

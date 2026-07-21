// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapProtocol.h"

#include <limits>

#include "Common/JsonUtil.h"
#include "Core/Debugger/DAP/DapJson.h"

namespace DAP::Protocol
{
namespace
{
const picojson::object* GetObject(const picojson::object& obj, const std::string& key)
{
  const auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<picojson::object>())
    return nullptr;
  return &it->second.get<picojson::object>();
}

const picojson::array* GetArray(const picojson::object& obj, const std::string& key)
{
  const auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<picojson::array>())
    return nullptr;
  return &it->second.get<picojson::array>();
}

// Resolve a Dolphin "source" object to a base address. A source is anchored at
// an address encoded as a hex string in either `name` or `path`.
std::optional<u32> ResolveSourceBase(const picojson::object& arguments)
{
  const picojson::object* source = GetObject(arguments, "source");
  if (source == nullptr)
    return std::nullopt;

  if (const std::optional<std::string> name = ReadStringFromJson(*source, "name"))
  {
    if (const std::optional<u32> base = Json::ParseHexAddress(*name))
      return base;
  }
  if (const std::optional<std::string> path = ReadStringFromJson(*source, "path"))
  {
    if (const std::optional<u32> base = Json::ParseHexAddress(*path))
      return base;
  }
  return std::nullopt;
}

std::optional<u32> ResolveMemoryReference(const picojson::object& arguments)
{
  const std::optional<std::string> reference = ReadStringFromJson(arguments, "memoryReference");
  if (!reference)
    return std::nullopt;
  return Json::ParseHexAddress(*reference);
}

std::optional<u32> ResolveSourceReference(const picojson::object& arguments)
{
  if (const std::optional<s64> reference = ReadNumericFromJson<s64>(arguments, "sourceReference"))
    return static_cast<u32>(*reference);
  return ResolveSourceBase(arguments);
}
}  // namespace

std::optional<Request> ParseRequest(const picojson::object& message)
{
  const std::optional<std::string> command = ReadStringFromJson(message, "command");
  const std::optional<int> seq = ReadNumericFromJson<int>(message, "seq");
  if (!command || !seq)
    return std::nullopt;

  Request request;
  request.seq = *seq;
  request.command = *command;
  if (const picojson::object* arguments = GetObject(message, "arguments"))
    request.arguments = *arguments;

  return request;
}

std::optional<ReadMemoryArguments> ParseReadMemory(const picojson::object& arguments)
{
  const std::optional<u32> address = ResolveMemoryReference(arguments);
  const std::optional<u32> count = ReadNumericFromJson<u32>(arguments, "count");
  if (!address || !count)
    return std::nullopt;

  ReadMemoryArguments result;
  result.address = *address;
  result.offset = ReadNumericFromJson<s64>(arguments, "offset").value_or(0);
  result.count = *count;
  return result;
}

std::optional<WriteMemoryArguments> ParseWriteMemory(const picojson::object& arguments)
{
  const std::optional<u32> address = ResolveMemoryReference(arguments);
  const std::optional<std::string> data = ReadStringFromJson(arguments, "data");
  if (!address || !data)
    return std::nullopt;

  const std::optional<std::vector<u8>> decoded = Json::Base64Decode(*data);
  if (!decoded)
    return std::nullopt;

  WriteMemoryArguments result;
  result.address = *address;
  result.offset = ReadNumericFromJson<s64>(arguments, "offset").value_or(0);
  result.allow_partial = ReadBoolFromJson(arguments, "allowPartial").value_or(false);
  result.data = *decoded;
  return result;
}

std::optional<DisassembleArguments> ParseDisassemble(const picojson::object& arguments)
{
  const std::optional<u32> address = ResolveMemoryReference(arguments);
  if (!address)
    return std::nullopt;

  DisassembleArguments result;
  result.address = *address;
  result.offset = ReadNumericFromJson<s64>(arguments, "offset").value_or(0);
  result.instruction_offset = ReadNumericFromJson<s64>(arguments, "instructionOffset").value_or(0);
  result.instruction_count = ReadNumericFromJson<u32>(arguments, "instructionCount").value_or(1);
  return result;
}

SetBreakpointsArguments ParseSetBreakpoints(const picojson::object& arguments)
{
  SetBreakpointsArguments result;
  result.base = ResolveSourceBase(arguments);

  const picojson::array* breakpoints = GetArray(arguments, "breakpoints");
  if (breakpoints == nullptr)
    return result;

  for (const picojson::value& entry : *breakpoints)
  {
    if (!entry.is<picojson::object>())
      continue;

    const picojson::object& entry_obj = entry.get<picojson::object>();
    RequestedBreakpoint breakpoint;

    // DESNOTE(jbarber, 2026-07-02): A line is only resolvable to an address
    // relative to the source base; without a base we leave the breakpoint
    // unresolved (verified=false) rather than inventing an address.
    if (const std::optional<u32> line = ReadNumericFromJson<u32>(entry_obj, "line"))
    {
      if (result.base)
        breakpoint.address = *result.base + *line * 4;
    }
    else if (result.base)
    {
      breakpoint.address = *result.base;
    }

    if (const std::optional<std::string> condition = ReadStringFromJson(entry_obj, "condition"))
      breakpoint.condition = *condition;

    result.breakpoints.push_back(breakpoint);
  }

  return result;
}

std::optional<SetVariableArguments> ParseSetVariable(const picojson::object& arguments)
{
  const std::optional<int> variables_reference =
      ReadNumericFromJson<int>(arguments, "variablesReference");
  const std::optional<std::string> name = ReadStringFromJson(arguments, "name");
  const std::optional<std::string> value = ReadStringFromJson(arguments, "value");
  if (!variables_reference || !name || !value)
    return std::nullopt;

  SetVariableArguments result;
  result.variables_reference = *variables_reference;
  result.name = *name;
  result.value = *value;
  return result;
}

SetDataBreakpointsArguments ParseSetDataBreakpoints(const picojson::object& arguments)
{
  SetDataBreakpointsArguments result;

  const picojson::array* breakpoints = GetArray(arguments, "breakpoints");
  if (breakpoints == nullptr)
    return result;

  for (const picojson::value& entry : *breakpoints)
  {
    if (!entry.is<picojson::object>())
      continue;

    const picojson::object& entry_obj = entry.get<picojson::object>();
    RequestedDataBreakpoint breakpoint;

    if (const std::optional<std::string> data_id = ReadStringFromJson(entry_obj, "dataId"))
    {
      if (const std::optional<u32> address = Json::ParseHexAddress(*data_id))
        breakpoint.address = *address;
    }

    // DESNOTE(jbarber, 2026-07-21): `length` is a Dolphin-specific extension to
    // DAP data breakpoints; spec-compliant clients omit it and get length 1
    // (single-byte watch). When present and > 1, the controller installs a
    // ranged watchpoint over the whole region.
    if (const std::optional<u32> length = ReadNumericFromJson<u32>(entry_obj, "length"))
      breakpoint.length = *length == 0 ? 1 : *length;

    if (const std::optional<std::string> access_type = ReadStringFromJson(entry_obj, "accessType"))
    {
      if (*access_type == "read")
      {
        breakpoint.read = true;
        breakpoint.write = false;
      }
      else if (*access_type == "write")
      {
        breakpoint.read = false;
        breakpoint.write = true;
      }
      else
      {
        breakpoint.read = true;
        breakpoint.write = true;
      }
    }

    if (const std::optional<std::string> condition = ReadStringFromJson(entry_obj, "condition"))
      breakpoint.condition = *condition;

    result.breakpoints.push_back(std::move(breakpoint));
  }

  return result;
}

std::optional<EvaluateArguments> ParseEvaluate(const picojson::object& arguments)
{
  const std::optional<std::string> expression = ReadStringFromJson(arguments, "expression");
  if (!expression)
    return std::nullopt;

  EvaluateArguments result;
  result.expression = *expression;
  return result;
}

SetInstructionBreakpointsArguments ParseSetInstructionBreakpoints(const picojson::object& arguments)
{
  SetInstructionBreakpointsArguments result;

  const picojson::array* breakpoints = GetArray(arguments, "breakpoints");
  if (breakpoints == nullptr)
    return result;

  for (const picojson::value& entry : *breakpoints)
  {
    if (!entry.is<picojson::object>())
      continue;

    const picojson::object& entry_obj = entry.get<picojson::object>();
    RequestedInstructionBreakpoint breakpoint;

    if (const std::optional<std::string> reference =
            ReadStringFromJson(entry_obj, "instructionReference"))
    {
      if (const std::optional<u32> base = Json::ParseHexAddress(*reference))
      {
        // DESNOTE(jbarber, 2026-07-21): The DAP `offset` is a signed byte
        // offset from `instructionReference`. Compute the effective address
        // in s64 so we can detect under/overflow -- a negative offset that
        // wraps below 0 or a positive one past u32 max means the client
        // asked for an address outside the PPC address space. Leave
        // `address` nullopt in that case rather than silently wrapping to
        // a nonsense PC; the session reports the unverified breakpoint to
        // the client so it can re-resolve.
        const s64 offset = ReadNumericFromJson<s64>(entry_obj, "offset").value_or(0);
        const s64 effective = static_cast<s64>(*base) + offset;
        if (effective >= 0 && effective <= static_cast<s64>(std::numeric_limits<u32>::max()))
          breakpoint.address = static_cast<u32>(effective);
      }
    }

    if (const std::optional<std::string> condition = ReadStringFromJson(entry_obj, "condition"))
      breakpoint.condition = *condition;

    result.breakpoints.push_back(std::move(breakpoint));
  }

  return result;
}

GotoTargetsArguments ParseGotoTargets(const picojson::object& arguments)
{
  GotoTargetsArguments result;

  const std::optional<u32> base = ResolveSourceBase(arguments);
  if (!base)
    return result;

  // DESNOTE(jbarber, 2026-07-03): Dolphin models a "source" as a code region
  // anchored at a hex address, so a goto line resolves to base + line*4, matching
  // setBreakpoints' line handling.
  const std::optional<u32> line = ReadNumericFromJson<u32>(arguments, "line");
  result.address = *base + line.value_or(0) * 4;
  return result;
}

std::optional<GotoArguments> ParseGoto(const picojson::object& arguments)
{
  const std::optional<int> thread_id = ReadNumericFromJson<int>(arguments, "threadId");
  const std::optional<s64> target = ReadNumericFromJson<s64>(arguments, "targetId");
  if (!thread_id || !target)
    return std::nullopt;

  GotoArguments result;
  result.thread_id = *thread_id;
  result.target = static_cast<u32>(*target);
  return result;
}

SourceRequestArguments ParseSourceRequest(const picojson::object& arguments)
{
  SourceRequestArguments result;
  result.base = ResolveSourceReference(arguments);
  result.start_line = ReadNumericFromJson<int>(arguments, "startLine").value_or(0);
  if (const std::optional<int> end_line = ReadNumericFromJson<int>(arguments, "endLine"))
    result.end_line = *end_line;
  return result;
}

BreakpointLocationsArguments ParseBreakpointLocations(const picojson::object& arguments)
{
  BreakpointLocationsArguments result;
  result.base = ResolveSourceReference(arguments);
  result.start_line = ReadNumericFromJson<int>(arguments, "line").value_or(0);
  if (const std::optional<int> end_line = ReadNumericFromJson<int>(arguments, "endLine"))
    result.end_line = *end_line;
  return result;
}

std::optional<RealtimeWatchArguments> ParseRealtimeWatch(const picojson::object& arguments)
{
  const std::optional<u32> address = ResolveMemoryReference(arguments);
  const std::optional<u32> count = ReadNumericFromJson<u32>(arguments, "count");
  if (!address || !count || *count == 0)
    return std::nullopt;

  RealtimeWatchArguments result;
  result.address = *address;
  result.count = *count;
  return result;
}

std::optional<RealtimeWatchCancelArguments>
ParseRealtimeWatchCancel(const picojson::object& arguments)
{
  const std::optional<int> watch_id = ReadNumericFromJson<int>(arguments, "watchId");
  if (!watch_id)
    return std::nullopt;

  RealtimeWatchCancelArguments result;
  result.watch_id = *watch_id;
  return result;
}

std::optional<FreezeArguments> ParseFreeze(const picojson::object& arguments)
{
  // DESNOTE(jbarber, 2026-07-21): `data` is base64-encoded per the DAP
  // `writeMemory` convention; `memoryReference` is the hex address. Two
  // mutually exclusive forms:
  //   - `watchId` present: freeze an existing watch (size will be checked
  //     against the subscription's `count` by RealtimeWatchSampler::Freeze).
  //   - `memoryReference` + `count` present: standalone freeze that creates
  //     a new subscription.
  // The `data` field is required in both forms.
  const std::optional<std::string> data = ReadStringFromJson(arguments, "data");
  if (!data)
    return std::nullopt;
  const std::optional<std::vector<u8>> decoded = Json::Base64Decode(*data);
  if (!decoded)
    return std::nullopt;

  const std::optional<int> watch_id = ReadNumericFromJson<int>(arguments, "watchId");
  const std::optional<u32> address = ResolveMemoryReference(arguments);
  const std::optional<u32> count = ReadNumericFromJson<u32>(arguments, "count");

  FreezeArguments result;
  result.value = std::move(*decoded);
  if (watch_id)
  {
    // Form 2: freeze existing. address/count must be absent; the sampler
    // authoritative source for the watch's width is its existing
    // subscription. We do allow them to be ignored if present (some clients
    // echo them back), but fail loudly if `data` doesn't match the watch.
    if (!address && !count)
    {
      result.watch_id = *watch_id;
      return result;
    }
    // Mixed form: address/count present alongside watchId. Reject -- the
    // client should pick one form or the other.
    return std::nullopt;
  }

  // Form 1: standalone freeze. Requires both address and count, and `data`
  // length must equal `count` exactly so the frozen canon covers the whole
  // watched region.
  if (!address || !count)
    return std::nullopt;
  if (result.value.size() != *count)
    return std::nullopt;
  if (*count == 0 || *address > std::numeric_limits<u32>::max() - *count)
    return std::nullopt;

  result.address = *address;
  result.count = *count;
  return result;
}

std::optional<UnfreezeArguments> ParseUnfreeze(const picojson::object& arguments)
{
  const std::optional<int> watch_id = ReadNumericFromJson<int>(arguments, "watchId");
  if (!watch_id)
    return std::nullopt;

  UnfreezeArguments result;
  result.watch_id = *watch_id;
  return result;
}

std::optional<FindFreeMemoryArguments> ParseFindFreeMemory(const picojson::object& arguments)
{
  const std::optional<u32> count = ReadNumericFromJson<u32>(arguments, "count");
  if (!count || *count == 0)
    return std::nullopt;

  FindFreeMemoryArguments result;
  result.count = *count;
  return result;
}

std::optional<InjectCodeArguments> ParseInjectCode(const picojson::object& arguments)
{
  // DESNOTE(jbarber, 2026-07-21): `code` is base64-encoded PPC machine code.
  // `memoryReference` is optional -- when absent, the server allocates a
  // region via FindFreeMemory; when present, it writes at that address.
  const std::optional<std::string> data = ReadStringFromJson(arguments, "code");
  if (!data)
    return std::nullopt;
  const std::optional<std::vector<u8>> decoded = Json::Base64Decode(*data);
  if (!decoded || decoded->empty())
    return std::nullopt;
  // Instructions are 4 bytes. Allow non-multiple-of-4 lengths for integrators
  // who pass trailing data or hand-crafted trampolines; the server won't
  // complain but PC alignment will be on them.
  if (decoded->size() % 4u != 0u)
    return std::nullopt;

  InjectCodeArguments result;
  result.address = ResolveMemoryReference(arguments);
  result.code = std::move(*decoded);
  return result;
}

std::optional<DetourArguments> ParseDetour(const picojson::object& arguments)
{
  // Required: target_address (the 4-byte instruction being detoured) and
  // detour_body (base64). Optional: detour_address (where the detour lives;
  // allocated via FindFreeMemory if omitted).
  const std::optional<u32> target_address = ResolveMemoryReference(arguments);
  if (!target_address)
    return std::nullopt;
  const std::optional<std::string> data = ReadStringFromJson(arguments, "detourBody");
  if (!data)
    return std::nullopt;
  const std::optional<std::vector<u8>> decoded = Json::Base64Decode(*data);
  if (!decoded || decoded->empty() || decoded->size() % 4u != 0u)
    return std::nullopt;

  DetourArguments result;
  result.target_address = *target_address;
  // detourAddress is optional (allocated by the server when absent). It's a
  // hex string like "0x8000C000" despite the camelCase-without-"Reference"
  // name -- we explicitly parse "0x..." since ResolveMemoryReference only
  // knows about the "memoryReference" key.
  if (const std::optional<std::string> detour_ref = ReadStringFromJson(arguments, "detourAddress"))
  {
    const std::optional<u32> parsed = Json::ParseHexAddress(*detour_ref);
    if (!parsed)
      return std::nullopt;
    result.detour_address = *parsed;
  }
  result.detour_body = std::move(*decoded);
  return result;
}

std::optional<LaunchArguments> ParseLaunch(const picojson::object& arguments)
{
  // DESNOTE(jbarber, 2026-07-21): `stopOnEntry` is optional; nullopt means
  // "client didn't say" so the session can fall back to the
  // `Dolphin.General.DAPStopOnEntry` config default. A client explicitly
  // writing true/false overrides the config for this session.
  LaunchArguments result;
  result.stop_on_entry = ReadBoolFromJson(arguments, "stopOnEntry");
  return result;
}

picojson::object MakeResponse(const int seq, const int request_seq, const std::string_view command,
                              const bool success, picojson::object body)
{
  picojson::object response;
  response.emplace("seq", static_cast<double>(seq));
  response.emplace("type", std::string("response"));
  response.emplace("request_seq", static_cast<double>(request_seq));
  response.emplace("command", std::string(command));
  response.emplace("success", success);
  response.emplace("body", std::move(body));
  return response;
}

picojson::object MakeErrorResponse(const int seq, const int request_seq,
                                   const std::string_view command, const std::string_view message)
{
  picojson::object response;
  response.emplace("seq", static_cast<double>(seq));
  response.emplace("type", std::string("response"));
  response.emplace("request_seq", static_cast<double>(request_seq));
  response.emplace("command", std::string(command));
  response.emplace("success", false);
  response.emplace("message", std::string(message));
  return response;
}

picojson::object MakeEvent(const int seq, const std::string_view event, picojson::object body)
{
  picojson::object envelope;
  envelope.emplace("seq", static_cast<double>(seq));
  envelope.emplace("type", std::string("event"));
  envelope.emplace("event", std::string(event));
  envelope.emplace("body", std::move(body));
  return envelope;
}

std::string Serialize(const picojson::object& object)
{
  return picojson::value(object).serialize();
}
}  // namespace DAP::Protocol

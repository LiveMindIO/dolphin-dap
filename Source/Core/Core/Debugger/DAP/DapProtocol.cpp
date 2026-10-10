// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapProtocol.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Common/JsonUtil.h"
#include "Core/Debugger/DAP/DapJson.h"

namespace DAP::Protocol
{
std::optional<SteppingGranularity> ParseSteppingGranularity(const picojson::object& arguments)
{
  if (arguments.contains("threadId"))
  {
    const std::optional<int> thread_id = ReadNumericFromJson<int>(arguments, "threadId");
    if (!thread_id || *thread_id != 1)
      return std::nullopt;
  }
  const std::string granularity =
      ReadStringFromJson(arguments, "granularity").value_or("statement");
  if (granularity == "statement")
    return SteppingGranularity::Statement;
  if (granularity == "line")
    return SteppingGranularity::Line;
  if (granularity == "instruction")
    return SteppingGranularity::Instruction;
  return std::nullopt;
}

namespace
{
using DAP::MemoryScanDataType;
using DAP::MemoryScanFilter;

constexpr size_t MAX_BYTE_PATTERN_SIZE = 4096;
constexpr size_t MAX_ENCODED_BYTE_PATTERN_SIZE = ((MAX_BYTE_PATTERN_SIZE + 2) / 3) * 4;
constexpr size_t MAX_SCAN_RANGES = 1024;
constexpr size_t MAX_SCAN_REGIONS = 3;
constexpr size_t MAX_NUMERIC_VALUE_LENGTH = 128;

std::optional<std::vector<u8>> DecodeBytePattern(std::string_view value)
{
  if (value.empty() || value.size() > MAX_ENCODED_BYTE_PATTERN_SIZE)
    return std::nullopt;
  const std::optional<std::vector<u8>> decoded = Json::Base64Decode(value);
  if (!decoded || decoded->empty() || decoded->size() > MAX_BYTE_PATTERN_SIZE ||
      Json::Base64Encode(*decoded) != value)
  {
    return std::nullopt;
  }
  return decoded;
}

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

template <typename T>
std::optional<T> ReadStrictUnsignedInteger(const picojson::object& obj, const std::string& key)
{
  constexpr double max_exact_json_integer = 9007199254740991.0;
  const auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<double>())
    return std::nullopt;
  const double value = it->second.get<double>();
  if (!std::isfinite(value) || value < 0 || std::floor(value) != value ||
      value > std::min(static_cast<double>(std::numeric_limits<T>::max()), max_exact_json_integer))
  {
    return std::nullopt;
  }
  return static_cast<T>(value);
}

template <typename T>
std::optional<T> ReadStrictSignedInteger(const picojson::value& input)
{
  if (!input.is<double>())
    return std::nullopt;
  const double value = input.get<double>();
  if (!std::isfinite(value) || std::floor(value) != value ||
      value < static_cast<double>(std::numeric_limits<T>::min()) ||
      value > static_cast<double>(std::numeric_limits<T>::max()))
  {
    return std::nullopt;
  }
  return static_cast<T>(value);
}

bool HasInvalidOptionalString(const picojson::object& obj, const std::string& key)
{
  const auto it = obj.find(key);
  return it != obj.end() && !it->second.is<std::string>();
}

bool HasInvalidOptionalBool(const picojson::object& obj, const std::string& key)
{
  const auto it = obj.find(key);
  return it != obj.end() && !it->second.is<bool>();
}

std::optional<u32> ResolveSourceObjectBase(const picojson::object& source)
{
  if (const std::optional<std::string> name = ReadStringFromJson(source, "name"))
  {
    if (const std::optional<u32> base = Json::ParseHexAddress(*name))
      return base;
  }
  if (const std::optional<std::string> path = ReadStringFromJson(source, "path"))
  {
    if (const std::optional<u32> base = Json::ParseHexAddress(*path))
      return base;
  }
  return std::nullopt;
}

// Resolve a Dolphin "source" object to a base address. A source is anchored at
// an address encoded as a hex string in either `name` or `path`.
std::optional<u32> ResolveSourceBase(const picojson::object& arguments)
{
  const picojson::object* source = GetObject(arguments, "source");
  return source ? ResolveSourceObjectBase(*source) : std::nullopt;
}

std::optional<u32> ResolveMemoryReference(const picojson::object& arguments)
{
  const std::optional<std::string> reference = ReadStringFromJson(arguments, "memoryReference");
  if (!reference)
    return std::nullopt;
  return Json::ParseHexAddress(*reference);
}

std::optional<SourceReference> ResolveSourceReference(const picojson::object& arguments)
{
  const auto read_reference = [](const picojson::object& object) -> std::optional<SourceReference> {
    const std::optional<SourceReference> reference =
        ReadStrictUnsignedInteger<SourceReference>(object, "sourceReference");
    if (!reference || *reference == 0 ||
        *reference > MakeDisassemblySourceReference(std::numeric_limits<u32>::max()))
      return std::nullopt;
    return reference;
  };

  if (const std::optional<SourceReference> reference = read_reference(arguments))
    return reference;
  const picojson::object* source = GetObject(arguments, "source");
  if (source == nullptr)
    return std::nullopt;
  if (const std::optional<SourceReference> reference = read_reference(*source))
    return reference;
  if (const picojson::object* adapter_data = GetObject(*source, "adapterData"))
  {
    const std::optional<u32> source_id =
        ReadStrictUnsignedInteger<u32>(*adapter_data, "dolphinSourceId");
    if (source_id && *source_id > 0)
      return *source_id;
  }
  if (const std::optional<u32> base = ResolveSourceObjectBase(*source))
    return MakeDisassemblySourceReference(*base);
  return std::nullopt;
}

std::optional<DAP::MemoryScanDataType> ParseMemoryScanDataType(std::string_view type)
{
  if (type == "u8")
    return MemoryScanDataType::U8;
  if (type == "u16")
    return MemoryScanDataType::U16;
  if (type == "u32")
    return MemoryScanDataType::U32;
  if (type == "u64")
    return MemoryScanDataType::U64;
  if (type == "s8")
    return MemoryScanDataType::S8;
  if (type == "s16")
    return MemoryScanDataType::S16;
  if (type == "s32")
    return MemoryScanDataType::S32;
  if (type == "s64")
    return MemoryScanDataType::S64;
  if (type == "f32")
    return MemoryScanDataType::F32;
  if (type == "f64")
    return MemoryScanDataType::F64;
  if (type == "bytes")
    return MemoryScanDataType::Bytes;
  if (type == "string")
    return MemoryScanDataType::String;
  if (type == "ppcInstruction")
    return MemoryScanDataType::PpcInstruction;
  return std::nullopt;
}

std::optional<DAP::MemoryScanFilter> ParseMemoryScanFilter(std::string_view filter)
{
  if (filter == "exact")
    return MemoryScanFilter::Exact;
  if (filter == "notEqual")
    return MemoryScanFilter::NotEqual;
  if (filter == "between")
    return MemoryScanFilter::Between;
  if (filter == "greaterThan")
    return MemoryScanFilter::GreaterThan;
  if (filter == "greaterOrEqual")
    return MemoryScanFilter::GreaterOrEqual;
  if (filter == "lessThan")
    return MemoryScanFilter::LessThan;
  if (filter == "lessOrEqual")
    return MemoryScanFilter::LessOrEqual;
  if (filter == "unknown")
    return MemoryScanFilter::Unknown;
  if (filter == "changed")
    return MemoryScanFilter::Changed;
  if (filter == "unchanged")
    return MemoryScanFilter::Unchanged;
  if (filter == "increased")
    return MemoryScanFilter::Increased;
  if (filter == "decreased")
    return MemoryScanFilter::Decreased;
  if (filter == "increasedBy")
    return MemoryScanFilter::IncreasedBy;
  if (filter == "decreasedBy")
    return MemoryScanFilter::DecreasedBy;
  if (filter == "mnemonic")
    return MemoryScanFilter::Mnemonic;
  if (filter == "validInstruction")
    return MemoryScanFilter::ValidInstruction;
  return std::nullopt;
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

  // DESNOTE(jbarber, 2026-07-22): Cap `count` at 1 MiB (1048576 bytes) so a
  // pathological client can't request a multi-GB read that would exhaust
  // memory on the session thread (ReadMemory reserves `count` bytes in a
  // std::vector and iterates them under CPUThreadGuard). 1 MiB is generous
  // — typical debug reads are <1 KiB, and clients paging through larger
  // regions can make repeated requests. Mirrors the instructionCount cap on
  // `disassemble` (65536). Bugbot #58.
  // Record the original requested count so HandleReadMemory can report the
  // capped bytes as `unreadableBytes` in the DAP response, rather than
  // silently truncating. A client that asked for 100 MiB and got 1 MiB back
  // needs to know the response was capped so it can re-issue smaller reads
  // instead of treating the partial payload as the full region. Bugbot #65.
  constexpr u32 kMaxReadMemoryBytes = 1u << 20;  // 1 MiB

  ReadMemoryArguments result;
  result.address = *address;
  result.offset = ReadNumericFromJson<s64>(arguments, "offset").value_or(0);
  result.requested_count = *count;
  result.count = std::min(*count, kMaxReadMemoryBytes);
  return result;
}

std::optional<WriteMemoryArguments> ParseWriteMemory(const picojson::object& arguments)
{
  const std::optional<u32> address = ResolveMemoryReference(arguments);
  const std::optional<std::string> data = ReadStringFromJson(arguments, "data");
  if (!address || !data)
    return std::nullopt;

  // DESNOTE(jbarber, 2026-07-22): Cap the decoded payload at 1 MiB so a
  // pathological client can't ship a multi-GB base64 body that decodes into
  // a multi-GB allocation on the session thread. Mirrors the kMaxReadMemoryBytes
  // cap on ParseReadMemory (Bugbot #58) and the 65536 cap on disassemble.
  // Truncate silently rather than rejecting: a write that exceeds the cap
  // still partially applies (the client can split oversized writes itself);
  // a hard reject would discard bytes the client reasonably expected to
  // land. Bugbot #60.
  // The decoded buffer allocation in Base64Decode is bounded at ~12 MiB by
  // the framing Content-Length cap (kMaxContentLength = 16 MiB), so the
  // pre-decode allocation can't balloon to multi-GB even without a separate
  // check here. Bugbot #64.
  constexpr std::size_t kMaxWriteMemoryBytes = 1u << 20;  // 1 MiB

  const std::optional<std::vector<u8>> decoded = Json::Base64Decode(*data);
  if (!decoded)
    return std::nullopt;

  std::vector<u8> capped = *decoded;
  if (capped.size() > kMaxWriteMemoryBytes)
    capped.resize(kMaxWriteMemoryBytes);

  WriteMemoryArguments result;
  result.address = *address;
  result.offset = ReadNumericFromJson<s64>(arguments, "offset").value_or(0);
  result.allow_partial = ReadBoolFromJson(arguments, "allowPartial").value_or(false);
  result.data = std::move(capped);
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
  // DESNOTE(jbarber, 2026-07-21): Cap instruction_count at 65536. Without
  // a bound a single disassemble request could force millions of MMU
  // reads + disassembly steps on the session thread, stalling all other
  // DAP traffic. 65536 instructions is far beyond anything a real debugger
  // view needs (a 4096-instruction viewport is already overkill) but
  // keeps bulk-dump commands usable. The cap mirrors the GetSource /
  // GetBreakpointLocations iteration limits.
  result.instruction_count = std::min(
      ReadNumericFromJson<u32>(arguments, "instructionCount").value_or(1), static_cast<u32>(65536));
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
      {
        // Compute in 64-bit so a wildly-large `line` produces a non-resolvable
        // nullopt address rather than silently wrapping past u32 max and
        // installing a breakpoint at a nonsense PC.
        if (*line > 0)
        {
          const u64 effective =
              static_cast<u64>(*result.base) + (static_cast<u64>(*line) - 1ull) * 4ull;
          if (effective <= static_cast<u64>(std::numeric_limits<u32>::max()))
            breakpoint.address = static_cast<u32>(effective);
        }
      }
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

  std::optional<u32> base;
  if (const std::optional<SourceReference> reference = ResolveSourceReference(arguments))
  {
    base = DecodeDisassemblySourceReference(*reference);
    if (!base && *reference <= std::numeric_limits<u32>::max())
      base = static_cast<u32>(*reference);
  }
  if (!base)
    return result;

  // DESNOTE(jbarber, 2026-07-03): Dolphin models a "source" as a code region
  // anchored at a hex address, so a goto line resolves to base + (line-1)*4,
  // matching setBreakpoints' line handling. Compute in u64 so an absurd
  // `line` value yields an out-of-range address (left as nullopt) instead of
  // silently wrapping the source base to a destination that points elsewhere.
  const std::optional<u32> line = ReadNumericFromJson<u32>(arguments, "line");
  if (!line || *line == 0)
    return result;
  result.line = static_cast<int>(*line);
  const u64 effective = static_cast<u64>(*base) + (static_cast<u64>(*line) - 1ull) * 4ull;
  if (effective <= static_cast<u64>(std::numeric_limits<u32>::max()))
    result.address = static_cast<u32>(effective);
  return result;
}

std::optional<GotoArguments> ParseGoto(const picojson::object& arguments)
{
  const std::optional<int> thread_id = ReadNumericFromJson<int>(arguments, "threadId");
  const std::optional<s64> target = ReadNumericFromJson<s64>(arguments, "targetId");
  if (!thread_id || !target)
    return std::nullopt;

  // DESNOTE(jbarber, 2026-07-21): Reject negative or > u32-max targetIds
  // instead of `static_cast<u32>` wrapping them. A negative or huge id would
  // otherwise land the PC at a wrapped address -- silently jumping the
  // debugger to an unintended PC while reporting success.
  if (*target < 0 || *target > static_cast<s64>(std::numeric_limits<u32>::max()))
    return std::nullopt;

  GotoArguments result;
  result.thread_id = *thread_id;
  result.target = static_cast<u32>(*target);
  return result;
}

SourceRequestArguments ParseSourceRequest(const picojson::object& arguments)
{
  SourceRequestArguments result;
  result.source_reference = ResolveSourceReference(arguments);
  result.start_line = ReadNumericFromJson<int>(arguments, "startLine").value_or(0);
  if (const std::optional<int> end_line = ReadNumericFromJson<int>(arguments, "endLine"))
    result.end_line = *end_line;
  return result;
}

BreakpointLocationsArguments ParseBreakpointLocations(const picojson::object& arguments)
{
  BreakpointLocationsArguments result;
  result.source_reference = ResolveSourceReference(arguments);
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

  // DESNOTE(jbarber, 2026-07-22): Cap `count` at 1 MiB so a pathological
  // client can't subscribe to multi-MB regions that would allocate multi-MB
  // buffers per subscription AND force a full-region seed read in
  // AddSubscription (and a full Tick() re-read at field rate). Mirrors the
  // readMemory/writeMemory caps (#58, #60). A 1 MiB realtime watch is far
  // beyond any realistic use case (games typically pin individual fields
  // of a few bytes); clients watching bigger regions should poll
  // periodically with readMemory. Bugbot #73.
  constexpr u32 kMaxRealtimeWatchBytes = 1u << 20;  // 1 MiB
  const u32 capped_count = std::min(*count, kMaxRealtimeWatchBytes);

  RealtimeWatchArguments result;
  result.address = *address;
  result.count = capped_count;
  result.requested_count = *count;
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
  // DESNOTE(jbarber, 2026-07-22): Cap the decoded payload at 1 MiB so a
  // pathological client can't ship a multi-GB base64 body that decodes into
  // a multi-GB allocation on the session thread. Matches the
  // kMaxWriteMemoryBytes cap on writeMemory. RealtimeWatchSampler::Freeze
  // separately enforces `value.size() == subscription.count` (which is now
  // also capped at 1 MiB), so a freeze request against an existing watch
  // with an oversized payload would reject naturally; for the standalone
  // form this is the first line of defense. Bugbot #73 (sibling to #60).
  constexpr std::size_t kMaxFreezeBytes = 1u << 20;  // 1 MiB
  std::vector<u8> capped_value = *decoded;
  if (capped_value.size() > kMaxFreezeBytes)
    capped_value.resize(kMaxFreezeBytes);

  const std::optional<int> watch_id = ReadNumericFromJson<int>(arguments, "watchId");
  const std::optional<u32> address = ResolveMemoryReference(arguments);
  const std::optional<u32> count = ReadNumericFromJson<u32>(arguments, "count");

  FreezeArguments result;
  result.value = std::move(capped_value);
  if (watch_id)
  {
    // DESNOTE(jbarber, 2026-07-21): Form 2 -- freeze an existing watch by id.
    // The sampler is the authoritative source for the watch's width (its
    // existing subscription), so address/count are not consulted here. Some
    // DAP clients echo the watch's memoryReference/count back on the freeze
    // request; the comment promised to ignore them, but the previous form
    // (`!address && !count`) rejected the request whenever either was
    // present, so an echoed-field client got "invalid arguments" instead
    // of a frozen watch. Accept watchId alone; any echoed address/count
    // is silently dropped.
    result.watch_id = *watch_id;
    return result;
  }

  // Form 1: standalone freeze. Requires both address and count, and `data`
  // length must equal `count` exactly so the frozen canon covers the whole
  // watched region.
  if (!address || !count)
    return std::nullopt;
  if (result.value.size() != *count)
    return std::nullopt;
  // DESNOTE(jbarber, 2026-07-21): Off-by-one fix. Reject only when the last
  // byte (address + count - 1) exceeds UINT32_MAX, so a 1-byte freeze at
  // 0xFFFFFFFF is accepted instead of rejected as overflow.
  if (*count == 0 || (*count - 1u) > (std::numeric_limits<u32>::max() - *address))
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
  // DESNOTE(jbarber, 2026-07-26): Cap decoded payload at 1 MiB — the 16 MiB
  // framing limit allows requests that decode to ~12 MiB, allocating that
  // much on the session thread before the write even starts. Mirrors the
  // readMemory/writeMemory/freeze caps. Bugbot #78.
  constexpr std::size_t kMaxInjectCodeBytes = 1u << 20;  // 1 MiB
  if (decoded->size() > kMaxInjectCodeBytes)
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
  // DESNOTE(jbarber, 2026-07-26): Cap decoded payload at 1 MiB. Bugbot #78.
  constexpr std::size_t kMaxDetourBodyBytes = 1u << 20;  // 1 MiB
  if (decoded->size() > kMaxDetourBodyBytes)
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

std::optional<ResolvePointerChainArguments>
ParseResolvePointerChain(const picojson::object& arguments)
{
  const std::optional<std::string> base = ReadStringFromJson(arguments, "baseAddress");
  const picojson::array* offsets = GetArray(arguments, "offsets");
  if (!base || offsets == nullptr || offsets->empty() || offsets->size() > 64)
    return std::nullopt;
  const std::optional<u32> parsed_base = Json::ParseHexAddress(*base);
  if (!parsed_base)
    return std::nullopt;

  ResolvePointerChainArguments result;
  result.base_address = *parsed_base;
  result.offsets.reserve(offsets->size());
  for (const picojson::value& input : *offsets)
  {
    const std::optional<s32> offset = ReadStrictSignedInteger<s32>(input);
    if (!offset)
      return std::nullopt;
    result.offsets.push_back(*offset);
  }
  return result;
}

std::optional<MemoryScanStartConfig> ParseMemoryScanStart(const picojson::object& arguments)
{
  const std::optional<std::string> type = ReadStringFromJson(arguments, "dataType");
  const std::optional<std::string> filter_name = ReadStringFromJson(arguments, "filter");
  if (!type || !filter_name)
    return std::nullopt;
  const std::optional<MemoryScanDataType> data_type = ParseMemoryScanDataType(*type);
  const std::optional<MemoryScanFilter> filter = ParseMemoryScanFilter(*filter_name);
  if (!data_type || !filter)
    return std::nullopt;
  if (HasInvalidOptionalString(arguments, "value") ||
      HasInvalidOptionalString(arguments, "value2") ||
      HasInvalidOptionalString(arguments, "encoding") ||
      HasInvalidOptionalBool(arguments, "aligned") ||
      HasInvalidOptionalBool(arguments, "caseSensitive") ||
      HasInvalidOptionalBool(arguments, "pauseDuringScan"))
  {
    return std::nullopt;
  }

  MemoryScanStartConfig result;
  result.data_type = *data_type;
  result.filter = *filter;
  result.value = ReadStringFromJson(arguments, "value");
  result.value2 = ReadStringFromJson(arguments, "value2");
  if (((result.data_type != MemoryScanDataType::Bytes &&
        result.data_type != MemoryScanDataType::String) &&
       result.value && result.value->size() > MAX_NUMERIC_VALUE_LENGTH) ||
      (result.value2 && result.value2->size() > MAX_NUMERIC_VALUE_LENGTH))
  {
    return std::nullopt;
  }
  result.aligned = ReadBoolFromJson(arguments, "aligned").value_or(true);
  result.pause_during_scan = ReadBoolFromJson(arguments, "pauseDuringScan").value_or(false);
  if (result.data_type != MemoryScanDataType::String &&
      (arguments.contains("encoding") || arguments.contains("caseSensitive")))
  {
    return std::nullopt;
  }
  if (result.data_type == MemoryScanDataType::PpcInstruction && !result.aligned)
    return std::nullopt;
  if (result.data_type == MemoryScanDataType::Bytes)
  {
    if (!result.value)
      return std::nullopt;
    const std::optional<std::vector<u8>> decoded = DecodeBytePattern(*result.value);
    if (!decoded)
      return std::nullopt;
    result.byte_value = *decoded;
  }
  else if (result.data_type == MemoryScanDataType::String)
  {
    if (!result.value || result.value->empty() || result.value->size() > MAX_BYTE_PATTERN_SIZE)
      return std::nullopt;
    const std::string encoding = ReadStringFromJson(arguments, "encoding").value_or("utf8");
    if (encoding != "utf8" && encoding != "ascii")
      return std::nullopt;
    if (encoding == "ascii" &&
        std::ranges::any_of(*result.value, [](unsigned char c) { return c > 0x7f; }))
    {
      return std::nullopt;
    }
    result.byte_value.assign(result.value->begin(), result.value->end());
    result.string_encoding =
        encoding == "ascii" ? MemoryScanStringEncoding::Ascii : MemoryScanStringEncoding::Utf8;
    result.case_sensitive = ReadBoolFromJson(arguments, "caseSensitive").value_or(true);
  }

  const picojson::array* regions = GetArray(arguments, "regions");
  if (arguments.contains("regions") && regions == nullptr)
    return std::nullopt;
  if (regions != nullptr)
  {
    if (regions->size() > MAX_SCAN_REGIONS)
      return std::nullopt;
    for (const picojson::value& region : *regions)
    {
      if (!region.is<std::string>())
        return std::nullopt;
      result.regions.push_back(region.get<std::string>());
    }
  }

  const picojson::array* ranges = GetArray(arguments, "ranges");
  if (arguments.contains("ranges") && ranges == nullptr)
    return std::nullopt;
  if (ranges != nullptr)
  {
    if (ranges->size() > MAX_SCAN_RANGES)
      return std::nullopt;
    for (const picojson::value& range_value : *ranges)
    {
      if (!range_value.is<picojson::object>())
        return std::nullopt;
      const picojson::object& range = range_value.get<picojson::object>();
      const std::optional<std::string> start = ReadStringFromJson(range, "start");
      const std::optional<std::string> end = ReadStringFromJson(range, "end");
      if (!start || !end)
        return std::nullopt;
      const std::optional<u32> parsed_start = Json::ParseHexAddress(*start);
      const std::optional<u32> parsed_end = Json::ParseHexAddress(*end);
      if (!parsed_start || !parsed_end)
        return std::nullopt;
      result.ranges.push_back({*parsed_start, *parsed_end});
    }
  }
  return result;
}

std::optional<MemoryScanRefineConfig> ParseMemoryScanRefine(const picojson::object& arguments)
{
  const std::optional<int> scan_id = ReadStrictUnsignedInteger<int>(arguments, "scanId");
  const std::optional<std::string> filter_name = ReadStringFromJson(arguments, "filter");
  if (!scan_id || *scan_id <= 0 || !filter_name)
    return std::nullopt;
  const std::optional<MemoryScanFilter> filter = ParseMemoryScanFilter(*filter_name);
  if (!filter || *filter == MemoryScanFilter::Unknown)
    return std::nullopt;
  if (HasInvalidOptionalString(arguments, "value") ||
      HasInvalidOptionalString(arguments, "value2") ||
      HasInvalidOptionalBool(arguments, "pauseDuringScan"))
  {
    return std::nullopt;
  }

  MemoryScanRefineConfig result;
  result.scan_id = *scan_id;
  result.filter = *filter;
  result.value = ReadStringFromJson(arguments, "value");
  result.value2 = ReadStringFromJson(arguments, "value2");
  if ((result.value && result.value->size() > MAX_ENCODED_BYTE_PATTERN_SIZE) ||
      (result.value2 && result.value2->size() > MAX_ENCODED_BYTE_PATTERN_SIZE))
  {
    return std::nullopt;
  }
  result.pause_during_scan = ReadBoolFromJson(arguments, "pauseDuringScan");
  return result;
}

std::optional<MemoryScanStatusArguments> ParseMemoryScanStatus(const picojson::object& arguments)
{
  const std::optional<int> scan_id = ReadStrictUnsignedInteger<int>(arguments, "scanId");
  if (!scan_id || *scan_id <= 0)
    return std::nullopt;
  return MemoryScanStatusArguments{*scan_id};
}

std::optional<MemoryScanResultsArguments> ParseMemoryScanResults(const picojson::object& arguments)
{
  const std::optional<int> scan_id = ReadStrictUnsignedInteger<int>(arguments, "scanId");
  if (!scan_id || *scan_id <= 0)
    return std::nullopt;
  MemoryScanResultsArguments result;
  result.scan_id = *scan_id;
  if (arguments.contains("start"))
  {
    const std::optional<u64> start = ReadStrictUnsignedInteger<u64>(arguments, "start");
    if (!start)
      return std::nullopt;
    result.start = *start;
  }
  if (arguments.contains("count"))
  {
    const std::optional<u32> count = ReadStrictUnsignedInteger<u32>(arguments, "count");
    if (!count)
      return std::nullopt;
    result.count = std::min(*count, 4096u);
  }
  else
  {
    result.count = 256;
  }
  return result;
}

std::optional<MemoryScanRemoveResultsArguments>
ParseMemoryScanRemoveResults(const picojson::object& arguments)
{
  const std::optional<int> scan_id = ReadStrictUnsignedInteger<int>(arguments, "scanId");
  const picojson::array* addresses = GetArray(arguments, "addresses");
  if (!scan_id || *scan_id <= 0 || addresses == nullptr || addresses->empty() ||
      addresses->size() > 4096)
  {
    return std::nullopt;
  }

  MemoryScanRemoveResultsArguments result;
  result.scan_id = *scan_id;
  result.addresses.reserve(addresses->size());
  for (const picojson::value& input : *addresses)
  {
    if (!input.is<std::string>())
      return std::nullopt;
    const std::optional<u32> address = Json::ParseHexAddress(input.get<std::string>());
    if (!address)
      return std::nullopt;
    result.addresses.push_back(*address);
  }
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

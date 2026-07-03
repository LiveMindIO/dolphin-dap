// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapProtocol.h"

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

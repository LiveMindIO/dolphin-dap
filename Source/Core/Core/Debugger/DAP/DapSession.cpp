// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapSession.h"

#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Core/Debugger/DAP/DapTransport.h"

namespace DAP
{
namespace
{
std::optional<int> ExtractIntField(std::string_view json, std::string_view field)
{
  const std::string needle = fmt::format("\"{}\":", field);
  const size_t pos = json.find(needle);
  if (pos == std::string_view::npos)
    return std::nullopt;

  size_t index = pos + needle.size();
  while (index < json.size() && (json[index] == ' ' || json[index] == '\t'))
    ++index;

  bool negative = false;
  if (index < json.size() && json[index] == '-')
  {
    negative = true;
    ++index;
  }

  int value = 0;
  bool any = false;
  while (index < json.size() && json[index] >= '0' && json[index] <= '9')
  {
    value = value * 10 + (json[index] - '0');
    ++index;
    any = true;
  }

  if (!any)
    return std::nullopt;

  return negative ? -value : value;
}

std::optional<std::string_view> ExtractStringField(std::string_view json, std::string_view field)
{
  const std::string needle = fmt::format("\"{}\":", field);
  const size_t pos = json.find(needle);
  if (pos == std::string_view::npos)
    return std::nullopt;

  size_t index = pos + needle.size();
  while (index < json.size() && (json[index] == ' ' || json[index] == '\t'))
    ++index;

  if (index >= json.size() || json[index] != '"')
    return std::nullopt;

  ++index;
  const size_t end = json.find('"', index);
  if (end == std::string_view::npos)
    return std::nullopt;

  return json.substr(index, end - index);
}

std::string MakeInitializeResponse(int request_seq)
{
  return fmt::format(
      R"({{"seq":1,"type":"response","request_seq":{},"command":"initialize","success":true,"body":{{"capabilities":{{"supportsConfigurationDoneRequest":true}},"serverInfo":{{"name":"Dolphin DAP","version":"{}"}}}}}})",
      request_seq, Common::GetScmRevGitStr());
}

std::string MakeConfigurationDoneResponse(int request_seq)
{
  return fmt::format(
      R"({{"seq":2,"type":"response","request_seq":{},"command":"configurationDone","success":true,"body":{{}}}})",
      request_seq);
}
}  // namespace

bool RunHandshake(DapTransport& transport)
{
  bool initialize_done = false;

  while (const std::optional<std::string> message = transport.ReadMessage())
  {
    if (message->empty())
      continue;

    const std::string_view json = *message;
    const std::optional<std::string_view> command = ExtractStringField(json, "command");
    const std::optional<int> request_seq = ExtractIntField(json, "seq");

    if (!command || !request_seq)
    {
      WARN_LOG_FMT(CONSOLE, "DAP: ignoring malformed message: {}", json);
      continue;
    }

    if (*command == "initialize")
    {
      if (!transport.WriteMessage(MakeInitializeResponse(*request_seq)))
        return false;

      initialize_done = true;
      INFO_LOG_FMT(CONSOLE, "DAP: initialize handshake complete.");
    }
    else if (*command == "configurationDone")
    {
      if (!initialize_done)
      {
        WARN_LOG_FMT(CONSOLE, "DAP: configurationDone received before initialize.");
        return false;
      }

      if (!transport.WriteMessage(MakeConfigurationDoneResponse(*request_seq)))
        return false;

      INFO_LOG_FMT(CONSOLE, "DAP: configurationDone handshake complete.");
      return true;
    }
    else
    {
      WARN_LOG_FMT(CONSOLE, "DAP: unexpected command during handshake: {}", *command);
    }
  }

  return false;
}
}  // namespace DAP

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapSession.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <WinSock2.h>
#else
#include <sys/select.h>
#endif

#include <fmt/format.h>

#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapDebugController.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/Debugger/DAP/DapTransport.h"
#include "Core/HW/CPU.h"
#include "Core/System.h"

namespace DAP
{
namespace
{
constexpr int kRegistersScope = 1000;
constexpr int kPcScope = 1001;

bool WaitForReadable(int socket, int timeout_ms)
{
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(socket, &readfds);

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  return select(socket + 1, &readfds, nullptr, nullptr, &tv) > 0;
}

std::vector<u32> ParseBreakpointAddresses(std::string_view json)
{
  std::vector<u32> addresses;

  std::optional<u32> base;
  if (const size_t source_pos = json.find("\"source\"");
      source_pos != std::string_view::npos)
  {
    const std::string_view source_json = json.substr(source_pos);
    if (const std::optional<std::string_view> name = Json::ExtractStringField(source_json, "name"))
      base = Json::ParseHexAddress(*name);
    if (!base)
    {
      if (const std::optional<std::string_view> path =
              Json::ExtractStringField(source_json, "path"))
        base = Json::ParseHexAddress(*path);
    }
  }

  size_t search_pos = json.find("\"breakpoints\"");
  if (search_pos == std::string_view::npos)
  {
    if (base)
      addresses.push_back(*base);
    return addresses;
  }

  const std::string_view breakpoints_json = json.substr(search_pos);
  size_t line_pos = 0;
  while ((line_pos = breakpoints_json.find("\"line\":", line_pos)) != std::string_view::npos)
  {
    line_pos += 7;
    size_t index = line_pos;
    while (index < breakpoints_json.size() &&
           (breakpoints_json[index] == ' ' || breakpoints_json[index] == '\t'))
      ++index;

    u32 line_value = 0;
    bool any = false;
    while (index < breakpoints_json.size() && breakpoints_json[index] >= '0' &&
           breakpoints_json[index] <= '9')
    {
      line_value = line_value * 10 + (breakpoints_json[index] - '0');
      ++index;
      any = true;
    }

    // DESNOTE(jbarber, 2026-07-02): A line number is only meaningful as an
    // address relative to the source's base address; without a base we cannot
    // resolve it, so skip rather than register a bogus breakpoint at the raw
    // line number.
    if (any && base)
      addresses.push_back(*base + line_value * 4);

    line_pos = index;
  }

  if (addresses.empty() && base)
    addresses.push_back(*base);

  return addresses;
}

std::optional<u32> ParseMemoryReference(std::string_view json)
{
  if (const std::optional<std::string_view> memory_reference =
          Json::ExtractStringField(json, "memoryReference"))
  {
    if (const std::optional<u32> address = Json::ParseHexAddress(*memory_reference))
      return address;
  }

  if (const std::optional<std::string_view> source_name =
          Json::ExtractStringField(json, "source"))
  {
    if (const std::optional<u32> address = Json::ParseHexAddress(*source_name))
      return address;
  }

  return std::nullopt;
}

class Session
{
public:
  Session(DapTransport& transport, Core::System& system)
      : m_transport(transport), m_controller(system), m_system(system)
  {
    m_state_hook = Core::AddOnStateChangedCallback([this](const Core::State state) {
      if (state == Core::State::Running)
        QueueEvent("continued", R"("body":{"threadId":1})");
    });
  }

  void Run()
  {
    if (!RunHandshake())
      return;

    m_was_stepping = m_system.GetCPU().IsStepping();
    SendStoppedEvent("entry");

    while (m_running)
    {
      PollBreakpointStop();

      if (!WaitForReadable(m_transport.GetSocket(), 50))
        continue;

      const std::optional<std::string> message = m_transport.ReadMessage();
      if (!message)
        break;

      if (message->empty())
        continue;

      HandleMessage(*message);
    }

    FlushEvents();
  }

private:
  void QueueEvent(std::string_view event, std::string_view body_json)
  {
    std::lock_guard lock(m_event_mutex);
    m_pending_events.push_back(fmt::format(R"({{"seq":{},"type":"event","event":"{}",{}}})",
                                           m_next_seq++, event, body_json));
  }

  void FlushEvents()
  {
    std::vector<std::string> events;
    {
      std::lock_guard lock(m_event_mutex);
      events.swap(m_pending_events);
    }

    for (const std::string& event : events)
      m_transport.WriteMessage(event);
  }

  void SendStoppedEvent(std::string_view reason)
  {
    QueueEvent("stopped", fmt::format(R"("body":{{"reason":"{}","threadId":1}})", reason));
    FlushEvents();
  }

  void PollBreakpointStop()
  {
    const bool stepping = m_system.GetCPU().IsStepping();
    if (!m_was_stepping && stepping)
      SendStoppedEvent("breakpoint");
    m_was_stepping = stepping;
    FlushEvents();
  }

  bool WriteResponse(int request_seq, std::string_view command, std::string_view body_json)
  {
    return m_transport.WriteMessage(fmt::format(
        R"({{"seq":{},"type":"response","request_seq":{},"command":"{}","success":true,{}}})",
        m_next_seq++, request_seq, command, body_json));
  }

  bool RunHandshake()
  {
    bool initialize_done = false;

    while (const std::optional<std::string> message = m_transport.ReadMessage())
    {
      if (message->empty())
        continue;

      const std::string_view json = *message;
      const std::optional<std::string_view> command = Json::ExtractStringField(json, "command");
      const std::optional<int> request_seq = Json::ExtractIntField(json, "seq");

      if (!command || !request_seq)
        continue;

      if (*command == "initialize")
      {
        if (!WriteResponse(*request_seq, "initialize",
                           fmt::format(
                               R"("body":{{"capabilities":{{"supportsConfigurationDoneRequest":true,"supportsDisassembleRequest":true,"supportsReadMemoryRequest":true}},"serverInfo":{{"name":"Dolphin DAP","version":"{}"}}}})",
                               Common::GetScmRevGitStr())))
          return false;

        initialize_done = true;
      }
      else if (*command == "configurationDone")
      {
        if (!initialize_done)
          return false;

        if (!WriteResponse(*request_seq, "configurationDone", R"("body":{})"))
          return false;

        return true;
      }
    }

    return false;
  }

  void HandleMessage(const std::string& message)
  {
    const std::string_view json = message;
    const std::optional<std::string_view> command = Json::ExtractStringField(json, "command");
    const std::optional<int> request_seq = Json::ExtractIntField(json, "seq");

    if (!command || !request_seq)
      return;

    if (*command == "disconnect")
    {
      m_running = false;
      return;
    }

    if (*command == "continue")
    {
      m_controller.Continue();
      m_was_stepping = false;
      WriteResponse(*request_seq, "continue", R"("body":{"allThreadsContinued":true})");
      return;
    }

    if (*command == "pause")
    {
      m_controller.Pause();
      WriteResponse(*request_seq, "pause", R"("body":{})");
      SendStoppedEvent("pause");
      return;
    }

    if (*command == "next" || *command == "stepIn")
    {
      m_controller.StepInto();
      WriteResponse(*request_seq, *command, R"("body":{})");
      SendStoppedEvent("step");
      return;
    }

    if (*command == "setBreakpoints")
    {
      m_controller.SetCodeBreakpoints(ParseBreakpointAddresses(json));
      WriteResponse(*request_seq, "setBreakpoints", R"("body":{"breakpoints":[]})");
      return;
    }

    if (*command == "scopes")
    {
      WriteResponse(*request_seq, "scopes",
                      R"("body":{"scopes":[{"name":"Registers","variablesReference":1000,"expensive":false},{"name":"PC","variablesReference":1001,"expensive":false}]})");
      return;
    }

    if (*command == "variables")
    {
      const std::optional<int> variables_reference = Json::ExtractIntField(json, "variablesReference");
      WriteResponse(*request_seq, "variables",
                      MakeVariablesResponse(variables_reference.value_or(0)));
      return;
    }

    if (*command == "readMemory")
    {
      const std::optional<u32> address = ParseMemoryReference(json);
      const int offset = Json::ExtractIntField(json, "offset").value_or(0);
      const int count = Json::ExtractIntField(json, "count").value_or(0);
      if (!address || count <= 0)
      {
        WriteResponse(*request_seq, "readMemory", R"("body":{})");
        return;
      }

      const std::vector<u8> bytes =
          m_controller.ReadMemory(*address + static_cast<u32>(offset), static_cast<size_t>(count));
      WriteResponse(*request_seq, "readMemory",
                      fmt::format(R"("body":{{"address":{},"data":"{}"}})", *address + offset,
                                  Json::Base64Encode(bytes)));
      return;
    }

    if (*command == "disassemble")
    {
      const std::optional<u32> address = ParseMemoryReference(json);
      const int instruction_count = Json::ExtractIntField(json, "instructionCount").value_or(1);
      if (!address)
      {
        WriteResponse(*request_seq, "disassemble", R"("body":{})");
        return;
      }

      const std::string disasm = m_controller.Disassemble(*address, instruction_count);
      std::string instructions_json;
      u32 addr = *address;
      size_t start = 0;
      while (start <= disasm.size())
      {
        const size_t end = disasm.find('\n', start);
        const size_t line_end = end == std::string::npos ? disasm.size() : end;
        const std::string line = disasm.substr(start, line_end - start);
        if (!line.empty())
        {
          if (!instructions_json.empty())
            instructions_json += ',';
          instructions_json += fmt::format(R"({{"address":{},"instruction":"{}"}})", addr,
                                           Json::EscapeString(line));
          addr += 4;
        }
        if (end == std::string::npos)
          break;
        start = end + 1;
      }

      WriteResponse(*request_seq, "disassemble",
                      fmt::format(R"("body":{{"instructions":[{}]}})", instructions_json));
      return;
    }

    if (*command == "attach")
    {
      WriteResponse(*request_seq, "attach", R"("body":{})");
      SendStoppedEvent("attach");
      return;
    }

    WARN_LOG_FMT(CONSOLE, "DAP: unhandled command {}", *command);
    m_transport.WriteMessage(fmt::format(
        R"({{"seq":{},"type":"response","request_seq":{},"command":"{}","success":false,"message":"unsupported"}})",
        m_next_seq++, *request_seq, *command));
  }

  std::string MakeVariablesResponse(int variables_reference)
  {
    const RegisterSnapshot registers = m_controller.GetRegisters();
    std::string variables_json;

    auto append_variable = [&](std::string_view name, u32 value) {
      if (!variables_json.empty())
        variables_json += ',';
      variables_json +=
          fmt::format(R"({{"name":"{}","value":"0x{:08x}","type":"uint32","variablesReference":0}})",
                      name, value);
    };

    if (variables_reference == kRegistersScope)
    {
      for (std::size_t i = 0; i < registers.gpr.size(); ++i)
        append_variable(fmt::format("r{}", i), registers.gpr[i]);
    }
    else if (variables_reference == kPcScope)
    {
      append_variable("pc", registers.pc);
      append_variable("lr", registers.lr);
      append_variable("ctr", registers.ctr);
      append_variable("msr", registers.msr);
      append_variable("cr", registers.cr);
      append_variable("xer", registers.xer);
    }

    return fmt::format(R"("body":{{"variables":[{}]}})", variables_json);
  }

  DapTransport& m_transport;
  DapDebugController m_controller;
  Core::System& m_system;
  Common::EventHook m_state_hook;
  std::atomic<bool> m_running{true};
  int m_next_seq = 1;
  bool m_was_stepping = false;

  std::mutex m_event_mutex;
  std::vector<std::string> m_pending_events;
};
}  // namespace

void RunSession(DapTransport& transport, Core::System& system)
{
  Session{transport, system}.Run();
}
}  // namespace DAP

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
#include <picojson.h>

#include "Common/HookableEvent.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapDebugController.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/Debugger/DAP/DapProtocol.h"
#include "Core/Debugger/DAP/DapTransport.h"
#include "Core/HW/CPU.h"
#include "Core/System.h"

namespace DAP
{
namespace
{
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

u32 ApplyOffset(u32 address, s64 offset)
{
  return static_cast<u32>(static_cast<s64>(address) + offset);
}

picojson::object MakeVariable(std::string_view name, u32 value)
{
  picojson::object variable;
  variable.emplace("name", std::string(name));
  variable.emplace("value", fmt::format("0x{:08x}", value));
  variable.emplace("type", std::string("uint32"));
  variable.emplace("variablesReference", 0.0);
  return variable;
}

class Session
{
public:
  Session(DapTransport& transport, Core::System& system)
      : m_transport(transport), m_controller(system), m_system(system)
  {
    m_state_hook = Core::AddOnStateChangedCallback([this](const Core::State state) {
      if (state == Core::State::Running)
      {
        picojson::object body;
        body.emplace("threadId", 1.0);
        body.emplace("allThreadsContinued", true);
        QueueEvent("continued", std::move(body));
      }
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
  void QueueEvent(std::string_view event, picojson::object body)
  {
    std::lock_guard lock(m_event_mutex);
    m_pending_events.push_back(
        Protocol::Serialize(Protocol::MakeEvent(m_next_seq++, event, std::move(body))));
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
    picojson::object body;
    body.emplace("reason", std::string(reason));
    body.emplace("threadId", 1.0);
    QueueEvent("stopped", std::move(body));
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

  // DESNOTE(jbarber, 2026-07-02): PollBreakpointStop reports a "breakpoint" stop
  // whenever it observes a not-stepping -> stepping transition it didn't already
  // account for. After we service an explicit pause/step/continue we must reseat
  // that baseline to the current CPU state, otherwise the very next poll re-reports
  // the stop we just emitted (e.g. a duplicate "stopped" right after "pause").
  void SyncSteppingBaseline() { m_was_stepping = m_system.GetCPU().IsStepping(); }

  bool Respond(int request_seq, std::string_view command, picojson::object body)
  {
    return m_transport.WriteMessage(Protocol::Serialize(
        Protocol::MakeResponse(m_next_seq++, request_seq, command, true, std::move(body))));
  }

  bool RespondError(int request_seq, std::string_view command, std::string_view message)
  {
    return m_transport.WriteMessage(Protocol::Serialize(
        Protocol::MakeErrorResponse(m_next_seq++, request_seq, command, message)));
  }

  bool RunHandshake()
  {
    bool initialize_done = false;

    while (const std::optional<std::string> message = m_transport.ReadMessage())
    {
      if (message->empty())
        continue;

      const std::optional<picojson::object> parsed = Json::ParseObject(*message);
      if (!parsed)
        continue;

      const std::optional<Protocol::Request> request = Protocol::ParseRequest(*parsed);
      if (!request)
        continue;

      if (request->command == "initialize")
      {
        if (!Respond(request->seq, "initialize", MakeCapabilities()))
          return false;

        initialize_done = true;
      }
      else if (request->command == "configurationDone")
      {
        if (!initialize_done)
          return false;

        return Respond(request->seq, "configurationDone", picojson::object{});
      }
    }

    return false;
  }

  picojson::object MakeCapabilities()
  {
    picojson::object capabilities;
    capabilities.emplace("supportsConfigurationDoneRequest", true);
    capabilities.emplace("supportsDisassembleRequest", true);
    capabilities.emplace("supportsReadMemoryRequest", true);
    capabilities.emplace("supportsWriteMemoryRequest", true);
    capabilities.emplace("supportsSetVariable", true);
    capabilities.emplace("supportsStackTraceRequest", true);

    picojson::object server_info;
    server_info.emplace("name", std::string("Dolphin DAP"));
    server_info.emplace("version", std::string(Common::GetScmRevGitStr()));

    picojson::object body;
    body.emplace("capabilities", std::move(capabilities));
    body.emplace("serverInfo", std::move(server_info));
    return body;
  }

  void HandleMessage(const std::string& message)
  {
    const std::optional<picojson::object> parsed = Json::ParseObject(message);
    if (!parsed)
    {
      WARN_LOG_FMT(CONSOLE, "DAP: ignoring malformed message");
      return;
    }

    const std::optional<Protocol::Request> request = Protocol::ParseRequest(*parsed);
    if (!request)
      return;

    const std::string& command = request->command;

    if (command == "disconnect")
    {
      m_running = false;
      Respond(request->seq, command, picojson::object{});
      return;
    }

    if (command == "continue")
    {
      m_controller.Continue();
      SyncSteppingBaseline();
      picojson::object body;
      body.emplace("allThreadsContinued", true);
      Respond(request->seq, command, std::move(body));
      return;
    }

    if (command == "pause")
    {
      m_controller.Pause();
      Respond(request->seq, command, picojson::object{});
      SendStoppedEvent("pause");
      SyncSteppingBaseline();
      return;
    }

    if (command == "next")
    {
      const StepOverResult result = m_controller.StepOver();
      Respond(request->seq, command, picojson::object{});
      if (result == StepOverResult::Stepped)
      {
        SendStoppedEvent("step");
        SyncSteppingBaseline();
      }
      else
      {
        SyncSteppingBaseline();
      }
      return;
    }

    if (command == "stepIn")
    {
      m_controller.StepInto();
      Respond(request->seq, command, picojson::object{});
      SendStoppedEvent("step");
      SyncSteppingBaseline();
      return;
    }

    if (command == "stepOut")
    {
      m_controller.StepOut();
      Respond(request->seq, command, picojson::object{});
      SendStoppedEvent("step");
      SyncSteppingBaseline();
      return;
    }

    if (command == "setBreakpoints")
    {
      HandleSetBreakpoints(*request);
      return;
    }

    if (command == "scopes")
    {
      Respond(request->seq, command, MakeScopes());
      return;
    }

    if (command == "variables")
    {
      const std::optional<int> variables_reference =
          ReadNumericFromJson<int>(request->arguments, "variablesReference");
      Respond(request->seq, command, MakeVariables(variables_reference.value_or(0)));
      return;
    }

    if (command == "setVariable")
    {
      HandleSetVariable(*request);
      return;
    }

    if (command == "threads")
    {
      Respond(request->seq, command, MakeThreads());
      return;
    }

    if (command == "stackTrace")
    {
      HandleStackTrace(*request);
      return;
    }

    if (command == "readMemory")
    {
      HandleReadMemory(*request);
      return;
    }

    if (command == "writeMemory")
    {
      HandleWriteMemory(*request);
      return;
    }

    if (command == "disassemble")
    {
      HandleDisassemble(*request);
      return;
    }

    if (command == "attach")
    {
      Respond(request->seq, command, picojson::object{});
      SendStoppedEvent("attach");
      SyncSteppingBaseline();
      return;
    }

    WARN_LOG_FMT(CONSOLE, "DAP: unhandled command {}", command);
    RespondError(request->seq, command, "unsupported");
  }

  void HandleSetBreakpoints(const Protocol::Request& request)
  {
    const Protocol::SetBreakpointsArguments arguments =
        Protocol::ParseSetBreakpoints(request.arguments);

    std::vector<u32> addresses;
    picojson::array breakpoints;
    for (const Protocol::RequestedBreakpoint& breakpoint : arguments.breakpoints)
    {
      picojson::object entry;
      entry.emplace("verified", breakpoint.address.has_value());
      if (breakpoint.address)
      {
        entry.emplace("instructionReference", Json::FormatAddress(*breakpoint.address));
        addresses.push_back(*breakpoint.address);
      }
      breakpoints.emplace_back(std::move(entry));
    }

    // DESNOTE(jbarber, 2026-07-02): DAP setBreakpoints is authoritative for the
    // given source, so a bare source address with no breakpoint entries clears
    // it. We still honor a base-only request (no entries) by setting that one.
    if (arguments.breakpoints.empty() && arguments.base)
      addresses.push_back(*arguments.base);

    m_controller.SetCodeBreakpoints(addresses);

    picojson::object body;
    body.emplace("breakpoints", std::move(breakpoints));
    Respond(request.seq, "setBreakpoints", std::move(body));
  }

  void HandleSetVariable(const Protocol::Request& request)
  {
    const std::optional<Protocol::SetVariableArguments> arguments =
        Protocol::ParseSetVariable(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "setVariable", "invalid setVariable arguments");
      return;
    }

    const std::optional<u32> value =
        m_controller.SetRegister(arguments->variables_reference, arguments->name, arguments->value);
    if (!value)
    {
      RespondError(request.seq, "setVariable", "invalid setVariable arguments");
      return;
    }

    picojson::object body;
    body.emplace("value", fmt::format("0x{:08x}", *value));
    Respond(request.seq, "setVariable", std::move(body));
  }

  picojson::object MakeThreads()
  {
    picojson::array threads;
    for (const ThreadInfo& thread : m_controller.GetThreads())
    {
      picojson::object entry;
      entry.emplace("id", static_cast<double>(thread.id));
      entry.emplace("name", thread.name);
      threads.emplace_back(std::move(entry));
    }

    picojson::object body;
    body.emplace("threads", std::move(threads));
    return body;
  }

  void HandleStackTrace(const Protocol::Request& request)
  {
    const std::optional<int> thread_id = ReadNumericFromJson<int>(request.arguments, "threadId");
    if (!thread_id || *thread_id != 1)
    {
      RespondError(request.seq, "stackTrace", "invalid stackTrace arguments");
      return;
    }

    const int start_frame = ReadNumericFromJson<int>(request.arguments, "startFrame").value_or(0);
    // DESNOTE(jbarber, 2026-07-03): DAP treats an omitted `levels` as "all
    // frames"; the controller interprets 0 that way.
    const int levels = ReadNumericFromJson<int>(request.arguments, "levels").value_or(0);

    const StackTraceResult trace = m_controller.GetStackTrace(start_frame, levels);

    picojson::array stack_frames;
    for (const StackFrame& frame : trace.frames)
    {
      picojson::object entry;
      entry.emplace("id", static_cast<double>(frame.id));
      entry.emplace("name", frame.name);
      entry.emplace("instructionPointerReference", Json::FormatAddress(frame.address));
      entry.emplace("line", 0.0);
      entry.emplace("column", 0.0);
      stack_frames.emplace_back(std::move(entry));
    }

    picojson::object body;
    body.emplace("stackFrames", std::move(stack_frames));
    body.emplace("totalFrames", static_cast<double>(trace.total_frames));
    Respond(request.seq, "stackTrace", std::move(body));
  }

  void HandleReadMemory(const Protocol::Request& request)
  {
    const std::optional<Protocol::ReadMemoryArguments> arguments =
        Protocol::ParseReadMemory(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "readMemory", "invalid readMemory arguments");
      return;
    }

    const u32 address = ApplyOffset(arguments->address, arguments->offset);
    const std::vector<u8> bytes = m_controller.ReadMemory(address, arguments->count);
    const u32 unreadable = arguments->count - static_cast<u32>(bytes.size());

    picojson::object body;
    body.emplace("address", Json::FormatAddress(address));
    body.emplace("data", Json::Base64Encode(bytes));
    if (unreadable > 0)
      body.emplace("unreadableBytes", static_cast<double>(unreadable));
    Respond(request.seq, "readMemory", std::move(body));
  }

  void HandleWriteMemory(const Protocol::Request& request)
  {
    const std::optional<Protocol::WriteMemoryArguments> arguments =
        Protocol::ParseWriteMemory(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "writeMemory", "invalid writeMemory arguments");
      return;
    }

    const u32 address = ApplyOffset(arguments->address, arguments->offset);
    const std::size_t written = m_controller.WriteMemory(address, arguments->data);

    if (written < arguments->data.size() && !arguments->allow_partial)
    {
      RespondError(request.seq, "writeMemory", "memory range not fully writable");
      return;
    }

    picojson::object body;
    body.emplace("bytesWritten", static_cast<double>(written));
    body.emplace("offset", 0.0);
    Respond(request.seq, "writeMemory", std::move(body));
  }

  void HandleDisassemble(const Protocol::Request& request)
  {
    const std::optional<Protocol::DisassembleArguments> arguments =
        Protocol::ParseDisassemble(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "disassemble", "invalid disassemble arguments");
      return;
    }

    u32 address = ApplyOffset(arguments->address, arguments->offset) +
                  static_cast<u32>(arguments->instruction_offset * 4);
    const std::string disasm =
        m_controller.Disassemble(address, static_cast<int>(arguments->instruction_count));

    picojson::array instructions;
    size_t start = 0;
    while (start <= disasm.size())
    {
      const size_t end = disasm.find('\n', start);
      const size_t line_end = end == std::string::npos ? disasm.size() : end;
      const std::string line = disasm.substr(start, line_end - start);
      if (!line.empty())
      {
        picojson::object instruction;
        instruction.emplace("address", Json::FormatAddress(address));
        instruction.emplace("instruction", line);
        instructions.emplace_back(std::move(instruction));
        address += 4;
      }
      if (end == std::string::npos)
        break;
      start = end + 1;
    }

    picojson::object body;
    body.emplace("instructions", std::move(instructions));
    Respond(request.seq, "disassemble", std::move(body));
  }

  static picojson::object MakeScope(std::string_view name, int variables_reference)
  {
    picojson::object scope;
    scope.emplace("name", std::string(name));
    scope.emplace("variablesReference", static_cast<double>(variables_reference));
    scope.emplace("expensive", false);
    return scope;
  }

  picojson::object MakeScopes()
  {
    picojson::array scopes;
    scopes.emplace_back(MakeScope("Registers", REGISTERS_SCOPE));
    scopes.emplace_back(MakeScope("PC", PC_SCOPE));

    picojson::object body;
    body.emplace("scopes", std::move(scopes));
    return body;
  }

  picojson::object MakeVariables(int variables_reference)
  {
    const RegisterSnapshot registers = m_controller.GetRegisters();
    picojson::array variables;

    if (variables_reference == REGISTERS_SCOPE)
    {
      for (std::size_t i = 0; i < registers.gpr.size(); ++i)
        variables.emplace_back(MakeVariable(fmt::format("r{}", i), registers.gpr[i]));
    }
    else if (variables_reference == PC_SCOPE)
    {
      variables.emplace_back(MakeVariable("pc", registers.pc));
      variables.emplace_back(MakeVariable("lr", registers.lr));
      variables.emplace_back(MakeVariable("ctr", registers.ctr));
      variables.emplace_back(MakeVariable("msr", registers.msr));
      variables.emplace_back(MakeVariable("cr", registers.cr));
      variables.emplace_back(MakeVariable("xer", registers.xer));
    }

    picojson::object body;
    body.emplace("variables", std::move(variables));
    return body;
  }

  DapTransport& m_transport;
  DapDebugController m_controller;
  Core::System& m_system;
  Common::EventHook m_state_hook;
  std::atomic<bool> m_running{true};
  // Sequence numbers are handed out from both the session loop (responses) and
  // the core state-changed callback (the "continued" event), which may run on
  // the CPU thread; keep allocation race-free.
  std::atomic<int> m_next_seq{1};
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

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapSession.h"

#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapDebugController.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/Debugger/DAP/DapProtocol.h"
#include "Core/Debugger/DAP/DapRealtimeWatch.h"
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

// DESNOTE(jbarber, 2026-07-21): DAP allows signed `offset` on readMemory,
// writeMemory, and disassemble (as an instruction byte offset). The previous
// form `static_cast<u32>(s64(address) + offset)` silently wrapped a
// negative or overflowed effective address to a nonsense high value --
// reads/writes would then land at the wrong cell, and a breakpoint-style
// request could corrupt an unrelated region. Return nullopt when the
// effective address falls outside [0, u32 max] so callers can surface an
// error response instead of touching the wrong memory.
std::optional<u32> ApplyOffset(u32 address, s64 offset)
{
  const s64 effective = static_cast<s64>(address) + offset;
  if (effective < 0 || effective > static_cast<s64>(std::numeric_limits<u32>::max()))
    return std::nullopt;
  return static_cast<u32>(effective);
}

const picojson::object* GetObject(const picojson::object& obj, const std::string& key)
{
  const auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<picojson::object>())
    return nullptr;
  return &it->second.get<picojson::object>();
}

std::optional<std::string> ReadSourceString(const picojson::object& source,
                                            const std::string& key)
{
  if (const std::optional<std::string> value = ReadStringFromJson(source, key))
  {
    if (!value->empty())
      return value;
  }
  return std::nullopt;
}

SourceBreakpointContext ParseSourceBreakpointContext(const picojson::object& arguments)
{
  SourceBreakpointContext context;
  if (const std::optional<int> source_reference =
          ReadNumericFromJson<int>(arguments, "sourceReference"))
  {
    context.source_reference = *source_reference;
  }

  if (const picojson::object* source = GetObject(arguments, "source"))
  {
    context.source_name = ReadSourceString(*source, "name");
    context.source_path = ReadSourceString(*source, "path");
  }
  return context;
}

std::string MakeBreakpointSourceKey(const SourceBreakpointContext& context)
{
  if (context.source_reference && *context.source_reference > 0)
    return fmt::format("ref:{}", *context.source_reference);

  if (context.source_path)
  {
    if (Json::ParseHexAddress(*context.source_path))
      return fmt::format("disasm:{}", *context.source_path);
    return fmt::format("path:{}", *context.source_path);
  }

  if (context.source_name)
  {
    if (Json::ParseHexAddress(*context.source_name))
      return fmt::format("disasm:{}", *context.source_name);
    return fmt::format("name:{}", *context.source_name);
  }

  return "unknown";
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

class Session : public std::enable_shared_from_this<Session>
{
public:
  // DESNOTE(jbarber, 2026-07-21): The step-out worker captures a
  // shared_ptr<Session>, so this destructor only runs after that worker has
  // exited (the captured shared_ptr is what keeps *this alive past
  // RunSession's stack-frame release). join() at that stage just reaps the
  // OS thread; it will never block. Without this, std::thread's destructor
  // would terminate the process on a still-joinable handle.
  ~Session()
  {
    if (m_step_out_thread.joinable())
      m_step_out_thread.join();
  }

  Session(DapTransport& transport, Core::System& system)
      : m_transport(transport), m_controller(system), m_system(system),
        m_stop_on_entry(Config::Get(Config::MAIN_DAP_STOP_ON_ENTRY))
  {
    // DESNOTE(jbarber, 2026-07-21): The state hook fires on the CPU thread at
    // the moment of a running<->paused transition. For Paused we capture the
    // stop reason synchronously here, while ppc_state.Exceptions still carries
    // EXCEPTION_FAKE_MEMCHECK_HIT (the flag is transient -- the next
    // interpreter step's CheckExceptions clears it). PollBreakpointStop, which
    // runs ~50ms later on the session thread, used to miss the flag and
    // mis-report watchpoint hits as "step"; it now consumes the stashed reason
    // instead. For Running (continue), we queue the `continued` event.
    m_state_hook = Core::AddOnStateChangedCallback([this](const Core::State state) {
      if (state == Core::State::Running)
      {
        picojson::object body;
        body.emplace("threadId", 1.0);
        body.emplace("allThreadsContinued", true);
        QueueEvent("continued", std::move(body));
      }
      else if (state == Core::State::Paused)
      {
        std::lock_guard lock(m_stop_info_mutex);
        m_pending_stop_info = m_controller.GetStopInfo();
      }
    });
  }

  void Run()
  {
    if (!RunHandshake())
      return;

    // DESNOTE(jbarber, 2026-07-21): Construct the realtime-watch sampler here
    // (not in the ctor) so its dispatch lambda can capture a weak_ptr to this
    // Session via weak_from_this() -- which is only valid once *this is owned
    // by a shared_ptr (RunSession constructs the Session via make_shared before
    // calling Run()). The weak_ptr lock keeps the Session alive for the
    // duration of an in-flight Tick() on the CPU thread, so disconnect/
    // shutdown can't race the dispatch and touch a destroyed Session. When the
    // weak_ptr can't be locked (session torn down), the dispatch is a no-op.
    auto self = shared_from_this();
    m_watch_sampler = std::make_unique<RealtimeWatchSampler>(
        m_system,
        [weak = std::weak_ptr<Session>(self)](
            const std::vector<RealtimeWatchChange>& changes) {
          if (auto sp = weak.lock())
          {
            for (const RealtimeWatchChange& change : changes)
            {
              picojson::object body;
              body.emplace("watchId", static_cast<double>(change.watch_id));
              body.emplace("address", Json::FormatAddress(change.address));
              body.emplace("count", static_cast<double>(change.count));
              body.emplace("data", Json::Base64Encode(change.bytes));
              sp->QueueEvent("dolphin_memoryChanged", std::move(body));
            }
          }
        });

    m_was_stepping = m_system.GetCPU().IsStepping();

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

    // Tear down the sampler before the rest of the Session so its
    // vi_end_field_event hook is unregistered (no further Tick() will fire)
    // before the transport/event-queue members are destroyed.
    m_watch_sampler.reset();
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
    // DESNOTE(jbarber, 2026-07-21): Drop any stashed stop reason so a later
    // spontaneous stop (e.g. data watchpoint hit after `continue`) can't
    // accidentally reuse a stale CodeBreakpoint classification from before
    // this explicit pause/step/goto/restart. The state hook repopulates
    // m_pending_stop_info on the next Paused transition with fresh info
    // captured while exceptions are still set, so SendClassifiedStoppedEvent
    // will always see an up-to-date stash. Without this clear, a pause
    // while the PC sits on a code breakpoint would leave the stash holding
    // "CodeBreakpoint" and a later SendClassifiedStoppedEvent would
    // mis-report the watchpoint stop as a breakpoint hit.
    {
      std::lock_guard lock(m_stop_info_mutex);
      m_pending_stop_info.reset();
    }
    picojson::object body;
    body.emplace("reason", std::string(reason));
    body.emplace("threadId", 1.0);
    QueueEvent("stopped", std::move(body));
    FlushEvents();
  }

  // DESNOTE(jbarber, 2026-07-21): Fires the entry/attach stop exactly once,
  // gated on BOTH `launch`/`attach` and `configurationDone` having arrived.
  // This lets a `stopOnEntry` override on a `launch` that comes in after
  // `configurationDone` actually take effect -- previously configurationDone
  // would fire the entry stop immediately with the config-default policy and
  // silently drop the later override. Honors m_stop_on_entry (overridden or
  // config-seeded) at decision time, so the override wins.
  void MaybeFireEntryStop(std::string_view command)
  {
    if (!m_launch_seen || !m_config_done || m_entry_stop_sent)
      return;
    (void)command;
    m_entry_stop_sent = true;
    if (m_stop_on_entry)
    {
      // DESNOTE(jbarber, 2026-07-21): Force the core into Paused before
      // emitting the stop event. CPUSetInitialExecutionState already pauses
      // when DAP is active, but a previous launch/attach with stopOnEntry
      // false (or the client continuing the core out-of-band) leaves State
      // at Running -- telling the client execution stopped while the game
      // kept running would lie. PauseAndLock is re-entrant so this is safe
      // even if the state hook fires on the resulting Paused transition.
      m_controller.Pause();
      SendStoppedEvent(m_launch_kind == "attach" ? "attach" : "entry");
    }
    else
    {
      // Core starts paused while a debugger is attached (see
      // CPUSetInitialExecutionState in Core.cpp); resume it so the game runs
      // immediately. The state hook queues a `continued` event.
      m_controller.Continue();
    }
  }

  // Emits a `stopped` event whose reason is classified from the current PC
  // (code breakpoint, data watchpoint, or step) and carries `hitBreakpointIds`
  // when a breakpoint at the PC caused the stop.
  //
  // The reason is taken from `m_pending_stop_info` when present: the state hook
  // stashes it synchronously on the CPU thread at break time, while
  // EXCEPTION_FAKE_MEMCHECK_HIT is still set (it's cleared by the next
  // CheckExceptions). If the stash is empty (e.g. an explicit pause that
  // already used SendStoppedEvent("pause"), or the hook ran before exceptions
  // were raised), `info` defaults to `Step` with no hit_breakpoint and we
  // send that as the safest non-committal answer. The previous form fell
  // back to a fresh `m_controller.GetStopInfo()` here, but that call runs
  // on the session thread without a CPU freeze and can see cleared/
  // mid-update exception state -- a race Bugbot flagged as "stale stop info
  // after pause". The stash is the authoritative source; everything else
  // is best-effort and we'd rather under-report than mis-report.
  //
  // DESNOTE(jbarber, 2026-07-21): Bugbot re-flagged this as
  // "Watchpoint stops report as step": `CPUManager::Break` (which fires on
  // code-breakpoint and watchpoint hits in the interpreter) doesn't call
  // `NotifyStateChanged`, so the Paused hook never runs for those stops
  // and `m_pending_stop_info` stays empty -- the client got "step" for a
  // real breakpoint hit. Only `Continue()` notifies (with Running), so
  // the Running hook fires but the matching Paused hook doesn't. Rather
  // than touch `CPUManager::Break` (a hotpath shared with frame-step,
  // GDB stub, etc., and wiring NotifyStateChanged there risks recursive
  // callbacks), fall back to a fresh `GetStopInfo()` here when the stash
  // is empty. `GetStopInfo` takes a `CPUThreadGuard` itself, so the CPU
  // is frozen for the read and the exception flag can't be cleared out
  // from under us -- the earlier "stale stop info" concern was about
  // reading without that freeze; with the guard the read is safe.
  void SendClassifiedStoppedEvent()
  {
    StopInfo info;
    {
      std::lock_guard lock(m_stop_info_mutex);
      if (m_pending_stop_info)
      {
        info = *m_pending_stop_info;
        m_pending_stop_info.reset();
      }
    }
    // No stash means the state hook didn't fire for this stop (typical for
    // interpreter breakpoint/watchpoint hits via CPU::Break, which doesn't
    // notify state changes). Re-classify from live CPU state -- GetStopInfo
    // takes a CPUThreadGuard so the read is atomic with respect to the
    // stepping CPU. Keep this as a fallback so the hook-driven fast path
    // still wins when it does fire.
    if (info.reason == StopReason::Step && !info.hit_breakpoint_address)
      info = m_controller.GetStopInfo();

    picojson::object body;
    body.emplace("threadId", 1.0);
    switch (info.reason)
    {
    case StopReason::CodeBreakpoint:
      body.emplace("reason", std::string("breakpoint"));
      break;
    case StopReason::DataBreakpoint:
      body.emplace("reason", std::string("data breakpoint"));
      break;
    case StopReason::Step:
      body.emplace("reason", std::string("step"));
      break;
    }

    if (info.hit_breakpoint_address)
    {
      // DESNOTE(jbarber, 2026-07-21): DAP `hitBreakpointIds` must echo the
      // stable id the client received in its setBreakpoints /
      // setInstructionBreakpoints response -- NOT the raw PC address. The
      // previous form shoved the address in here, which broke client-side
      // correlation between configured breakpoints and stop events. Look
      // up the id via m_bp_id_by_address; if no mapping exists (e.g. a
      // temporary step-over breakpoint set via SetTemporary, which never
      // got a DAP response), omit hitBreakpointIds entirely rather than
      // fabricate one.
      std::optional<int> hit_id = LookupBreakpointId(*info.hit_breakpoint_address);
      if (hit_id)
      {
        picojson::array hit_ids;
        hit_ids.emplace_back(static_cast<double>(*hit_id));
        body.emplace("hitBreakpointIds", std::move(hit_ids));
      }
    }

    QueueEvent("stopped", std::move(body));
    FlushEvents();
  }

  // DESNOTE(jbarber, 2026-07-21): Assign a stable DAP id for a breakpoint
  // site installed at `address` and remember the (address -> id) mapping so
  // SendClassifiedStoppedEvent can translate a hit PC back to the id the
  // client received in its setBreakpoints / setInstructionBreakpoints
  // response. Re-installing the same address (e.g. when the client
  // re-sends an authoritative set with the same site) reuses the existing
  // id rather than minting a new one, so client-side correlation doesn't
  // break across refreshes. Ids start at 1 because DAP clients often
  // treat 0 as "no id".
  int AssignBreakpointId(u32 address)
  {
    std::lock_guard lock(m_bp_id_mutex);
    auto it = m_bp_id_by_address.find(address);
    if (it != m_bp_id_by_address.end())
      return it->second;
    const int id = m_next_bp_id++;
    m_bp_id_by_address.emplace(address, id);
    return id;
  }

  std::optional<int> LookupBreakpointId(u32 address)
  {
    std::lock_guard lock(m_bp_id_mutex);
    auto it = m_bp_id_by_address.find(address);
    if (it == m_bp_id_by_address.end())
      return std::nullopt;
    return it->second;
  }

  // DESNOTE(jbarber, 2026-07-21): Removed the per-set `ClearBreakpointIds`
  // call. The previous form wiped the whole (address -> id) map on each
  // authoritative setBreakpoints / setInstructionBreakpoints call, which
  // also discarded ids for breakpoints of the OTHER kind (source BPs from
  // other files coexisting with instruction BPs in Dolphin's single
  // code-breakpoint list). A subsequent stop for one of those still-
  // installed sites then had no id to echo back in hitBreakpointIds,
  // breaking client correlation. Stale entries (addresses no longer
  // installed as breakpoints) are harmless: GetStopInfo only sets
  // hit_breakpoint_address when an actual breakpoint exists at the PC,
  // so a stale map entry can't be looked up against a fabricated hit.
  // AssignBreakpointId reuses an existing id when the client re-installs
  // an address, keeping client-side id correlation stable across
  // refreshes.

  void PollBreakpointStop()
  {
    // Async step-out completion: the worker signals via m_step_out_done once
    // DapDebugController::StepOut has returned. Join and emit the appropriate
    // stopped event here, on the session thread, so the client still sees it
    // in command order. The step-out may have stopped early because the
    // interpreter loop hit a code breakpoint (StepOut bails when
    // CheckBreakPoints fires) -- classify via SendClassifiedStoppedEvent so
    // a real breakpoint hit isn't mis-reported as a plain 'step'. This
    // replaces the previous synchronous inline call that blocked
    // HandleMessage for up to the 5s step-out timeout.
    if (m_step_out_thread.joinable() && m_step_out_done.load())
    {
      m_step_out_thread.join();
      // DESNOTE(jbarber, 2026-07-21): If a continue/pause was deferred
      // during the step-out (because the worker held the stepping lock
      // for the whole interpreter loop), apply it now that the lock is
      // free. Continue pre-empts the stopped event; pause synthesizes a
      // "pause" stop instead of the step-out's classified one. Without
      // this deferral, m_controller.Continue/Pause would block the
      // session thread on the stepping lock until the worker's 5s
      // timeout elapsed.
      //
      // DESNOTE(jbarber, 2026-07-21): If both flags are set (client asked to
      // continue THEN pause during the step-out window), pause wins: the
      // user's most recent intent was to halt. The earlier form ran Continue
      // first and returned, silently dropping the deferred pause so the core
      // kept running despite the client requesting a halt. Drain both flags
      // and apply pause when both were requested.
      const bool want_continue = m_pending_continue.exchange(false);
      const bool want_pause = m_pending_pause.exchange(false);
      if (want_pause)
      {
        // DESNOTE(jbarber, 2026-07-21): A user-initiated pause that
        // happened during async step-out should be reported to the client
        // with reason "pause", not whatever SendClassifiedStoppedEvent
        // would synthesize from the step-out's stashed stop info (which
        // could be NoteBreakpoint / Step). The user explicitly asked the
        // core to halt. If a continue was also pending (continue THEN
        // pause), pause pre-empts since it is the more recent request.
        m_controller.Pause();
        SendStoppedEvent("pause");
        SyncSteppingBaseline();
        return;
      }
      if (want_continue)
      {
        m_controller.Continue();
        SyncSteppingBaseline();
        return;
      }
      SendClassifiedStoppedEvent();
      SyncSteppingBaseline();
      return;
    }

    const bool stepping = m_system.GetCPU().IsStepping();
    if (!m_was_stepping && stepping)
      SendClassifiedStoppedEvent();
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

        QueueEvent("initialized", picojson::object{});
        FlushEvents();
        return true;
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
    capabilities.emplace("supportsDataBreakpoints", true);
    capabilities.emplace("supportsEvaluateForHovers", true);
    capabilities.emplace("supportsInstructionBreakpoints", true);
    capabilities.emplace("supportsGotoTargetsRequest", true);
    capabilities.emplace("supportsExceptionInfoRequest", true);
    capabilities.emplace("supportsLoadedSourcesRequest", true);
    capabilities.emplace("supportsRestartRequest", true);
    // Dolphin-specific custom capabilities. Clients can probe these to decide
    // whether to use the realtime memory watch / freeze extensions or fall
    // back to standard DAP readMemory/writeMemory polling.
    capabilities.emplace("supportsDolphinRealtimeWatch", true);
    capabilities.emplace("supportsDolphinFreeze", true);
    capabilities.emplace("supportsDolphinFindFreeMemory", true);
    capabilities.emplace("supportsDolphinInjectCode", true);
    capabilities.emplace("supportsDolphinDetour", true);

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
      // DESNOTE(jbarber, 2026-07-21): An in-flight async step-out holds the
      // CPU stepping_lock via its CPUThreadGuard for the whole interpreter
      // loop (up to the configured 5s timeout). Directly calling
      // m_controller.Continue() here would block the session thread waiting
      // on that lock -- stallong socket reads, realtime-watch flushing, and
      // disconnect. Respond success immediately and remember the continue
      // request; PollBreakpointStop picks it up once the worker has joined.
      if (m_step_out_thread.joinable() && !m_step_out_done.load())
      {
        m_pending_continue.store(true);
        // DESNOTE(jbarber, 2026-07-21): Ack the request with an empty body
        // and NO allThreadsContinued flag. The core is still stepping out
        // (the worker holds the CPUThreadGuard for up to its 5s timeout),
        // so claiming all threads have been continued would lie to the
        // client -- it would transition its UI to "running" while the
        // emulator is still single-stepping toward the next return. The
        // earlier form mirrored the synchronous path's
        // allThreadsContinued: true here, which Bugbot flagged as
        // "Continue deferred during step-out". Continue() actually runs
        // in PollBreakpointStop once the worker joins; that fires the
        // state hook which queues the `continued` event with
        // allThreadsContinued: true at that point. The client treats the
        // immediate response as "request acknowledged, resume pending"
        // and waits for the `continued` event before flipping to running.
        Respond(request->seq, command, picojson::object{});
        return;
      }
      m_controller.Continue();
      SyncSteppingBaseline();
      picojson::object body;
      body.emplace("allThreadsContinued", true);
      Respond(request->seq, command, std::move(body));
      return;
    }

    if (command == "pause")
    {
      // Same deferred-execution rationale as continue above -- the step-out
      // worker holds the stepping lock, so pause can't take effect until
      // it bails. Queue it for PollBreakpointStop to apply post-join.
      if (m_step_out_thread.joinable() && !m_step_out_done.load())
      {
        m_pending_pause.store(true);
        Respond(request->seq, command, picojson::object{});
        return;
      }
      m_controller.Pause();
      Respond(request->seq, command, picojson::object{});
      SendStoppedEvent("pause");
      SyncSteppingBaseline();
      return;
    }

    if (command == "next")
    {
      // DESNOTE(jbarber, 2026-07-21): DAP stepping requires the core to be
      // paused. If a client sends `next` while running (a client error), the
      // controller's StepOver early-returns without advancing -- don't emit a
      // spurious `stopped`/"step". Pause first so the step has a frame to step
      // from, mirroring how VS Code's own client pauses before stepping.
      if (!m_system.GetCPU().IsStepping())
        m_controller.Pause();
      // DESNOTE(jbarber, 2026-07-21): Drop any stashed stop reason from the
      // pre-step Pause above. That Pause fires the state hook which stashes
      // GetStopInfo at the pre-step PC -- and if the pre-step PC has a code
      // breakpoint (the user installed one AT the current instruction), the
      // stash is CodeBreakpoint. After Step advances the PC, the stale
      // CodeBreakpoint stash would misclassify this stop as a breakpoint hit
      // instead of a step. Clearing here leaves SendClassifiedStoppedEvent
      // with an empty stash, so it falls back to a fresh GetStopInfo at
      // the post-step PC and classifies correctly (CodeBreakpoint only if
      // the NEXT instruction has one, Step otherwise).
      {
        std::lock_guard lock(m_stop_info_mutex);
        m_pending_stop_info.reset();
      }
      const StepOverResult result = m_controller.StepOver();
      Respond(request->seq, command, picojson::object{});
      if (result == StepOverResult::Stepped)
      {
        // DESNOTE(jbarber, 2026-07-21): Classify the actual stop via the
        // stashed reason rather than unconditionally reporting "step" --
        // StepOver may have advanced onto a code/data breakpoint that was
        // hiding at the next instruction. SendClassifiedStoppedEvent will
        // emit "breakpoint" with hitBreakpointIds in that case, "step" when
        // the step genuinely advanced.
        SendClassifiedStoppedEvent();
        SyncSteppingBaseline();
      }
      else if (result == StepOverResult::NotStepped)
      {
        // DESNOTE(jbarber, 2026-07-21): The underlying StepInto timed out
        // without the CPU thread acknowledging -- the PC has not advanced.
        // Suppress the stopped event (mirroring the stepIn guard) so the
        // client isn't told execution stopped at an unchanged PC. The
        // response still acks the `next` request so the client knows the
        // command was received.
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
      if (!m_system.GetCPU().IsStepping())
        m_controller.Pause();
      // DESNOTE(jbarber, 2026-07-21): Drop any stashed stop reason from the
      // pre-step Pause above -- a CodeBreakpoint stash at the pre-step PC
      // would misclassify this stop as a breakpoint hit instead of a step
      // once Step advances the PC. Clearing leaves SendClassifiedStoppedEvent
      // with an empty stash so it falls back to a fresh GetStopInfo at the
      // post-step PC. (Same rationale as the `next` handler above.)
      {
        std::lock_guard lock(m_stop_info_mutex);
        m_pending_stop_info.reset();
      }
      // DESNOTE(jbarber, 2026-07-21): StepInto may return false when the
      // CPU thread can't acknowledge the StepOpcode signal within its 2s
      // wait (e.g. the emulator is mid-block or under load). Emitting a
      // `stopped`/`step` event in that case would lie to the client -- the
      // PC hasn't advanced -- so we only send the stop when the step
      // actually completed. The earlier "stop arrives later via
      // PollBreakpointStop" framing was wrong: PollBreakpointStop only
      // emits stops on a not-stepping->stepping transition, and after
      // Pause/SyncSteppingBaseline the core is already stepping=true with
      // no further state change when the late completion fires -- so
      // suppressing the stop here is the only correct response.
      const bool completed = m_controller.StepInto();
      Respond(request->seq, command, picojson::object{});
      if (completed)
      {
        // DESNOTE(jbarber, 2026-07-21): Classify the actual stop reason
        // (CodeBreakpoint / DataBreakpoint / Step) instead of always
        // emitting "step" -- StepInto may have walked the PC onto a real
        // breakpoint the user installed at the next instruction. The state
        // hook has already stashed the reason while the exception flag was
        // still set; SendClassifiedStoppedEvent consumes it.
        SendClassifiedStoppedEvent();
        SyncSteppingBaseline();
      }
      else
      {
        SyncSteppingBaseline();
      }
      return;
    }

    if (command == "stepOut")
    {
      if (!m_system.GetCPU().IsStepping())
        m_controller.Pause();
      // DESNOTE(jbarber, 2026-07-21): Run StepOut on a worker thread so the
      // session loop keeps polling the socket for disconnect / new requests
      // and keeps flushing realtime-watch events while the interpreter
      // single-steps toward the next return. PollBreakpointStop joins the
      // worker when it signals completion and emits the stopped event via
      // SendClassifiedStoppedEvent so a real breakpoint hit on the way out is
      // reported as 'breakpoint', not as a plain 'step'.
      //
      // The pre-step pause fires the state hook and populates
      // m_pending_stop_info with whatever GetStopInfo sees at that instant
      // (often a CodeBreakpoint if we stopped on a code breakpoint the user
      // had planted at the PC). If we left that stash in place, the
      // post-step SendClassifiedStoppedEvent would consume it and lie about
      // the stop reason. Reset it here so only a state-hook fire *during*
      // the worker's interpreter loop can attribute the eventual stop --
      // otherwise SendClassifiedStoppedEvent defaults to 'step' (which is
      // the correct characterization for a step-out that ran to a return).
      //
      // A previous step-out is still in flight only if the client reissued
      // step-out within one 50ms poll window -- in that pathological case we
      // can't safely start a second concurrent interpreter loop, so we
      // acknowledge the request with success and let the prior worker
      // complete. (Previously this branch returned without responding,
      // which left the client's request un-answered.)
      // DESNOTE(jbarber, 2026-07-21): A previous step-out may still be in
      // flight (client reissued stepOut within one 50ms poll window). We
      // can't safely start a second concurrent interpreter loop against
      // the same CPU -- two workers would each take CPUThreadGuard and
      // race on the PC + breakpoint state. The previous form responded
      // success and returned, leaving the client believing a second
      // step-out ran when only the prior worker is active and only one
      // classified stop will arrive. Reject the duplicate explicitly so
      // the client knows the second step-out didn't start -- it can retry
      // once the in-flight worker's stopped event arrives.
      if (m_step_out_thread.joinable())
      {
        if (!m_step_out_done.load())
        {
          RespondError(request->seq, command, "stepOut already in progress");
          return;
        }
        m_step_out_thread.join();
      }
      {
        std::lock_guard lock(m_stop_info_mutex);
        m_pending_stop_info.reset();
      }
      m_step_out_done.store(false);
      m_step_out_thread = std::thread([self = shared_from_this()]() {
        self->m_controller.StepOut();
        self->m_step_out_done.store(true);
      });
      Respond(request->seq, command, picojson::object{});
      // Don't emit stopped here -- PollBreakpointStop will, once the
      // worker signals completion. SyncSteppingBaseline is also deferred to
      // the join site so the polling baseline matches the post-step state.
      return;
    }

    if (command == "setBreakpoints")
    {
      HandleSetBreakpoints(*request);
      return;
    }

    if (command == "setDataBreakpoints")
    {
      HandleSetDataBreakpoints(*request);
      return;
    }

    if (command == "setInstructionBreakpoints")
    {
      HandleSetInstructionBreakpoints(*request);
      return;
    }

    if (command == "gotoTargets")
    {
      HandleGotoTargets(*request);
      return;
    }

    if (command == "goto")
    {
      HandleGoto(*request);
      return;
    }

    if (command == "exceptionInfo")
    {
      HandleExceptionInfo(*request);
      return;
    }

    if (command == "loadedSources")
    {
      HandleLoadedSources(*request);
      return;
    }

    if (command == "source")
    {
      HandleSource(*request);
      return;
    }

    if (command == "breakpointLocations")
    {
      HandleBreakpointLocations(*request);
      return;
    }

    if (command == "terminate")
    {
      HandleTerminate(*request);
      return;
    }

    if (command == "restart")
    {
      HandleRestart(*request);
      return;
    }

    if (command == "evaluate")
    {
      HandleEvaluate(*request);
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

    if (command == "configurationDone")
    {
      Respond(request->seq, command, picojson::object{});
      // DESNOTE(jbarber, 2026-07-21): Don't commit the entry-stop policy yet
      // if `launch`/`attach` hasn't been seen -- a configDone-before-launch
      // ordering would otherwise freeze the client's later `stopOnEntry=false`
      // override. MaybeFireEntryStop gates on both m_launch_seen and
      // m_config_done, so the deferred fire picks up the override.
      m_config_done = true;
      MaybeFireEntryStop(command);
      SyncSteppingBaseline();
      return;
    }

    if (command == "launch" || command == "attach")
    {
      // DESNOTE(jbarber, 2026-07-21): `stopOnEntry` is the standard DAP field
      // for "should we break at entry". When the client omits it, we fall back
      // to the `Dolphin.General.DAPStopOnEntry` config (set at dolphin launch
      // via `-C Dolphin.General.DAPStopOnEntry=false`), which the ctor seeded
      // into m_stop_on_entry. An explicit true/false overrides the config for
      // this session. Default true preserves the historical always-paused
      // behavior when neither the config nor the request says otherwise.
      const std::optional<Protocol::LaunchArguments> launch_args =
          Protocol::ParseLaunch(request->arguments);
      if (launch_args && launch_args->stop_on_entry.has_value())
        m_stop_on_entry = *launch_args->stop_on_entry;

      Respond(request->seq, command, picojson::object{});
      m_debugging_started = true;
      m_launch_seen = true;
      m_launch_kind = command;
      MaybeFireEntryStop(command);
      SyncSteppingBaseline();
      return;
    }

    if (command == "dolphin_realtimeWatch")
    {
      HandleRealtimeWatch(*request);
      return;
    }

    if (command == "dolphin_realtimeWatchCancel")
    {
      HandleRealtimeWatchCancel(*request);
      return;
    }

    if (command == "dolphin_freeze")
    {
      HandleFreeze(*request);
      return;
    }

    if (command == "dolphin_unfreeze")
    {
      HandleUnfreeze(*request);
      return;
    }

    if (command == "dolphin_findFreeMemory")
    {
      HandleFindFreeMemory(*request);
      return;
    }

    if (command == "dolphin_injectCode")
    {
      HandleInjectCode(*request);
      return;
    }

    if (command == "dolphin_detour")
    {
      HandleDetour(*request);
      return;
    }

    WARN_LOG_FMT(CONSOLE, "DAP: unhandled command {}", command);
    RespondError(request->seq, command, "unsupported");
  }

  void HandleSetBreakpoints(const Protocol::Request& request)
  {
    const SourceBreakpointContext context = ParseSourceBreakpointContext(request.arguments);
    const std::string source_key = MakeBreakpointSourceKey(context);

    const picojson::array* breakpoint_entries = nullptr;
    const auto breakpoints_it = request.arguments.find("breakpoints");
    if (breakpoints_it != request.arguments.end() && breakpoints_it->second.is<picojson::array>())
      breakpoint_entries = &breakpoints_it->second.get<picojson::array>();

    std::vector<SourceBreakpointSpec> specs;
    if (breakpoint_entries != nullptr)
    {
      for (const picojson::value& entry : *breakpoint_entries)
      {
        if (!entry.is<picojson::object>())
          continue;

        const picojson::object& entry_obj = entry.get<picojson::object>();
        const std::optional<u32> line = ReadNumericFromJson<u32>(entry_obj, "line");
        if (!line)
          continue;

        SourceBreakpointSpec spec;
        spec.line = *line;
        if (const std::optional<std::string> condition = ReadStringFromJson(entry_obj, "condition"))
          spec.condition = *condition;
        specs.push_back(std::move(spec));
      }
    }

    if (specs.empty())
    {
      const Protocol::SetBreakpointsArguments legacy = Protocol::ParseSetBreakpoints(request.arguments);
      if (legacy.base)
      {
        SourceBreakpointSpec spec;
        spec.line = 0;
        specs.push_back(std::move(spec));
      }
    }

    const std::vector<std::optional<u32>> addresses =
        m_controller.UpdateSourceBreakpoints(source_key, context, specs);

    picojson::array breakpoints;
    for (size_t i = 0; i < specs.size(); ++i)
    {
      picojson::object entry;
      const std::optional<u32>& address = addresses[i];
      entry.emplace("verified", address.has_value());
      if (address)
      {
        entry.emplace("id", static_cast<double>(AssignBreakpointId(*address)));
        entry.emplace("instructionReference", Json::FormatAddress(*address));
      }
      breakpoints.emplace_back(std::move(entry));
    }

    picojson::object body;
    body.emplace("breakpoints", std::move(breakpoints));
    Respond(request.seq, "setBreakpoints", std::move(body));
  }

  void HandleSetInstructionBreakpoints(const Protocol::Request& request)
  {
    const Protocol::SetInstructionBreakpointsArguments arguments =
        Protocol::ParseSetInstructionBreakpoints(request.arguments);

    std::vector<CodeBreakpointRequest> breakpoint_requests;
    picojson::array breakpoints;
    for (const Protocol::RequestedInstructionBreakpoint& breakpoint : arguments.breakpoints)
    {
      picojson::object entry;
      entry.emplace("verified", breakpoint.address.has_value());
      if (breakpoint.address)
      {
        entry.emplace("id", static_cast<double>(AssignBreakpointId(*breakpoint.address)));
        entry.emplace("instructionReference", Json::FormatAddress(*breakpoint.address));
        CodeBreakpointRequest bp;
        bp.address = *breakpoint.address;
        bp.condition = breakpoint.condition;
        breakpoint_requests.push_back(std::move(bp));
      }
      breakpoints.emplace_back(std::move(entry));
    }

    // DESNOTE(jbarber, 2026-07-03): Dolphin keeps a single code-breakpoint list,
    // so instruction breakpoints share storage with source breakpoints; each
    // authoritative set replaces the whole list (as the GDB stub also does).
    m_controller.UpdateInstructionBreakpoints(std::move(breakpoint_requests));

    picojson::object body;
    body.emplace("breakpoints", std::move(breakpoints));
    Respond(request.seq, "setInstructionBreakpoints", std::move(body));
  }

  void HandleGotoTargets(const Protocol::Request& request)
  {
    const Protocol::GotoTargetsArguments arguments = Protocol::ParseGotoTargets(request.arguments);

    picojson::array targets;
    if (arguments.address)
    {
      picojson::object target;
      // The address doubles as the target id so `goto` can resolve it statelessly.
      target.emplace("id", static_cast<double>(*arguments.address));
      target.emplace("label", Json::FormatAddress(*arguments.address));
      target.emplace("line", 0.0);
      target.emplace("instructionPointerReference", Json::FormatAddress(*arguments.address));
      targets.emplace_back(std::move(target));
    }

    picojson::object body;
    body.emplace("targets", std::move(targets));
    Respond(request.seq, "gotoTargets", std::move(body));
  }

  void HandleGoto(const Protocol::Request& request)
  {
    const std::optional<Protocol::GotoArguments> arguments = Protocol::ParseGoto(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "goto", "invalid goto arguments");
      return;
    }

    // DESNOTE(jbarber, 2026-07-21): Pause before SetPC so the post-goto
    // stopped event is truthful. Without this, if the client called goto
    // while emulation was running, the CPU would keep executing at the new
    // PC while the adapter told the client emulation halted -- the same
    // desync Restart previously had. Pause mirrors what Restart/Terminate
    // do before emitting a stopped event.
    if (!m_system.GetCPU().IsStepping())
      m_controller.Pause();
    m_controller.SetPC(arguments->target);
    Respond(request.seq, "goto", picojson::object{});
    SendStoppedEvent("goto");
    SyncSteppingBaseline();
  }

  void HandleExceptionInfo(const Protocol::Request& request)
  {
    const std::optional<ExceptionInfo> info = m_controller.GetExceptionInfo();
    if (!info)
    {
      RespondError(request.seq, "exceptionInfo", "no exception");
      return;
    }

    picojson::object body;
    body.emplace("exceptionId", fmt::format("0x{:08x}", info->exceptions));
    body.emplace("description", info->description);
    body.emplace("breakMode", std::string("always"));
    Respond(request.seq, "exceptionInfo", std::move(body));
  }

  void HandleLoadedSources(const Protocol::Request& request)
  {
    picojson::array sources;
    for (const LoadedSource& source : m_controller.GetLoadedSources())
    {
      picojson::object entry;
      entry.emplace("sourceReference", static_cast<double>(source.source_reference));
      entry.emplace("name", source.name);
      entry.emplace("path", source.path);
      sources.emplace_back(std::move(entry));
    }

    picojson::object body;
    body.emplace("sources", std::move(sources));
    Respond(request.seq, "loadedSources", std::move(body));
  }

  void HandleSource(const Protocol::Request& request)
  {
    const Protocol::SourceRequestArguments arguments =
        Protocol::ParseSourceRequest(request.arguments);
    if (!arguments.base)
    {
      RespondError(request.seq, "source", "invalid source arguments");
      return;
    }

    const std::optional<SourceContent> content =
        m_controller.GetSource(*arguments.base, arguments.start_line, arguments.end_line);
    if (!content)
    {
      RespondError(request.seq, "source", "source unavailable");
      return;
    }

    picojson::object body;
    body.emplace("content", content->content);
    body.emplace("mimeType", content->mime_type);
    Respond(request.seq, "source", std::move(body));
  }

  void HandleBreakpointLocations(const Protocol::Request& request)
  {
    const Protocol::BreakpointLocationsArguments arguments =
        Protocol::ParseBreakpointLocations(request.arguments);
    if (!arguments.base)
    {
      RespondError(request.seq, "breakpointLocations", "invalid breakpointLocations arguments");
      return;
    }

    picojson::array breakpoints;
    for (const BreakpointLocation& location : m_controller.GetBreakpointLocations(
             *arguments.base, arguments.start_line, arguments.end_line))
    {
      picojson::object entry;
      entry.emplace("line", static_cast<double>(location.line));
      breakpoints.emplace_back(std::move(entry));
    }

    picojson::object body;
    body.emplace("breakpoints", std::move(breakpoints));
    Respond(request.seq, "breakpointLocations", std::move(body));
  }

  void HandleTerminate(const Protocol::Request& request)
  {
    m_controller.Terminate();
    Respond(request.seq, "terminate", picojson::object{});

    picojson::object body;
    body.emplace("restart", false);
    QueueEvent("terminated", std::move(body));
    FlushEvents();
  }

  void HandleRestart(const Protocol::Request& request)
  {
    m_controller.Restart();
    Respond(request.seq, "restart", picojson::object{});
    SendStoppedEvent("restart");
    SyncSteppingBaseline();
  }

  void HandleSetDataBreakpoints(const Protocol::Request& request)
  {
    const Protocol::SetDataBreakpointsArguments arguments =
        Protocol::ParseSetDataBreakpoints(request.arguments);

    std::vector<DataBreakpointRequest> breakpoint_requests;
    picojson::array breakpoints;
    // DESNOTE(jbarber, 2026-07-21): Dolphin's TMemCheck store keys on
    // start_address and MemChecks::Add replaces an existing entry at the
    // same address, so a single setDataBreakpoints request that lists two
    // entries with the same dataId installs only the LAST -- earlier
    // entries are silently overwritten. Without de-dup, every entry was
    // reported verified:true and the client believed N watchpoints were
    // installed when only one was.
    //
    // The earlier de-dup marked later duplicates verified:false and skipped
    // them entirely, but Bugbot flagged that as wrong: a request listing
    // the same dataId twice with different length / accessType is the
    // client's way of REPLACING the prior config with a new range/access
    // kind. Dropping the later entry silently left the configured ranged
    // watch inactive while still telling the client nothing changed.
    //
    // New behavior: pass ALL entries through to the controller (so
    // MemChecks::Add's replace-on-same-address makes the LAST config win)
    // and report entries accordingly. For a duplicate address, mark the
    // PRIOR entry's response slot verified:false (it was overridden) and
    // the CURRENT entry verified:true (its config is the one now active).
    // That way the client sees which entry is the survivor and isn't told
    // a changed configuration is active when it has been replaced by a
    // later sibling.
    std::unordered_map<u32, size_t> first_index_for_address;
    for (const Protocol::RequestedDataBreakpoint& breakpoint : arguments.breakpoints)
    {
      picojson::object entry;
      entry.emplace("verified", breakpoint.address.has_value());
      if (breakpoint.address)
      {
        const u32 addr = *breakpoint.address;
        auto prev = first_index_for_address.find(addr);
        if (prev != first_index_for_address.end())
        {
          // Mark the prior entry (which will be overwritten by MemChecks::Add
          // when the controller processes this list) as not-verified in the
          // response so the client knows it isn't the live config.
          picojson::object& prev_obj = breakpoints[prev->second].get<picojson::object>();
          prev_obj["verified"] = picojson::value(false);
        }
        first_index_for_address[addr] = breakpoints.size();

        DataBreakpointRequest bp;
        bp.address = addr;
        bp.length = breakpoint.length;
        bp.read = breakpoint.read;
        bp.write = breakpoint.write;
        bp.condition = breakpoint.condition;
        breakpoint_requests.push_back(std::move(bp));
      }
      breakpoints.emplace_back(std::move(entry));
    }

    m_controller.SetDataBreakpoints(std::move(breakpoint_requests));

    picojson::object body;
    body.emplace("breakpoints", std::move(breakpoints));
    Respond(request.seq, "setDataBreakpoints", std::move(body));
  }

  void HandleEvaluate(const Protocol::Request& request)
  {
    const std::optional<Protocol::EvaluateArguments> arguments =
        Protocol::ParseEvaluate(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "evaluate", "invalid evaluate arguments");
      return;
    }

    const std::optional<std::string> result =
        m_controller.EvaluateExpression(arguments->expression);
    if (!result)
    {
      RespondError(request.seq, "evaluate", "invalid expression");
      return;
    }

    picojson::object body;
    body.emplace("result", *result);
    body.emplace("type", std::string("string"));
    Respond(request.seq, "evaluate", std::move(body));
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
      if (frame.source_file)
      {
        picojson::object source;
        source.emplace("path", *frame.source_file);
        const size_t slash = frame.source_file->find_last_of("/\\");
        const std::string name =
            slash != std::string::npos ? frame.source_file->substr(slash + 1) : *frame.source_file;
        source.emplace("name", name);
        entry.emplace("source", std::move(source));
        entry.emplace("line", static_cast<double>(frame.source_line));
      }
      else if (frame.source_base)
      {
        picojson::object source;
        source.emplace("name", Json::FormatAddress(*frame.source_base));
        source.emplace("path", Json::FormatAddress(*frame.source_base));
        entry.emplace("source", std::move(source));
        entry.emplace("line", static_cast<double>(frame.source_line));
      }
      else
      {
        entry.emplace("line", 0.0);
      }
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

    const std::optional<u32> address = ApplyOffset(arguments->address, arguments->offset);
    if (!address)
    {
      RespondError(request.seq, "readMemory", "address + offset out of range");
      return;
    }
    const std::vector<u8> bytes = m_controller.ReadMemory(*address, arguments->count);
    const u32 unreadable = arguments->count - static_cast<u32>(bytes.size());

    picojson::object body;
    body.emplace("address", Json::FormatAddress(*address));
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

    const std::optional<u32> address = ApplyOffset(arguments->address, arguments->offset);
    if (!address)
    {
      RespondError(request.seq, "writeMemory", "address + offset out of range");
      return;
    }
    const std::size_t written = m_controller.WriteMemory(*address, arguments->data);

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

    const std::optional<u32> base = ApplyOffset(arguments->address, arguments->offset);
    if (!base)
    {
      RespondError(request.seq, "disassemble", "address + offset out of range");
      return;
    }
    // `instructionOffset` is a signed instruction-word count; fold it in s64
    // so a negative offset (backwards disassembly) can't wrap past 0.
    const s64 effective = static_cast<s64>(*base) +
                          static_cast<s64>(arguments->instruction_offset) * 4;
    if (effective < 0 || effective > static_cast<s64>(std::numeric_limits<u32>::max()))
    {
      RespondError(request.seq, "disassemble", "instruction offset out of range");
      return;
    }
    u32 address = static_cast<u32>(effective);
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

  void HandleRealtimeWatch(const Protocol::Request& request)
  {
    const std::optional<Protocol::RealtimeWatchArguments> arguments =
        Protocol::ParseRealtimeWatch(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "dolphin_realtimeWatch", "invalid dolphin_realtimeWatch arguments");
      return;
    }

    // DESNOTE(jbarber, 2026-07-20): Subscribe-and-seed happens synchronously
    // here on the session thread; Tick() later runs on the CPU thread. The
    // client uses the returned watchId to correlate the asynchronous
    // `dolphin_memoryChanged` events and to cancel the subscription.
    const int watch_id = m_watch_sampler->AddSubscription(arguments->address, arguments->count);

    // DESNOTE(jbarber, 2026-07-21): AddSubscription returns kInvalidWatchId
    // (0) when the (address, count) pair is rejected (zero count, address+
    // count wrap, or count larger than physical RAM). The previous form
    // still reported success with watchId 0, misleading clients into
    // thinking a watch was active and waiting for events that never
    // arrive. Surface the rejection as an error response so the client
    // can re-issue or give up.
    if (watch_id == DAP::kInvalidWatchId)
    {
      RespondError(request.seq, "dolphin_realtimeWatch",
                   "rejected address/count (zero, overflow, or exceeds RAM)");
      return;
    }

    picojson::object body;
    body.emplace("watchId", static_cast<double>(watch_id));
    body.emplace("address", Json::FormatAddress(arguments->address));
    body.emplace("count", static_cast<double>(arguments->count));
    Respond(request.seq, "dolphin_realtimeWatch", std::move(body));
  }

  void HandleRealtimeWatchCancel(const Protocol::Request& request)
  {
    const std::optional<Protocol::RealtimeWatchCancelArguments> arguments =
        Protocol::ParseRealtimeWatchCancel(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "dolphin_realtimeWatchCancel",
                   "invalid dolphin_realtimeWatchCancel arguments");
      return;
    }

    if (!m_watch_sampler->RemoveSubscription(arguments->watch_id))
    {
      RespondError(request.seq, "dolphin_realtimeWatchCancel", "no such watch");
      return;
    }

    Respond(request.seq, "dolphin_realtimeWatchCancel", picojson::object{});
  }

  void HandleFreeze(const Protocol::Request& request)
  {
    // DESNOTE(jbarber, 2026-07-21): `dolphin_freeze` holds a memory region at
    // a fixed value: the sampler writes the frozen bytes back into memory
    // every field where it observes drift, instead of dispatching a
    // `dolphin_memoryChanged` event. Two forms are accepted (see
    // ParseFreeze): a standalone `memoryReference + count + data` that
    // creates a new frozen subscription, or `watchId + data` that freezes an
    // existing watch in place. The former's address/count are echoed back in
    // the response so a client that doesn't track the subscription can still
    // correlate freezes with addresses.
    const std::optional<Protocol::FreezeArguments> arguments =
        Protocol::ParseFreeze(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "dolphin_freeze", "invalid dolphin_freeze arguments");
      return;
    }

    int watch_id = 0;
    u32 address = 0;
    u32 count = 0;
    if (arguments->watch_id)
    {
      // Form 2: freeze an existing watch. Address/count come from the
      // subscription itself; we can't trust anything the client supplied.
      watch_id = *arguments->watch_id;
      if (!m_watch_sampler->Freeze(watch_id, std::move(arguments->value)))
      {
        RespondError(request.seq, "dolphin_freeze",
                     "no such watch_id or value size mismatch");
        return;
      }
      // Reuse the address/count from the subscription so the response can
      // echo them; for this we lean on ParseFreeze not populating them in
      // form 2. The response below does not include them in form 2.
    }
    else
    {
      // Form 1: standalone freeze. Create the subscription and freeze it.
      // AddSubscription seeds last_seen with the current memory contents;
      // Freeze then overwrites last_seen with the frozen canon so the next
      // Tick() treats the canon as the baseline and only writes back when
      // the cell drifts. We pre-create the subscription under our own
      // thread (no Tick contention because we hold no CPU-thread guard
      // here -- AddSubscription takes its own).
      address = *arguments->address;
      count = *arguments->count;
      watch_id = m_watch_sampler->AddSubscription(address, count);
      if (watch_id == DAP::kInvalidWatchId)
      {
        RespondError(request.seq, "dolphin_freeze", "rejected address/count (overflow?)");
        return;
      }
      if (!m_watch_sampler->Freeze(watch_id, std::move(arguments->value)))
      {
        // Should be unreachable: AddSubscription just succeeded at this
        // count, so Freeze's size check must pass. Defensively roll back.
        m_watch_sampler->RemoveSubscription(watch_id);
        RespondError(request.seq, "dolphin_freeze", "internal error: freeze-after-add failed");
        return;
      }
    }

    picojson::object body;
    body.emplace("watchId", static_cast<double>(watch_id));
    if (!arguments->watch_id)
    {
      body.emplace("address", Json::FormatAddress(address));
      body.emplace("count", static_cast<double>(count));
    }
    Respond(request.seq, "dolphin_freeze", std::move(body));
  }

  void HandleUnfreeze(const Protocol::Request& request)
  {
    const std::optional<Protocol::UnfreezeArguments> arguments =
        Protocol::ParseUnfreeze(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "dolphin_unfreeze", "invalid dolphin_unfreeze arguments");
      return;
    }

    if (!m_watch_sampler->Unfreeze(arguments->watch_id))
    {
      RespondError(request.seq, "dolphin_unfreeze", "no such watch");
      return;
    }

    Respond(request.seq, "dolphin_unfreeze", picojson::object{});
  }

  void HandleFindFreeMemory(const Protocol::Request& request)
  {
    // DESNOTE(jbarber, 2026-07-21): Scans MEM1 for the smallest 4-byte-aligned
    // zero-run >= `count` and returns its address. Integrators that want a
    // code cave but don't know the game's memory layout use this to let the
    // server pick a safe address.
    const std::optional<Protocol::FindFreeMemoryArguments> arguments =
        Protocol::ParseFindFreeMemory(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "dolphin_findFreeMemory",
                   "invalid dolphin_findFreeMemory arguments");
      return;
    }
    const std::optional<u32> address = m_controller.FindFreeMemory(arguments->count);
    if (!address)
    {
      RespondError(request.seq, "dolphin_findFreeMemory", "no free region of that size");
      return;
    }
    picojson::object body;
    body.emplace("address", Json::FormatAddress(*address));
    body.emplace("count", static_cast<double>(arguments->count));
    Respond(request.seq, "dolphin_findFreeMemory", std::move(body));
  }

  void HandleInjectCode(const Protocol::Request& request)
  {
    // DESNOTE(jbarber, 2026-07-21): Writes PPC machine code at an
    // explicitly-provided or server-allocated address. The integrator
    // supplies raw bytes (base64-encoded); the server doesn't assemble.
    // WriteMemory's iCache+JIT invalidation ensures the injected bytes are
    // observed by the next fetch in both interpreter and JIT modes.
    const std::optional<Protocol::InjectCodeArguments> arguments =
        Protocol::ParseInjectCode(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "dolphin_injectCode", "invalid dolphin_injectCode arguments");
      return;
    }
    // DESNOTE(jbarber, 2026-07-21): When no address is supplied, allocate
    // ONCE here and pass it down to InjectCode. The previous form called
    // FindFreeMemory as a predicate pre-check ("is there ANY free region?")
    // and let InjectCode scan again to find the actual cave. With the core
    // running, writes between those two scans can change the free-map, so
    // the pre-check passes but the inject-time scan returns a different
    // address (or none) -- the pre-check promised feasibility that the
    // inject didn't deliver. Allocating up front and threading the chosen
    // address through makes the response truthful and avoids the second
    // scan entirely.
    std::optional<u32> resolved_address = arguments->address;
    if (!resolved_address)
    {
      auto alloc = m_controller.FindFreeMemory(static_cast<u32>(arguments->code.size()));
      if (!alloc)
      {
        RespondError(request.seq, "dolphin_injectCode", "no free region of that size");
        return;
      }
      resolved_address = *alloc;
    }
    const u32 address = m_controller.InjectCode(resolved_address, arguments->code);
    if (address == 0)
    {
      RespondError(request.seq, "dolphin_injectCode", "write failed (invalid address?)");
      return;
    }
    picojson::object body;
    body.emplace("address", Json::FormatAddress(address));
    body.emplace("count", static_cast<double>(arguments->code.size()));
    Respond(request.seq, "dolphin_injectCode", std::move(body));
  }

  void HandleDetour(const Protocol::Request& request)
  {
    // DESNOTE(jbarber, 2026-07-21): Transparent-detour pattern. The server:
    //   1. Allocates detour_address + trampoline_address (if detour_address
    //      is omitted, finds free memory big enough for both).
    //   2. Writes detour_body at detour_address, appends `b trampoline`.
    //   3. Writes trampoline: original_instruction + `b target+4`.
    //   4. Patches target_address with `b detour_address`.
    // The detour body should end with `b trampoline_address` (or fall
    // through to the implicit appended one) to transparently resume the
    // patched-out instruction and continue after the patch site.
    const std::optional<Protocol::DetourArguments> arguments =
        Protocol::ParseDetour(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, "dolphin_detour", "invalid dolphin_detour arguments");
      return;
    }
    auto result = m_controller.Detour(arguments->target_address, arguments->detour_address,
                                      arguments->detour_body);
    if (!result)
    {
      RespondError(request.seq, "dolphin_detour",
                   "detour failed (invalid target or no free memory?)");
      return;
    }
    picojson::object body;
    body.emplace("targetAddress", Json::FormatAddress(result->target_address));
    body.emplace("detourAddress", Json::FormatAddress(result->detour_address));
    body.emplace("trampolineAddress", Json::FormatAddress(result->trampoline_address));
    body.emplace("originalInstruction", Json::Base64Encode(result->original_instruction));
    Respond(request.seq, "dolphin_detour", std::move(body));
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
  std::unique_ptr<RealtimeWatchSampler> m_watch_sampler;
  std::atomic<bool> m_running{true};
  // Sequence numbers are handed out from both the session loop (responses) and
  // the core state-changed callback (the "continued" event), which may run on
  // the CPU thread; keep allocation race-free.
  std::atomic<int> m_next_seq{1};
  bool m_was_stepping = false;
  bool m_debugging_started = false;
  // Stop-on-entry policy. Seeded from `Dolphin.General.DAPStopOnEntry` at
  // construction (so it can be configured at dolphin launch without a
  // per-session request); an explicit `stopOnEntry` on `launch`/`attach`
  // overrides it for the session. Consulted by `MaybeFireEntryStop`.
  bool m_stop_on_entry = true;
  // DESNOTE(jbarber, 2026-07-21): Entry-stop decision is deferred until BOTH
  // sides of the protocol have signaled readiness: the `launch`/`attach`
  // request (which may carry a `stopOnEntry` override) AND the
  // `configurationDone` request. Previously configurationDone fired the
  // entry stop immediately when it arrived before launch, which silently
  // dropped any later `launch` `stopOnEntry=false` override. With the gate
  // below, a configDone-before-launch ordering waits for launch to apply
  // its override before committing the policy.
  bool m_launch_seen = false;
  bool m_config_done = false;
  // DESNOTE(jbarber, 2026-07-21): Remember whether the session was started
  // via `launch` or `attach`; the deferred entry-stop fires from whichever
  // of {launch, configurationDone} arrives second, and the event reason
  // ("entry" vs "attach") must reflect the original launch/attach kind, not
  // the gate that triggered the fire.
  std::string m_launch_kind = "launch";
  // Guards the entry-stop so it fires exactly once across launch/attach and
  // configurationDone, even when a client interleaves them.
  bool m_entry_stop_sent = false;

  // Stop reason captured synchronously by the state hook at break time (on the
  // CPU thread), before EXCEPTION_FAKE_MEMCHECK_HIT is cleared. Consumed by
  // SendClassifiedStoppedEvent on the session thread.
  std::mutex m_stop_info_mutex;
  std::optional<StopInfo> m_pending_stop_info;

  std::mutex m_event_mutex;
  std::vector<std::string> m_pending_events;

  // DESNOTE(jbarber, 2026-07-21): DAP `hitBreakpointIds` in a stopped event
  // must echo the stable `id` the client received in its
  // setBreakpoints/setInstructionBreakpoints response, NOT the raw PC where
  // the breakpoint hit (clients correlate via the id and would mis-associate
  // a hit if we shipped the address instead). We assign a monotonic id per
  // installed breakpoint site and remember the (address -> id) mapping so
  // SendClassifiedStoppedEvent can translate the hit PC back to an id.
  std::mutex m_bp_id_mutex;
  std::unordered_map<u32, int> m_bp_id_by_address;
  int m_next_bp_id = 1;

  // DESNOTE(jbarber, 2026-07-21): Asynchronous step-out worker. The previous
  // form ran DapDebugController::StepOut inline on the session thread, which
  // holds a CPUThreadGuard for up to its full 5s timeout while single-stepping
  // the interpreter. That blocked `HandleMessage` from polling the socket, so
  // disconnect, new requests, and realtime-watch event dispatch stalled until
  // the step completed. We now run StepOut on a worker thread so the session
  // loop keeps draining its 50ms `WaitForReadable` poll, and `PollBreakpointStop`
  // joins the worker and emits the `stopped`/`step` event when it completes.
  // The worker captures a shared_ptr<Session> (rather than `this`) so a late
  // tear-down can't free the Session while the worker still holds the guard.
  //
  // `m_pending_continue` / `m_pending_pause` are set when the client issues
  // continue/pause while a step-out worker is still holding the stepping lock.
  // We respond success immediately and defer the actual SetState call to
  // PollBreakpointStop (which runs after the worker joins), so the session
  // thread never blocks on the stepping lock.
  std::thread m_step_out_thread;
  std::atomic<bool> m_step_out_done{true};
  std::atomic<bool> m_pending_continue{false};
  std::atomic<bool> m_pending_pause{false};
};
}  // namespace

void RunSession(DapTransport& transport, Core::System& system)
{
  // DESNOTE(jbarber, 2026-07-21): Session derives from enable_shared_from_this
  // so the realtime-watch sampler's dispatch lambda can capture a weak_ptr and
  // keep *this alive across an in-flight vi_end_field_event Tick() on the CPU
  // thread. Constructing here via make_shared (rather than a stack local)
  // makes weak_from_this() valid by the time Run() is entered.
  std::make_shared<Session>(transport, system)->Run();
}
}  // namespace DAP

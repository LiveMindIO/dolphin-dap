// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapSession.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapDebugController.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/Debugger/DAP/DapMemoryEngine.h"
#include "Core/Debugger/DAP/DapProtocol.h"
#include "Core/Debugger/DAP/DapRealtimeWatch.h"
#include "Core/Debugger/DAP/DapTransport.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
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

std::optional<std::string> ReadSourceString(const picojson::object& source, const std::string& key)
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
  if (const picojson::object* source = GetObject(arguments, "source"))
  {
    if (const std::optional<double> source_reference =
            ReadNumericFromJson<double>(*source, "sourceReference");
        source_reference && std::isfinite(*source_reference) && *source_reference > 0 &&
        std::floor(*source_reference) == *source_reference &&
        *source_reference <=
            static_cast<double>(MakeDisassemblySourceReference(std::numeric_limits<u32>::max())))
    {
      context.source_reference = static_cast<SourceReference>(*source_reference);
    }
    context.source_name = ReadSourceString(*source, "name");
    context.source_path = ReadSourceString(*source, "path");
    if (const picojson::object* adapter_data = GetObject(*source, "adapterData"))
    {
      if (const std::optional<double> source_id =
              ReadNumericFromJson<double>(*adapter_data, "dolphinSourceId");
          source_id && std::isfinite(*source_id) && *source_id > 0 &&
          std::floor(*source_id) == *source_id &&
          *source_id <= static_cast<double>(std::numeric_limits<u32>::max()))
      {
        context.source_id = static_cast<u32>(*source_id);
      }
    }
  }
  return context;
}

std::string MakeBreakpointSourceKey(const SourceBreakpointContext& context)
{
  if (context.source_id)
    return fmt::format("source:{}", *context.source_id);
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

// Memory watchpoint ownership remains global for now. Serialize first/last DAP
// transitions for that legacy teardown and the shared entry-stop gate. Code
// breakpoints are independently owned by the Core BreakPoints registry.
std::mutex s_session_lifetime_mutex;
int s_active_session_count = 0;

// DESNOTE(jbarber, 2026-07-22): MaybeFireEntryStop decides whether the core
// continues or stays paused on entry/attach. Without a shared gate, each
// session makes the decision independently: a second client with
// `stopOnEntry:false` calls Continue() on the shared core even if a first
// client is still expecting a paused entry stop. The first session to reach
// MaybeFireEntryStop test-and-sets this flag; any later session's call is a
// no-op. The first session's `stopOnEntry` policy wins for the lifetime of
// the boot -- concurrent clients with different policies is a documented
// limitation (see Tools/dap/README.md). Reset when the last session exits so
// a DAP re-init within the same process boot starts fresh. Bugbot #67.
std::atomic<bool> s_entry_stop_handled{false};

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
    m_step_cancelled.store(true);
    if (m_step_out_thread.joinable())
      m_step_out_thread.join();
  }

  Session(DapTransport& transport, Core::System& system, const SessionTestHooks* test_hooks)
      : m_transport(transport), m_controller(system), m_system(system),
        m_stop_on_entry(Config::Get(Config::MAIN_DAP_STOP_ON_ENTRY)), m_test_hooks(test_hooks)
  {
  }

  void Run()
  {
    if (!RunHandshake())
      return;

    {
      std::lock_guard lock(s_session_lifetime_mutex);
      ++s_active_session_count;
    }

    // DESNOTE(jbarber, 2026-07-21): Construct the realtime-watch sampler here
    // (not in the ctor) so its dispatch lambda can capture a weak_ptr to this
    // Session via weak_from_this() -- which is only valid once *this is owned
    // by a shared_ptr (RunSession constructs the Session via make_shared before
    // calling Run()). The weak_ptr lock keeps the Session alive for the
    // duration of an in-flight Tick() on the CPU thread, so disconnect/
    // shutdown can't race the dispatch and touch a destroyed Session. When the
    // weak_ptr can't be locked (session torn down), the dispatch is a no-op.
    auto self = shared_from_this();
    m_controller.SetBreakpointEventCallback(
        [weak = std::weak_ptr<Session>(self)](std::shared_ptr<const BreakPoints::Event> event) {
          if (auto sp = weak.lock())
            sp->HandleBreakpointEvent(std::move(event));
        });
    m_controller.SetDataBreakpointEventCallback(
        [weak = std::weak_ptr<Session>(self)](std::shared_ptr<const MemChecks::Event> event) {
          if (auto sp = weak.lock())
            sp->HandleDataBreakpointEvent(std::move(event));
        });
    m_controller.SetExecutionEventCallback(
        [weak = std::weak_ptr<Session>(self)](
            std::shared_ptr<const Core::Debug::ExecutionEvent> event) {
          if (auto sp = weak.lock())
            sp->HandleExecutionEvent(std::move(event));
        });
    m_watch_sampler = std::make_unique<RealtimeWatchSampler>(
        m_system,
        [weak = std::weak_ptr<Session>(self)](const std::vector<RealtimeWatchChange>& changes) {
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

    m_memory_engine = std::make_unique<DapMemoryEngine>(
        m_system, [weak = std::weak_ptr<Session>(self)](MemoryScanTerminalEvent terminal) {
          if (auto sp = weak.lock())
          {
            picojson::object body;
            body.emplace("scanId", static_cast<double>(terminal.scan_id));
            body.emplace("jobId", static_cast<double>(terminal.job_id));
            body.emplace("generation", static_cast<double>(terminal.generation));
            body.emplace("resultCount", static_cast<double>(terminal.result_count));
            body.emplace("durationMilliseconds", static_cast<double>(terminal.duration_ms));
            body.emplace("pauseDuringScan", terminal.pause_during_scan);
            if (!terminal.message.empty())
              body.emplace("message", std::move(terminal.message));
            sp->QueueEvent(terminal.event, std::move(body));
          }
        });

    while (m_running)
    {
      PollAsyncStepAndEvents();

      if (!WaitForReadable(m_transport.GetSocket(), 50))
        continue;

      // A step worker can finish while WaitForReadable is blocked on an already queued request.
      // Re-poll before dispatch so its stopped event cannot be overtaken by that request's
      // response.
      PollAsyncStepAndEvents();

      const std::optional<std::string> message = m_transport.ReadMessage();
      if (!message)
        break;

      if (message->empty())
        continue;

      HandleMessage(*message);
    }

    // Stop and join the scan worker before tearing down the event queue. The
    // peer has gone away, so suppress the terminal cancellation event.
    {
      std::lock_guard lock(m_event_mutex);
      m_accept_events = false;
      m_pending_events.clear();
    }
    m_memory_engine.reset();
    // Tear down the sampler before the rest of the Session so its
    // vi_end_field_event hook is unregistered (no further Tick() will fire)
    // before the transport/event-queue members are destroyed.
    m_watch_sampler.reset();
    // DESNOTE(jbarber, 2026-07-22): Join the async step-out worker (if
    // running) BEFORE clearing breakpoints. The worker captures a
    // shared_ptr<Session> and calls m_controller.StepOut(), which uses the
    // global PPC BreakPoints / MemChecks store. Clearing breakpoints while
    // the worker is still mid-StepOut races that store -- the worker could
    // install a temporary breakpoint that ClearBreakpoints wipes, or vice
    // versa. The destructor also joins, but it runs AFTER this teardown
    // path, so ClearBreakpoints would have already raced. Bugbot #72.
    m_step_cancelled.store(true);
    if (m_step_out_thread.joinable())
      m_step_out_thread.join();
    m_system.GetCPU().GetExecutionState().UnregisterClient(m_controller.GetExecutionClientId());
    m_controller.ClearBreakpoints();
    {
      std::lock_guard lock(s_session_lifetime_mutex);
      if (--s_active_session_count == 0)
        s_entry_stop_handled.store(false);
    }
  }

private:
  void QueueEvent(std::string_view event, picojson::object body)
  {
    std::lock_guard lock(m_event_mutex);
    if (!m_accept_events)
      return;
    m_pending_events.emplace_back(std::string(event), std::move(body));
  }

  void HandleBreakpointEvent(std::shared_ptr<const BreakPoints::Event> event)
  {
    if (event->origin == m_controller.GetBreakpointClientId())
      return;

    for (const BreakPoints::Change& change : event->changes)
    {
      picojson::object breakpoint;
      breakpoint.emplace("id", static_cast<double>(AssignBreakpointId(change.breakpoint.address)));
      breakpoint.emplace("verified", change.reason != BreakPoints::ChangeReason::Removed);
      breakpoint.emplace("instructionReference", Json::FormatAddress(change.breakpoint.address));

      picojson::object body;
      switch (change.reason)
      {
      case BreakPoints::ChangeReason::New:
        body.emplace("reason", std::string("new"));
        break;
      case BreakPoints::ChangeReason::Changed:
        body.emplace("reason", std::string("changed"));
        break;
      case BreakPoints::ChangeReason::Removed:
        body.emplace("reason", std::string("removed"));
        break;
      }
      body.emplace("breakpoint", std::move(breakpoint));
      QueueEvent("breakpoint", std::move(body));
    }
  }

  void HandleDataBreakpointEvent(std::shared_ptr<const MemChecks::Event> event)
  {
    if (event->origin == m_controller.GetMemCheckClientId())
      return;

    for (const MemChecks::Change& change : event->changes)
    {
      picojson::object breakpoint;
      breakpoint.emplace(
          "id", static_cast<double>(AssignDataBreakpointId(change.breakpoint.start_address,
                                                           change.breakpoint.end_address)));
      breakpoint.emplace("verified", change.reason != MemChecks::ChangeReason::Removed);
      breakpoint.emplace("instructionReference",
                         Json::FormatAddress(change.breakpoint.start_address));

      picojson::object body;
      switch (change.reason)
      {
      case MemChecks::ChangeReason::New:
        body.emplace("reason", std::string("new"));
        break;
      case MemChecks::ChangeReason::Changed:
        body.emplace("reason", std::string("changed"));
        break;
      case MemChecks::ChangeReason::Removed:
        body.emplace("reason", std::string("removed"));
        break;
      }
      body.emplace("breakpoint", std::move(breakpoint));
      QueueEvent("breakpoint", std::move(body));
    }
  }

  void HandleExecutionEvent(std::shared_ptr<const Core::Debug::ExecutionEvent> event)
  {
    if (event->stop_cause == Core::Debug::ExecutionStopCause::Entry &&
        (!m_launch_seen || !m_config_done))
    {
      return;
    }
    m_invalidate_debug_values.store(true);
    if (event->kind == Core::Debug::ExecutionEventKind::Continued)
    {
      picojson::object body;
      body.emplace("threadId", 1.0);
      body.emplace("allThreadsContinued", true);
      QueueEvent("continued", std::move(body));
      return;
    }

    picojson::object body;
    body.emplace("threadId", 1.0);
    switch (event->stop_cause.value_or(Core::Debug::ExecutionStopCause::Step))
    {
    case Core::Debug::ExecutionStopCause::Unknown:
    case Core::Debug::ExecutionStopCause::UserPause:
      body.emplace("reason", std::string("pause"));
      break;
    case Core::Debug::ExecutionStopCause::CodeBreakpoint:
      body.emplace("reason", std::string("breakpoint"));
      break;
    case Core::Debug::ExecutionStopCause::DataBreakpoint:
      body.emplace("reason", std::string("data breakpoint"));
      break;
    case Core::Debug::ExecutionStopCause::Exception:
      body.emplace("reason", std::string("exception"));
      break;
    case Core::Debug::ExecutionStopCause::Goto:
      body.emplace("reason", std::string("goto"));
      break;
    case Core::Debug::ExecutionStopCause::Entry:
      body.emplace("reason",
                   m_launch_kind == "attach" ? std::string("attach") : std::string("entry"));
      break;
    case Core::Debug::ExecutionStopCause::Restart:
      body.emplace("reason", std::string("restart"));
      break;
    case Core::Debug::ExecutionStopCause::Terminate:
      body.emplace("reason", std::string("pause"));
      break;
    case Core::Debug::ExecutionStopCause::Step:
      body.emplace("reason", std::string("step"));
      break;
    }

    std::optional<int> hit_id;
    if (event->code_breakpoint_address)
      hit_id = LookupBreakpointId(*event->code_breakpoint_address);
    else if (event->watchpoint_start)
      hit_id = LookupDataBreakpointId(*event->watchpoint_start);
    if (hit_id)
    {
      picojson::array hit_ids;
      hit_ids.emplace_back(static_cast<double>(*hit_id));
      body.emplace("hitBreakpointIds", std::move(hit_ids));
    }
    QueueEvent("stopped", std::move(body));
  }

  void FlushEvents()
  {
    std::vector<std::pair<std::string, picojson::object>> events;
    {
      std::lock_guard lock(m_event_mutex);
      events.swap(m_pending_events);
    }

    for (auto& [event, body] : events)
      m_transport.WriteMessage(
          Protocol::Serialize(Protocol::MakeEvent(m_next_seq++, event, std::move(body))));
  }

  void FlushEventsThroughPriorMemoryScanTerminal(int job_id)
  {
    std::vector<std::pair<std::string, picojson::object>> events;
    {
      std::lock_guard lock(m_event_mutex);
      size_t count = 0;
      for (size_t i = 0; i < m_pending_events.size(); ++i)
      {
        const auto& [event, body] = m_pending_events[i];
        const bool terminal = event == "dolphin_memoryScanCompleted" ||
                              event == "dolphin_memoryScanFailed" ||
                              event == "dolphin_memoryScanCancelled";
        const auto body_job = body.find("jobId");
        if (terminal && body_job != body.end() && body_job->second.is<double>() &&
            body_job->second.get<double>() < job_id)
        {
          count = i + 1;
        }
      }
      events.reserve(count);
      for (size_t i = 0; i < count; ++i)
        events.emplace_back(std::move(m_pending_events[i]));
      m_pending_events.erase(m_pending_events.begin(), m_pending_events.begin() + count);
    }
    for (auto& [event, body] : events)
      m_transport.WriteMessage(
          Protocol::Serialize(Protocol::MakeEvent(m_next_seq++, event, std::move(body))));
  }

  void SendStoppedEvent(std::string_view reason)
  {
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
    // DESNOTE(jbarber, 2026-07-22): Only the first session to reach this
    // point makes the continue/stay-paused decision; later sessions observe
    // the resulting core State and emit a matching lifecycle event so the
    // client isn't left hanging without a `stopped`/`continued`. Without
    // this gate, a second client with stopOnEntry:false would resume the
    // shared core out from under a first client that was still expecting a
    // paused entry stop. Bugbot #67 + #69.
    bool expected = false;
    if (!s_entry_stop_handled.compare_exchange_strong(expected, true))
    {
      // First session already made the decision -- emit a lifecycle event
      // reflecting the CURRENT core state so this client knows whether
      // they're paused or running. Without this, the second session's
      // client would hang waiting for a stopped/continued that already
      // happened. Bugbot #69.
      if (m_system.GetCPU().GetState() == CPU::State::Running)
      {
        picojson::object body;
        body.emplace("threadId", 1.0);
        body.emplace("allThreadsContinued", true);
        QueueEvent("continued", std::move(body));
      }
      else
      {
        SendStoppedEvent(m_launch_kind == "attach" ? "attach" : "entry");
      }
      FlushEvents();
      return;
    }
    if (m_stop_on_entry)
    {
      // DESNOTE(jbarber, 2026-07-21): Force the core into Paused before
      // emitting the stop event. CPUSetInitialExecutionState already pauses
      // when DAP is active, but a previous launch/attach with stopOnEntry
      // false (or the client continuing the core out-of-band) leaves State
      // at Running -- telling the client execution stopped while the game
      // kept running would lie.
      m_controller.PauseWithoutEvent();
      m_controller.PublishEntryStop();
    }
    else
    {
      // Core starts paused while a debugger is attached (see
      // CPUSetInitialExecutionState in Core.cpp); resume it so the game runs
      // immediately. The shared execution service queues the resulting event.
      ContinueCore();
    }
  }

  void ContinueCore()
  {
    ClearDebugValueHandles();
    static_cast<void>(m_controller.Continue());
  }

  bool JoinCompletedStepWorker(const Protocol::Request& request)
  {
    if (!m_step_out_thread.joinable())
      return true;
    if (!m_step_out_done.load())
    {
      RespondError(request.seq, request.command, "step already in progress");
      return false;
    }
    m_step_out_thread.join();
    return true;
  }

  bool CancelAndJoinStepWorker(const Protocol::Request& request)
  {
    const auto cancelled = m_controller.CancelActiveStep();
    if (!cancelled)
    {
      RespondError(request.seq, request.command, cancelled.error());
      return false;
    }
    m_step_cancelled.store(true);
    if (m_step_out_thread.joinable())
      m_step_out_thread.join();
    m_step_out_done.store(true);
    return true;
  }

  void DiscardQueuedStoppedEvents()
  {
    // A mutation request supersedes a completed step stop that is still queued locally; publishing
    // both would present an obsolete intermediate state after the request has already taken effect.
    std::lock_guard lock(m_event_mutex);
    std::erase_if(m_pending_events, [](const auto& event) { return event.first == "stopped"; });
  }

  void StartSourceStep(const Protocol::Request& request, const bool step_over)
  {
    if (!JoinCompletedStepWorker(request))
      return;
    m_step_cancelled.store(false);
    m_step_out_done.store(false);
    const auto operation =
        m_controller.BeginStep(step_over ? Core::Debug::ExecutionOperationKind::SourceStepOver :
                                           Core::Debug::ExecutionOperationKind::SourceStepInto,
                               &m_step_cancelled);
    if (!operation)
    {
      m_step_out_done.store(true);
      RespondError(request.seq, request.command, operation.error());
      return;
    }
    if (!m_system.GetCPU().IsStepping())
      m_controller.PauseWithoutEvent();
    Respond(request.seq, request.command, picojson::object{});
    m_controller.PublishStepContinued(*operation);
    m_step_out_thread = std::thread([self = shared_from_this(), step_over, operation = *operation] {
      const Core::Debug::PPCStepResult result =
          self->m_controller.StepSource(step_over, self->m_step_cancelled);
      self->m_step_out_done.store(true);
      if (self->m_step_cancelled.load())
        self->m_controller.AbandonStep(operation);
      else if (result == Core::Debug::PPCStepResult::Stepped)
        self->m_controller.CompleteStep(operation);
      else if (result == Core::Debug::PPCStepResult::Continuing)
        self->m_controller.PublishStepContinued(operation);
      else
        self->m_controller.AbandonStep(operation);
    });
  }

  // DESNOTE(jbarber, 2026-07-21): Assign a stable DAP id for a breakpoint
  // site installed at `address` and remember the (address -> id) mapping so
  // HandleExecutionEvent can translate a hit PC back to the id the
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

  int AssignDataBreakpointId(u32 start_address, u32 end_address)
  {
    std::lock_guard lock(m_bp_id_mutex);
    const std::pair key{start_address, end_address};
    if (const auto it = m_data_bp_id_by_range.find(key); it != m_data_bp_id_by_range.end())
      return it->second;
    const int id = m_next_bp_id++;
    m_data_bp_id_by_range.emplace(key, id);
    return id;
  }

  std::optional<int> LookupDataBreakpointId(u32 address)
  {
    std::lock_guard lock(m_bp_id_mutex);
    for (const auto& [range, id] : m_data_bp_id_by_range)
    {
      if (range.first <= address && address <= range.second)
        return id;
    }
    return std::nullopt;
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

  void PollAsyncStepAndEvents()
  {
    auto core_scan_lock = DapMemoryEngine::TryLockCoreScan();
    if (!core_scan_lock.owns_lock())
    {
      // CPUThreadGuard temporarily puts the CPU in stepping state while a scan
      // snapshot is captured (and for the whole job with pauseDuringScan).
      // That is not a debugger-visible stop.
      FlushEvents();
      return;
    }
    // Async step completion: once stepping returns, the worker marks itself ready to join before
    // publishing its result. Joining guarantees that publication is complete; flush immediately
    // afterward, on the session thread, so the client sees the stopped event
    // in command order. The step-out may have stopped early because the
    // interpreter loop hit a code breakpoint (StepOut bails when
    // CheckBreakPoints fires). The shared execution event carries that exact
    // stop cause. This replaces the previous synchronous inline call that
    // blocked HandleMessage for up to the 5s step-out timeout.
    if (m_step_out_thread.joinable() && m_step_out_done.load())
    {
      m_step_out_thread.join();
      if (m_test_hooks && m_test_hooks->async_step_worker_joined)
        m_test_hooks->async_step_worker_joined();
      FlushEvents();
      return;
    }

    if (m_invalidate_debug_values.exchange(false))
      ClearDebugValueHandles();
    FlushEvents();
  }

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
    capabilities.emplace("supportsDolphinMemoryRegions", true);
    capabilities.emplace("supportsDolphinMemoryScan", true);
    capabilities.emplace("supportsDolphinPointerChain", true);

    // The initialize response body is the Capabilities object itself. Wrapping
    // it in a `capabilities` property hides every advertised feature from
    // standard clients such as nvim-dap, including configurationDone support.
    return capabilities;
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
      m_step_cancelled.store(true);
      m_running = false;
      Respond(request->seq, command, picojson::object{});
      return;
    }

    // A full-pause scan worker owns CPUThreadGuard through filtering and commit.
    // Reject operations that could acquire the same core lock so the session
    // remains able to receive cancellation and disconnect requests.
    const bool scan_safe_command =
        command == "dolphin_memoryRegions" || command == "dolphin_memoryScanStatus" ||
        command == "dolphin_memoryScanResults" || command == "dolphin_memoryScanCancel" ||
        command == "dolphin_memoryScanDispose" || command == "dolphin_memoryScanUndo" ||
        command == "dolphin_memoryScanRemoveResults";
    auto core_scan_lock = DapMemoryEngine::TryLockCoreScan();
    if (!scan_safe_command && !core_scan_lock.owns_lock())
    {
      RespondError(request->seq, command, "memory scan pause is active");
      return;
    }

    if (command == "continue")
    {
      // Stop the session-owned worker and release its CPUThreadGuard before resuming the core.
      if (!CancelAndJoinStepWorker(*request))
        return;
      DiscardQueuedStoppedEvents();
      ClearDebugValueHandles();
      const auto continued = m_controller.Continue();
      if (!continued)
      {
        RespondError(request->seq, command, continued.error());
        return;
      }
      picojson::object body;
      body.emplace("allThreadsContinued", true);
      Respond(request->seq, command, std::move(body));
      return;
    }

    if (command == "pause")
    {
      if (!CancelAndJoinStepWorker(*request))
        return;
      DiscardQueuedStoppedEvents();
      const auto paused = m_controller.Pause();
      if (!paused)
      {
        RespondError(request->seq, command, paused.error());
        return;
      }
      Respond(request->seq, command, picojson::object{});
      return;
    }

    if (command == "next")
    {
      ClearDebugValueHandles();
      const auto granularity = Protocol::ParseSteppingGranularity(request->arguments);
      if (!granularity)
      {
        RespondError(request->seq, command, "invalid stepping granularity");
        return;
      }
      if (*granularity != Protocol::SteppingGranularity::Instruction)
      {
        StartSourceStep(*request, true);
        return;
      }
      if (!JoinCompletedStepWorker(*request))
        return;
      const auto operation = m_controller.BeginStep(Core::Debug::ExecutionOperationKind::StepOver);
      if (!operation)
      {
        RespondError(request->seq, command, operation.error());
        return;
      }
      // DAP stepping requires the core to be paused. Reserve the operation
      // first so a duplicate request cannot pause and disrupt an active
      // continuing step before it is rejected.
      if (!m_system.GetCPU().IsStepping())
        m_controller.PauseWithoutEvent();
      const StepOverResult result = m_controller.StepOver(*operation);
      Respond(request->seq, command, picojson::object{});
      if (result == StepOverResult::Stepped)
      {
        // A breakpoint encountered by the step already completed the shared
        // operation; otherwise this publishes the normal step completion.
        m_controller.CompleteStep(*operation);
      }
      else if (result == StepOverResult::NotStepped)
      {
        m_controller.AbandonStep(*operation);
        // DESNOTE(jbarber, 2026-07-21): The underlying StepInto timed out
        // without the CPU thread acknowledging -- the PC has not advanced.
        // Suppress the stopped event (mirroring the stepIn guard) so the
        // client isn't told execution stopped at an unchanged PC. The
        // response still acks the `next` request so the client knows the
        // command was received.
      }
      else
      {
        m_controller.PublishStepContinued(*operation);
      }
      return;
    }

    if (command == "stepIn")
    {
      ClearDebugValueHandles();
      const auto granularity = Protocol::ParseSteppingGranularity(request->arguments);
      if (!granularity)
      {
        RespondError(request->seq, command, "invalid stepping granularity");
        return;
      }
      if (*granularity != Protocol::SteppingGranularity::Instruction)
      {
        StartSourceStep(*request, false);
        return;
      }
      if (!JoinCompletedStepWorker(*request))
        return;
      // DESNOTE(jbarber, 2026-07-21): StepInto may return false when the
      // CPU thread can't acknowledge the StepOpcode signal within its 2s
      // wait (e.g. the emulator is mid-block or under load). Emitting a
      // `stopped`/`step` event in that case would lie to the client -- the
      // PC hasn't advanced -- so we only send the stop when the step
      // actually completed.
      const auto operation = m_controller.BeginStep(Core::Debug::ExecutionOperationKind::StepInto);
      if (!operation)
      {
        RespondError(request->seq, command, operation.error());
        return;
      }
      if (!m_system.GetCPU().IsStepping())
        m_controller.PauseWithoutEvent();
      const bool completed = m_controller.StepInto();
      Respond(request->seq, command, picojson::object{});
      if (completed)
      {
        // A breakpoint encountered by the step already completed the shared
        // operation; otherwise this publishes the normal step completion.
        m_controller.CompleteStep(*operation);
      }
      else
      {
        m_controller.AbandonStep(*operation);
      }
      return;
    }

    if (command == "stepOut")
    {
      ClearDebugValueHandles();
      if (!JoinCompletedStepWorker(*request))
        return;
      // DESNOTE(jbarber, 2026-07-21): Run StepOut on a worker thread so the
      // session loop keeps polling the socket for disconnect / new requests
      // and keeps flushing realtime-watch events while the interpreter
      // single-steps toward the next return. The worker publishes the exact
      // stop through the shared execution service.
      //
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
      m_step_out_done.store(false);
      m_step_cancelled.store(false);
      const auto operation =
          m_controller.BeginStep(Core::Debug::ExecutionOperationKind::StepOut, &m_step_cancelled);
      if (!operation)
      {
        m_step_out_done.store(true);
        RespondError(request->seq, command, operation.error());
        return;
      }
      if (!m_system.GetCPU().IsStepping())
        m_controller.PauseWithoutEvent();
      Respond(request->seq, command, picojson::object{});
      m_controller.PublishStepContinued(*operation);
      m_step_out_thread = std::thread([self = shared_from_this(), operation = *operation]() {
        const Core::Debug::PPCStepResult result =
            self->m_controller.StepOut(self->m_step_cancelled);
        self->m_step_out_done.store(true);
        if (self->m_step_cancelled.load())
          self->m_controller.AbandonStep(operation);
        else if (result == Core::Debug::PPCStepResult::Stepped)
          self->m_controller.CompleteStep(operation);
        else if (result == Core::Debug::PPCStepResult::Continuing)
          self->m_controller.PublishStepContinued(operation);
        else
          self->m_controller.AbandonStep(operation);
      });
      // The worker publishes the stopped event when it completes.
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
      const int frame_id = ReadNumericFromJson<int>(request->arguments, "frameId").value_or(-1);
      Respond(request->seq, command, MakeScopes(frame_id));
      return;
    }

    if (command == "variables")
    {
      const std::optional<int> variables_reference =
          ReadNumericFromJson<int>(request->arguments, "variablesReference");
      if (variables_reference && *variables_reference >= 0x10000 &&
          !m_debug_value_handles.contains(*variables_reference))
      {
        RespondError(request->seq, command, "unknown or stale variablesReference");
        return;
      }
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

    if (command == "dolphin_memoryRegions")
    {
      HandleMemoryRegions(*request);
      return;
    }

    if (command == "dolphin_memoryScanStart")
    {
      HandleMemoryScanStart(*request);
      return;
    }

    if (command == "dolphin_memoryScanRefine")
    {
      HandleMemoryScanRefine(*request);
      return;
    }

    if (command == "dolphin_memoryScanStatus")
    {
      HandleMemoryScanStatus(*request);
      return;
    }

    if (command == "dolphin_memoryScanResults")
    {
      HandleMemoryScanResults(*request);
      return;
    }

    if (command == "dolphin_memoryScanCancel")
    {
      HandleMemoryScanCancel(*request);
      return;
    }

    if (command == "dolphin_memoryScanDispose")
    {
      HandleMemoryScanDispose(*request);
      return;
    }

    if (command == "dolphin_memoryScanUndo")
    {
      HandleMemoryScanUndo(*request);
      return;
    }

    if (command == "dolphin_memoryScanRemoveResults")
    {
      HandleMemoryScanRemoveResults(*request);
      return;
    }

    if (command == "dolphin_resolvePointerChain")
    {
      HandleResolvePointerChain(*request);
      return;
    }

    WARN_LOG_FMT(CONSOLE, "DAP: unhandled command {}", command);
    RespondError(request->seq, command, "unsupported");
  }

  void HandleMemoryRegions(const Protocol::Request& request)
  {
    picojson::array regions;
    for (const MemoryRegionInfo& region : m_memory_engine->GetMemoryRegions())
    {
      picojson::object entry;
      entry.emplace("id", region.id);
      entry.emplace("name", region.name);
      entry.emplace("baseAddress", Json::FormatAddress(region.base_address));
      entry.emplace("size", static_cast<double>(region.size));
      entry.emplace("readable", true);
      entry.emplace("writable", true);
      entry.emplace("scannable", true);
      regions.emplace_back(std::move(entry));
    }
    picojson::object body;
    body.emplace("platform", std::string(m_system.IsWii() ? "wii" : "gamecube"));
    body.emplace("pointerSize", 4.0);
    body.emplace("byteOrder", std::string("big"));
    body.emplace("regions", std::move(regions));
    Respond(request.seq, request.command, std::move(body));
  }

  void HandleResolvePointerChain(const Protocol::Request& request)
  {
    const auto arguments = Protocol::ParseResolvePointerChain(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, request.command, "invalid pointer chain arguments");
      return;
    }
    const auto resolved =
        m_controller.ResolvePointerChain(arguments->base_address, arguments->offsets);
    if (!resolved)
    {
      RespondError(request.seq, request.command, resolved.error());
      return;
    }

    picojson::array steps;
    for (const PointerChainStep& step : resolved->steps)
    {
      picojson::object entry;
      entry.emplace("address", Json::FormatAddress(step.address));
      entry.emplace("pointerValue", Json::FormatAddress(step.pointer_value));
      entry.emplace("offset", static_cast<double>(step.offset));
      entry.emplace("resultAddress", Json::FormatAddress(step.result_address));
      steps.emplace_back(std::move(entry));
    }
    picojson::object body;
    body.emplace("finalAddress", Json::FormatAddress(resolved->final_address));
    body.emplace("steps", std::move(steps));
    Respond(request.seq, request.command, std::move(body));
  }

  void HandleMemoryScanStart(const Protocol::Request& request)
  {
    const std::optional<MemoryScanStartConfig> arguments =
        Protocol::ParseMemoryScanStart(request.arguments);
    if (!arguments)
    {
      FlushEvents();
      RespondError(request.seq, request.command, "invalid memory scan arguments");
      return;
    }
    const auto accepted = m_memory_engine->StartScan(*arguments);
    if (!accepted)
    {
      FlushEvents();
      RespondError(request.seq, request.command, accepted.error());
      return;
    }
    picojson::object body;
    body.emplace("scanId", static_cast<double>(accepted->scan_id));
    body.emplace("jobId", static_cast<double>(accepted->job_id));
    body.emplace("state", std::string("running"));
    body.emplace("pauseDuringScan", accepted->pause_during_scan);
    FlushEventsThroughPriorMemoryScanTerminal(accepted->job_id);
    Respond(request.seq, request.command, std::move(body));
  }

  void HandleMemoryScanRefine(const Protocol::Request& request)
  {
    const std::optional<MemoryScanRefineConfig> arguments =
        Protocol::ParseMemoryScanRefine(request.arguments);
    if (!arguments)
    {
      FlushEvents();
      RespondError(request.seq, request.command, "invalid memory scan refinement arguments");
      return;
    }
    const auto accepted = m_memory_engine->RefineScan(*arguments);
    if (!accepted)
    {
      FlushEvents();
      RespondError(request.seq, request.command, accepted.error());
      return;
    }
    picojson::object body;
    body.emplace("scanId", static_cast<double>(accepted->scan_id));
    body.emplace("jobId", static_cast<double>(accepted->job_id));
    body.emplace("state", std::string("running"));
    body.emplace("pauseDuringScan", accepted->pause_during_scan);
    FlushEventsThroughPriorMemoryScanTerminal(accepted->job_id);
    Respond(request.seq, request.command, std::move(body));
  }

  void HandleMemoryScanStatus(const Protocol::Request& request)
  {
    const auto arguments = Protocol::ParseMemoryScanStatus(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, request.command, "invalid memory scan status arguments");
      return;
    }
    const std::optional<MemoryScanStatus> status = m_memory_engine->GetStatus(arguments->scan_id);
    if (!status)
    {
      RespondError(request.seq, request.command, "no such memory scan");
      return;
    }
    picojson::object body;
    body.emplace("scanId", static_cast<double>(status->scan_id));
    body.emplace("jobId", static_cast<double>(status->job_id));
    body.emplace("generation", static_cast<double>(status->generation));
    body.emplace("state", status->state);
    body.emplace("phase", status->phase);
    body.emplace("pauseDuringScan", status->pause_during_scan);
    body.emplace("emulationPaused", status->emulation_paused);
    body.emplace("resultCount", static_cast<double>(status->result_count));
    Respond(request.seq, request.command, std::move(body));
  }

  void HandleMemoryScanResults(const Protocol::Request& request)
  {
    const auto arguments = Protocol::ParseMemoryScanResults(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, request.command, "invalid memory scan result arguments");
      return;
    }
    const auto page =
        m_memory_engine->GetResults(arguments->scan_id, arguments->start, arguments->count);
    if (!page)
    {
      RespondError(request.seq, request.command, page.error());
      return;
    }
    picojson::array results;
    for (const MemoryScanResult& result : page->results)
    {
      picojson::object entry;
      entry.emplace("address", Json::FormatAddress(result.address));
      entry.emplace("scannedValue", result.scanned_value);
      entry.emplace("raw", Json::Base64Encode(result.raw));
      if (result.disassembly)
        entry.emplace("disassembly", *result.disassembly);
      results.emplace_back(std::move(entry));
    }
    picojson::object body;
    body.emplace("scanId", static_cast<double>(page->scan_id));
    body.emplace("generation", static_cast<double>(page->generation));
    body.emplace("totalResults", static_cast<double>(page->total_results));
    body.emplace("start", static_cast<double>(page->start));
    body.emplace("results", std::move(results));
    Respond(request.seq, request.command, std::move(body));
  }

  void HandleMemoryScanCancel(const Protocol::Request& request)
  {
    const auto arguments = Protocol::ParseMemoryScanStatus(request.arguments);
    if (!arguments || !m_memory_engine->Cancel(arguments->scan_id))
    {
      RespondError(request.seq, request.command, "no active job for that memory scan");
      return;
    }
    Respond(request.seq, request.command, picojson::object{});
  }

  void HandleMemoryScanDispose(const Protocol::Request& request)
  {
    const auto arguments = Protocol::ParseMemoryScanStatus(request.arguments);
    if (!arguments || !m_memory_engine->Dispose(arguments->scan_id))
    {
      RespondError(request.seq, request.command, "no such memory scan");
      return;
    }
    Respond(request.seq, request.command, picojson::object{});
  }

  void RespondMemoryScanMutation(const Protocol::Request& request,
                                 const MemoryScanMutationResult& result)
  {
    FlushEvents();
    picojson::object body;
    body.emplace("scanId", static_cast<double>(result.scan_id));
    body.emplace("generation", static_cast<double>(result.generation));
    body.emplace("resultCount", static_cast<double>(result.result_count));
    body.emplace("removedCount", static_cast<double>(result.removed_count));
    body.emplace("canUndo", result.can_undo);
    Respond(request.seq, request.command, std::move(body));
  }

  void HandleMemoryScanUndo(const Protocol::Request& request)
  {
    const auto arguments = Protocol::ParseMemoryScanStatus(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, request.command, "invalid memory scan undo arguments");
      return;
    }
    const auto result = m_memory_engine->Undo(arguments->scan_id);
    if (!result)
    {
      RespondError(request.seq, request.command, result.error());
      return;
    }
    RespondMemoryScanMutation(request, *result);
  }

  void HandleMemoryScanRemoveResults(const Protocol::Request& request)
  {
    const auto arguments = Protocol::ParseMemoryScanRemoveResults(request.arguments);
    if (!arguments)
    {
      RespondError(request.seq, request.command, "invalid memory scan result-removal arguments");
      return;
    }
    const auto result = m_memory_engine->RemoveResults(arguments->scan_id, arguments->addresses);
    if (!result)
    {
      RespondError(request.seq, request.command, result.error());
      return;
    }
    RespondMemoryScanMutation(request, *result);
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
      const Protocol::SetBreakpointsArguments legacy =
          Protocol::ParseSetBreakpoints(request.arguments);
      if (legacy.base)
      {
        SourceBreakpointSpec spec;
        spec.line = 0;
        specs.push_back(std::move(spec));
      }
    }

    std::string error;
    const auto addresses = m_controller.UpdateSourceBreakpoints(source_key, context, specs, &error);
    if (!error.empty())
    {
      RespondError(request.seq, "setBreakpoints", error);
      return;
    }

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
    std::vector<std::optional<u32>> requested_addresses;
    requested_addresses.reserve(arguments.breakpoints.size());
    for (const Protocol::RequestedInstructionBreakpoint& breakpoint : arguments.breakpoints)
    {
      requested_addresses.push_back(breakpoint.address);
      if (breakpoint.address)
      {
        CodeBreakpointRequest bp;
        bp.address = *breakpoint.address;
        bp.condition = breakpoint.condition;
        breakpoint_requests.push_back(std::move(bp));
      }
    }

    std::string error;
    if (!m_controller.UpdateInstructionBreakpoints(std::move(breakpoint_requests), &error))
    {
      RespondError(request.seq, "setInstructionBreakpoints", error);
      return;
    }

    picojson::array breakpoints;
    for (const std::optional<u32> address : requested_addresses)
    {
      picojson::object entry;
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
      target.emplace("line", static_cast<double>(arguments.line));
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

    if (!CancelAndJoinStepWorker(request))
      return;
    DiscardQueuedStoppedEvents();
    ClearDebugValueHandles();
    const auto result = m_controller.Goto(arguments->target);
    if (!result)
    {
      RespondError(request.seq, "goto", result.error());
      return;
    }
    Respond(request.seq, "goto", picojson::object{});
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
      if (source.source_reference > 0)
        entry.emplace("sourceReference", static_cast<double>(source.source_reference));
      if (source.source_id)
      {
        picojson::object adapter_data;
        adapter_data.emplace("dolphinSourceId", static_cast<double>(*source.source_id));
        entry.emplace("adapterData", std::move(adapter_data));
      }
      entry.emplace("name", source.name);
      if (!source.path.empty())
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
    if (!arguments.source_reference)
    {
      RespondError(request.seq, "source", "invalid source arguments");
      return;
    }

    const std::optional<SourceContent> content = m_controller.GetSource(
        *arguments.source_reference, arguments.start_line, arguments.end_line);
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
    if (!arguments.source_reference)
    {
      RespondError(request.seq, "breakpointLocations", "invalid breakpointLocations arguments");
      return;
    }

    picojson::array breakpoints;
    for (const BreakpointLocation& location : m_controller.GetBreakpointLocations(
             *arguments.source_reference, arguments.start_line, arguments.end_line))
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
    if (!CancelAndJoinStepWorker(request))
      return;
    DiscardQueuedStoppedEvents();
    ClearDebugValueHandles();
    const auto result = m_controller.Terminate();
    if (!result)
    {
      RespondError(request.seq, "terminate", result.error());
      return;
    }
    Respond(request.seq, "terminate", picojson::object{});

    picojson::object body;
    body.emplace("restart", false);
    QueueEvent("terminated", std::move(body));
    FlushEvents();
  }

  void HandleRestart(const Protocol::Request& request)
  {
    if (!CancelAndJoinStepWorker(request))
      return;
    DiscardQueuedStoppedEvents();
    ClearDebugValueHandles();
    const auto result = m_controller.Restart();
    if (!result)
    {
      RespondError(request.seq, "restart", result.error());
      return;
    }
    Respond(request.seq, "restart", picojson::object{});
  }

  void HandleSetDataBreakpoints(const Protocol::Request& request)
  {
    const Protocol::SetDataBreakpointsArguments arguments =
        Protocol::ParseSetDataBreakpoints(request.arguments);

    std::vector<DataBreakpointRequest> breakpoint_requests;
    for (const Protocol::RequestedDataBreakpoint& breakpoint : arguments.breakpoints)
    {
      if (breakpoint.address)
      {
        DataBreakpointRequest bp;
        bp.address = *breakpoint.address;
        bp.length = breakpoint.length;
        bp.read = breakpoint.read;
        bp.write = breakpoint.write;
        bp.condition = breakpoint.condition;
        breakpoint_requests.push_back(std::move(bp));
      }
    }

    const auto result = m_controller.SetDataBreakpoints(std::move(breakpoint_requests));
    if (!result)
    {
      RespondError(request.seq, "setDataBreakpoints", result.error());
      return;
    }

    picojson::array breakpoints;
    for (const Protocol::RequestedDataBreakpoint& breakpoint : arguments.breakpoints)
    {
      picojson::object entry;
      entry.emplace("verified", breakpoint.address.has_value());
      if (breakpoint.address)
      {
        const u32 length = breakpoint.length == 0 ? 1 : breakpoint.length;
        const u32 end = length - 1 > std::numeric_limits<u32>::max() - *breakpoint.address ?
                            std::numeric_limits<u32>::max() :
                            *breakpoint.address + length - 1;
        entry.emplace("id", static_cast<double>(AssignDataBreakpointId(*breakpoint.address, end)));
        entry.emplace("instructionReference", Json::FormatAddress(*breakpoint.address));
      }
      breakpoints.emplace_back(std::move(entry));
    }
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

    ClearDebugValueHandles();
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
        if (frame.source_id)
        {
          picojson::object adapter_data;
          adapter_data.emplace("dolphinSourceId", static_cast<double>(*frame.source_id));
          source.emplace("adapterData", std::move(adapter_data));
        }
        entry.emplace("source", std::move(source));
        entry.emplace("line", static_cast<double>(frame.source_line));
      }
      else if (frame.source_base)
      {
        picojson::object source;
        source.emplace("name", Json::FormatAddress(*frame.source_base));
        source.emplace("sourceReference",
                       static_cast<double>(MakeDisassemblySourceReference(*frame.source_base)));
        entry.emplace("source", std::move(source));
        entry.emplace("line", static_cast<double>(frame.source_line));
      }
      else
      {
        entry.emplace("line", static_cast<double>(frame.source_line));
      }
      entry.emplace("column", 1.0);
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
    // DESNOTE(jbarber, 2026-07-22): If `count` was capped below the client's
    // requested count, surface the difference as `unreadableBytes` so the
    // client knows the response was truncated (rather than silently getting
    // a smaller payload). Mirrors how a partially-unreadable region is
    // reported. Bugbot #65.
    if (arguments->requested_count > arguments->count)
    {
      const u32 capped = arguments->requested_count - arguments->count;
      // Add the cap shortfall to any prior unreadable tail so the client
      // sees a single honest count of bytes it didn't get back.
      const u32 total_unreadable = unreadable + capped;
      body["unreadableBytes"] = picojson::value(static_cast<double>(total_unreadable));
    }
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
    const s64 effective =
        static_cast<s64>(*base) + static_cast<s64>(arguments->instruction_offset) * 4;
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
    // DESNOTE(jbarber, 2026-07-26): If `count` was capped, surface the
    // original request size so the client knows it's watching fewer bytes
    // than it asked for (mirrors readMemory's requested_count/unreadableBytes
    // pattern). Bugbot #79.
    if (arguments->requested_count > arguments->count)
      body.emplace("requestedCount", static_cast<double>(arguments->requested_count));
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

    // DESNOTE(jbarber, 2026-07-26): Remove the MMU-level freeze memcheck
    // before cancelling the subscription. Without this, cancelling a frozen
    // watch leaves the freeze memcheck active — CPU writes to the range stay
    // suppressed with no owning watch to manage or unfreeze it. Bugbot #74.
    auto freeze_it = m_watch_to_freeze.find(arguments->watch_id);
    if (freeze_it != m_watch_to_freeze.end())
    {
      m_controller.RemoveFreeze(freeze_it->second);
      m_watch_to_freeze.erase(freeze_it);
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
    // DESNOTE(jbarber, 2026-07-22): `dolphin_freeze` now installs an MMU-level
    // write suppression (via a private MemChecks freeze range) in addition to the
    // existing field-rate Tick fallback. CPU stores to the frozen range are
    // silently dropped at the MMU layer — the game never sees its own
    // writes. The field-rate Tick remains as a fallback for DMA/peripheral
    // writes that bypass MMU::Write. Two forms are accepted (see ParseFreeze):
    // a standalone `memoryReference + count + data` that creates a new frozen
    // subscription, or `watchId + data` that freezes an existing watch.
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
      if (!m_watch_sampler->Freeze(watch_id, arguments->value))
      {
        RespondError(request.seq, "dolphin_freeze", "no such watch_id or value size mismatch");
        return;
      }
      // Get the subscription's address/count to install the MMU memcheck.
      auto info = m_watch_sampler->GetSubscriptionInfo(watch_id);
      if (!info)
      {
        // Shouldn't happen — Freeze just succeeded on this watch_id.
        m_watch_sampler->Unfreeze(watch_id);
        RespondError(request.seq, "dolphin_freeze", "internal error: subscription vanished");
        return;
      }
      address = info->address;
      count = info->count;
    }
    else
    {
      // Form 1: standalone freeze. Create the subscription and freeze it.
      address = *arguments->address;
      count = *arguments->count;
      watch_id = m_watch_sampler->AddSubscription(address, count);
      if (watch_id == DAP::kInvalidWatchId)
      {
        RespondError(request.seq, "dolphin_freeze", "rejected address/count (overflow?)");
        return;
      }
      if (!m_watch_sampler->Freeze(watch_id, arguments->value))
      {
        m_watch_sampler->RemoveSubscription(watch_id);
        RespondError(request.seq, "dolphin_freeze", "internal error: freeze-after-add failed");
        return;
      }
    }

    // DESNOTE(jbarber, 2026-07-26): If this watch was already frozen, remove
    // the old freeze memcheck before installing the new one. Without this,
    // re-freezing leaks a memcheck entry in the global store — the old
    // freeze_id is overwritten in m_watch_to_freeze and can never be torn
    // down. Bugbot #76.
    {
      auto existing = m_watch_to_freeze.find(watch_id);
      if (existing != m_watch_to_freeze.end())
      {
        m_controller.RemoveFreeze(existing->second);
        m_watch_to_freeze.erase(existing);
      }
    }

    // Install the MMU-level private freeze range so CPU writes to
    // [address, address+count) are silently suppressed. The frozen value
    // is written to RAM by InstallFreeze (via HostWrite, which bypasses
    // the freeze memcheck). Bugbot-safe: Freeze already wrote the canon
    // to RAM (in RealtimeWatchSampler::Freeze), and InstallFreeze writes
    // it again via HostWrite — double-write is harmless.
    const u32 freeze_id = m_controller.InstallFreeze(address, count, arguments->value);
    if (freeze_id == 0)
    {
      // MMU memcheck install failed — roll back the sampler freeze so
      // the client gets a truthful error instead of a freeze that only
      // has field-rate protection (no write suppression).
      m_watch_sampler->Unfreeze(watch_id);
      if (!arguments->watch_id)
        m_watch_sampler->RemoveSubscription(watch_id);
      RespondError(request.seq, "dolphin_freeze", "failed to install MMU freeze memcheck");
      return;
    }
    m_watch_to_freeze[watch_id] = freeze_id;

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

    // DESNOTE(jbarber, 2026-07-22): Also tear down the MMU-level private freeze
    // memcheck so CPU writes to the formerly-frozen range are no longer
    // suppressed. The field-rate Tick (which was the DMA fallback) also
    // stops restoring the value (Unfreeze cleared `frozen_value`).
    auto it = m_watch_to_freeze.find(arguments->watch_id);
    if (it != m_watch_to_freeze.end())
    {
      m_controller.RemoveFreeze(it->second);
      m_watch_to_freeze.erase(it);
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
    // DESNOTE(jbarber, 2026-07-22): When no detour address is supplied,
    // allocate ONCE here and pass it down to Detour, mirroring
    // HandleInjectCode's pre-allocation. The previous form let Detour scan
    // FindFreeMemory internally, which is functionally equivalent (one scan
    // either way) but asymmetric with the inject handler and made the
    // "could rescan between calls" complaint surface. Detour now requires
    // detour_address when invoked; the scan happens here, before any
    // WriteMemory, so the response is truthful about feasibility. Bugbot #68.
    // The size requested is body + 12 (tail branch + 8-byte trampoline), not
    // just body.size() -- see Detour for the layout.
    std::optional<u32> resolved_address = arguments->detour_address;
    if (!resolved_address)
    {
      // detour_body is validated % 4 == 0 by ParseDetour; body_size is safe.
      const u32 detour_size =
          static_cast<u32>(arguments->detour_body.size()) + 12u;  // body + tail + trampoline
      auto alloc = m_controller.FindFreeMemory(detour_size);
      if (!alloc)
      {
        RespondError(request.seq, "dolphin_detour", "no free region of that size");
        return;
      }
      resolved_address = *alloc;
    }
    auto result =
        m_controller.Detour(arguments->target_address, resolved_address, arguments->detour_body);
    if (!result)
    {
      RespondError(request.seq, "dolphin_detour",
                   "detour failed (invalid target or out-of-range branch?)");
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

  void ClearDebugValueHandles() { m_debug_value_handles.clear(); }

  picojson::object MakeScopes(const int frame_id)
  {
    ClearDebugValueHandles();
    picojson::array scopes;
    scopes.emplace_back(MakeScope("Registers", REGISTERS_SCOPE));
    scopes.emplace_back(MakeScope("PC", PC_SCOPE));
    const auto debug_info = m_system.GetPPCSymbolDB().GetDwarfDebugInfo();
    if (frame_id == 0 && debug_info && !debug_info->variables.empty())
    {
      scopes.emplace_back(MakeScope("Locals", LOCALS_SCOPE));
      scopes.emplace_back(MakeScope("Globals", GLOBALS_SCOPE));
    }

    picojson::object body;
    body.emplace("scopes", std::move(scopes));
    return body;
  }

  picojson::object MakeDebugVariable(DebugVariable variable)
  {
    picojson::object result;
    result.emplace("name", std::move(variable.name));
    result.emplace("value", std::move(variable.value));
    result.emplace("type", std::move(variable.type));
    int reference = 0;
    if (variable.children && m_debug_value_handles.size() < 100000 &&
        m_next_debug_value_handle < std::numeric_limits<int>::max())
    {
      reference = m_next_debug_value_handle++;
      m_debug_value_handles.emplace(reference, std::move(*variable.children));
    }
    result.emplace("variablesReference", static_cast<double>(reference));
    return result;
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
    else if (variables_reference == LOCALS_SCOPE || variables_reference == GLOBALS_SCOPE)
    {
      for (DebugVariable& variable :
           m_controller.GetDebugVariables(variables_reference == GLOBALS_SCOPE))
        variables.emplace_back(MakeDebugVariable(std::move(variable)));
    }
    else if (const auto it = m_debug_value_handles.find(variables_reference);
             it != m_debug_value_handles.end())
    {
      for (DebugVariable& variable : m_controller.GetDebugVariableChildren(it->second))
        variables.emplace_back(MakeDebugVariable(std::move(variable)));
    }

    picojson::object body;
    body.emplace("variables", std::move(variables));
    return body;
  }

  DapTransport& m_transport;
  DapDebugController m_controller;
  Core::System& m_system;
  std::unique_ptr<RealtimeWatchSampler> m_watch_sampler;
  std::unique_ptr<DapMemoryEngine> m_memory_engine;
  // DESNOTE(jbarber, 2026-07-22): Maps watch_id → freeze_id returned by
  // DapDebugController::InstallFreeze, so HandleUnfreeze can call
  // RemoveFreeze to tear down the MMU-level private freeze range that
  // suppresses CPU writes to the frozen range.
  std::map<int, u32> m_watch_to_freeze;
  std::map<int, DebugValueContext> m_debug_value_handles;
  int m_next_debug_value_handle = 0x10000;
  std::atomic<bool> m_running{true};
  std::atomic<bool> m_invalidate_debug_values{false};
  // Sequence numbers are handed out from both the session loop (responses) and
  // the core state-changed callback (the "continued" event), which may run on
  // the CPU thread; keep allocation race-free.
  std::atomic<int> m_next_seq{1};
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

  std::mutex m_event_mutex;
  std::vector<std::pair<std::string, picojson::object>> m_pending_events;
  bool m_accept_events = true;

  // DESNOTE(jbarber, 2026-07-21): DAP `hitBreakpointIds` in a stopped event
  // must echo the stable `id` the client received in its
  // setBreakpoints/setInstructionBreakpoints response, NOT the raw PC where
  // the breakpoint hit (clients correlate via the id and would mis-associate
  // a hit if we shipped the address instead). We assign a monotonic id per
  // installed breakpoint site and remember the (address -> id) mapping so
  // HandleExecutionEvent can translate the hit PC back to an id.
  std::mutex m_bp_id_mutex;
  std::unordered_map<u32, int> m_bp_id_by_address;
  std::map<std::pair<u32, u32>, int> m_data_bp_id_by_range;
  int m_next_bp_id = 1;

  // DESNOTE(jbarber, 2026-07-21): Asynchronous step-out worker. The previous
  // form ran DapDebugController::StepOut inline on the session thread, which
  // holds a CPUThreadGuard for up to its full 5s timeout while single-stepping
  // the interpreter. That blocked `HandleMessage` from polling the socket, so
  // disconnect, new requests, and realtime-watch event dispatch stalled until
  // the step completed. We now run StepOut on a worker thread so the session
  // loop keeps draining its 50ms `WaitForReadable` poll, and
  // PollAsyncStepAndEvents joins the worker after it publishes its stop.
  // The worker captures a shared_ptr<Session> (rather than `this`) so a late
  // tear-down can't free the Session while the worker still holds the guard.
  std::thread m_step_out_thread;
  std::atomic<bool> m_step_out_done{true};
  std::atomic<bool> m_step_cancelled{false};
  const SessionTestHooks* m_test_hooks;
};
}  // namespace

void RunSession(DapTransport& transport, Core::System& system, const SessionTestHooks* test_hooks)
{
  // DESNOTE(jbarber, 2026-07-21): Session derives from enable_shared_from_this
  // so the realtime-watch sampler's dispatch lambda can capture a weak_ptr and
  // keep *this alive across an in-flight vi_end_field_event Tick() on the CPU
  // thread. Constructing here via make_shared (rather than a stack local)
  // makes weak_from_this() valid by the time Run() is entered.
  std::make_shared<Session>(transport, system, test_hooks)->Run();
}
}  // namespace DAP

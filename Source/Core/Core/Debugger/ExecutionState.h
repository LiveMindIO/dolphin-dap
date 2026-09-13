// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "Common/CommonTypes.h"

namespace Core::Debug
{
enum class ExecutionEventKind
{
  Continued,
  Stopped,
};

enum class ExecutionStopCause
{
  Unknown,
  UserPause,
  Step,
  CodeBreakpoint,
  DataBreakpoint,
  Exception,
  Goto,
  Entry,
  Restart,
  Terminate,
};

enum class ExecutionOperationKind
{
  Continue,
  Pause,
  StepInto,
  StepOver,
  StepOut,
  SourceStepInto,
  SourceStepOver,
  Goto,
  Entry,
  Restart,
  Terminate,
};

enum class DataAccessType
{
  Read,
  Write,
};

struct ExecutionEvent
{
  u64 revision = 0;
  u64 stop_generation = 0;
  ExecutionEventKind kind = ExecutionEventKind::Stopped;
  std::optional<ExecutionStopCause> stop_cause;
  std::optional<u64> origin;
  std::optional<u64> operation_id;
  std::optional<u32> pc;
  std::optional<u32> code_breakpoint_address;
  std::optional<u32> data_address;
  std::optional<u32> data_size;
  std::optional<u32> watchpoint_start;
  std::optional<u32> watchpoint_end;
  std::optional<DataAccessType> data_access;
  std::optional<u32> exceptions;
};

struct ExecutionStopDetails
{
  ExecutionStopCause cause = ExecutionStopCause::Step;
  std::optional<u64> origin;
  std::optional<u64> operation_id;
  std::optional<u32> pc;
  std::optional<u32> code_breakpoint_address;
  std::optional<u32> data_address;
  std::optional<u32> data_size;
  std::optional<u32> watchpoint_start;
  std::optional<u32> watchpoint_end;
  std::optional<DataAccessType> data_access;
  std::optional<u32> exceptions;
};

class ExecutionState final
{
public:
  using ClientId = u64;
  using OperationId = u64;
  using EventCallback = std::function<void(std::shared_ptr<const ExecutionEvent>)>;

  ClientId RegisterClient(EventCallback callback = {});
  void SetClientEventCallback(ClientId client_id, EventCallback callback);
  void UnregisterClient(ClientId client_id);

  std::expected<OperationId, std::string> BeginOperation(ClientId origin,
                                                         ExecutionOperationKind kind,
                                                         std::atomic<bool>* cancellation = nullptr);
  std::expected<void, std::string> CancelActiveStep(ClientId requester);
  std::expected<void, std::string> CancelActiveStepAndWait(ClientId requester);
  std::expected<void, std::string> PublishContinued(ClientId origin, OperationId operation_id,
                                                    std::optional<u32> pc = {});
  void PublishStopped(ExecutionStopDetails details);
  void AbandonStep(OperationId operation_id);
  void MarkStepWorkerComplete(OperationId operation_id);
  bool SetActiveStepCleanup(OperationId operation_id, std::function<void()> cleanup);
  bool IsOperationActive(OperationId operation_id) const;
  std::optional<ClientId> GetActiveStepOrigin() const;

  u64 GetRevision() const;
  u64 GetStopGeneration() const;

private:
  struct Client
  {
    EventCallback callback;
  };

  struct ActiveStep
  {
    ClientId origin = 0;
    OperationId operation_id = 0;
    std::atomic<bool>* cancellation = nullptr;
    std::function<void()> cleanup;
  };

  static bool IsStep(ExecutionOperationKind kind);
  void QueueEventLocked(std::shared_ptr<const ExecutionEvent> event);
  void DrainDispatchQueue();
  void SynchronizeDispatch();

  mutable std::mutex m_mutex;
  std::condition_variable m_operation_changed;
  std::recursive_mutex m_dispatch_mutex;
  std::unordered_map<ClientId, Client> m_clients;
  std::deque<std::shared_ptr<const ExecutionEvent>> m_pending_dispatches;
  std::optional<ActiveStep> m_active_step;
  std::optional<ActiveStep> m_pending_operation;
  std::optional<ClientId> m_running_origin;
  std::optional<OperationId> m_running_operation;
  bool m_is_draining = false;
  ClientId m_next_client_id = 1;
  OperationId m_next_operation_id = 1;
  u64 m_revision = 0;
  u64 m_stop_generation = 0;
};
}  // namespace Core::Debug

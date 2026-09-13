// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/ExecutionState.h"

#include <utility>
#include <vector>

namespace Core::Debug
{
ExecutionState::ClientId ExecutionState::RegisterClient(EventCallback callback)
{
  std::lock_guard lock(m_mutex);
  const ClientId id = m_next_client_id++;
  m_clients.emplace(id, Client{std::move(callback)});
  return id;
}

void ExecutionState::SetClientEventCallback(const ClientId client_id, EventCallback callback)
{
  std::lock_guard lock(m_mutex);
  if (const auto client = m_clients.find(client_id); client != m_clients.end())
    client->second.callback = std::move(callback);
}

void ExecutionState::UnregisterClient(const ClientId client_id)
{
  {
    std::lock_guard lock(m_mutex);
    m_clients.erase(client_id);
    if (m_active_step && m_active_step->origin == client_id && m_active_step->cancellation)
      m_active_step->cancellation->store(true);
    if (m_active_step && m_active_step->origin == client_id)
      m_active_step.reset();
    if (m_pending_operation && m_pending_operation->origin == client_id)
      m_pending_operation.reset();
    if (m_running_origin == client_id)
    {
      m_running_origin.reset();
      m_running_operation.reset();
    }
  }
  SynchronizeDispatch();
}

bool ExecutionState::IsStep(const ExecutionOperationKind kind)
{
  return kind == ExecutionOperationKind::StepInto || kind == ExecutionOperationKind::StepOver ||
         kind == ExecutionOperationKind::StepOut ||
         kind == ExecutionOperationKind::SourceStepInto ||
         kind == ExecutionOperationKind::SourceStepOver;
}

std::expected<ExecutionState::OperationId, std::string>
ExecutionState::BeginOperation(const ClientId origin, const ExecutionOperationKind kind,
                               std::atomic<bool>* const cancellation)
{
  std::lock_guard lock(m_mutex);
  if (!m_clients.contains(origin))
    return std::unexpected("execution client is not registered");
  if (m_active_step)
    return std::unexpected("step already in progress");
  if (m_pending_operation)
    return std::unexpected("execution operation already in progress");

  const OperationId id = m_next_operation_id++;
  if (IsStep(kind))
    m_active_step = ActiveStep{origin, id, cancellation};
  else
    m_pending_operation = ActiveStep{origin, id, nullptr};
  return id;
}

std::expected<void, std::string> ExecutionState::CancelActiveStep(const ClientId requester)
{
  std::lock_guard lock(m_mutex);
  if (!m_clients.contains(requester))
    return std::unexpected("execution client is not registered");
  if (!m_active_step)
    return {};
  if (m_active_step->origin != requester)
    return std::unexpected("step is owned by another debugger client");
  if (m_active_step->cancellation)
    m_active_step->cancellation->store(true);
  else
    m_active_step.reset();
  return {};
}

std::expected<void, std::string> ExecutionState::PublishContinued(const ClientId origin,
                                                                  const OperationId operation_id,
                                                                  const std::optional<u32> pc)
{
  {
    std::lock_guard lock(m_mutex);
    if (!m_clients.contains(origin))
      return std::unexpected("execution client is not registered");
    const bool active_step_matches = m_active_step && m_active_step->origin == origin &&
                                     m_active_step->operation_id == operation_id;
    const bool pending_operation_matches = m_pending_operation &&
                                           m_pending_operation->origin == origin &&
                                           m_pending_operation->operation_id == operation_id;
    if (!active_step_matches && !pending_operation_matches)
      return std::unexpected("continue operation is not active");
    auto event = std::make_shared<ExecutionEvent>();
    event->revision = ++m_revision;
    event->stop_generation = m_stop_generation;
    event->kind = ExecutionEventKind::Continued;
    event->origin = origin;
    event->operation_id = operation_id;
    event->pc = pc;
    m_running_origin = origin;
    m_running_operation = operation_id;
    if (pending_operation_matches)
      m_pending_operation.reset();
    QueueEventLocked(std::move(event));
  }
  DrainDispatchQueue();
  return {};
}

void ExecutionState::PublishStopped(ExecutionStopDetails details)
{
  {
    std::lock_guard lock(m_mutex);
    const bool requires_active_step =
        details.operation_id && (details.cause == ExecutionStopCause::Step ||
                                 details.cause == ExecutionStopCause::CodeBreakpoint);
    if (requires_active_step &&
        (!m_active_step || m_active_step->operation_id != details.operation_id))
    {
      return;
    }
    if (m_active_step &&
        (!details.operation_id || *details.operation_id != m_active_step->operation_id) &&
        m_active_step->cancellation)
    {
      m_active_step->cancellation->store(true);
    }
    if (!details.origin)
      details.origin =
          m_active_step ? std::optional<ClientId>(m_active_step->origin) : m_running_origin;
    if (!details.operation_id)
      details.operation_id = m_active_step ?
                                 std::optional<OperationId>(m_active_step->operation_id) :
                                 m_running_operation;

    auto event = std::make_shared<ExecutionEvent>();
    event->revision = ++m_revision;
    event->stop_generation = ++m_stop_generation;
    event->kind = ExecutionEventKind::Stopped;
    event->stop_cause = details.cause;
    event->origin = details.origin;
    event->operation_id = details.operation_id;
    event->pc = details.pc;
    event->code_breakpoint_address = details.code_breakpoint_address;
    event->data_address = details.data_address;
    event->data_size = details.data_size;
    event->watchpoint_start = details.watchpoint_start;
    event->watchpoint_end = details.watchpoint_end;
    event->data_access = details.data_access;
    event->exceptions = details.exceptions;
    m_active_step.reset();
    if (details.operation_id && m_pending_operation &&
        m_pending_operation->operation_id == details.operation_id)
      m_pending_operation.reset();
    m_running_origin.reset();
    m_running_operation.reset();
    QueueEventLocked(std::move(event));
  }
  DrainDispatchQueue();
}

void ExecutionState::AbandonStep(const OperationId operation_id)
{
  std::lock_guard lock(m_mutex);
  if (m_active_step && m_active_step->operation_id == operation_id)
    m_active_step.reset();
  if (m_pending_operation && m_pending_operation->operation_id == operation_id)
    m_pending_operation.reset();
}

bool ExecutionState::IsOperationActive(const OperationId operation_id) const
{
  std::lock_guard lock(m_mutex);
  return (m_active_step && m_active_step->operation_id == operation_id) ||
         (m_pending_operation && m_pending_operation->operation_id == operation_id);
}

std::optional<ExecutionState::ClientId> ExecutionState::GetActiveStepOrigin() const
{
  std::lock_guard lock(m_mutex);
  if (!m_active_step)
    return std::nullopt;
  return m_active_step->origin;
}

u64 ExecutionState::GetRevision() const
{
  std::lock_guard lock(m_mutex);
  return m_revision;
}

u64 ExecutionState::GetStopGeneration() const
{
  std::lock_guard lock(m_mutex);
  return m_stop_generation;
}

void ExecutionState::QueueEventLocked(std::shared_ptr<const ExecutionEvent> event)
{
  m_pending_dispatches.emplace_back(std::move(event));
}

void ExecutionState::DrainDispatchQueue()
{
  std::lock_guard dispatch_lock(m_dispatch_mutex);
  if (m_is_draining)
    return;
  m_is_draining = true;
  while (true)
  {
    std::shared_ptr<const ExecutionEvent> event;
    std::vector<EventCallback> callbacks;
    {
      std::lock_guard lock(m_mutex);
      if (m_pending_dispatches.empty())
        break;
      event = std::move(m_pending_dispatches.front());
      m_pending_dispatches.pop_front();
      callbacks.reserve(m_clients.size());
      for (const auto& [_, client] : m_clients)
      {
        if (client.callback)
          callbacks.emplace_back(client.callback);
      }
    }
    for (const EventCallback& callback : callbacks)
      callback(event);
  }
  m_is_draining = false;
}

void ExecutionState::SynchronizeDispatch()
{
  std::lock_guard lock(m_dispatch_mutex);
}
}  // namespace Core::Debug

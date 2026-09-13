// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Debugger/ExecutionState.h"

namespace
{
using Core::Debug::DataAccessType;
using Core::Debug::ExecutionEvent;
using Core::Debug::ExecutionEventKind;
using Core::Debug::ExecutionOperationKind;
using Core::Debug::ExecutionState;
using Core::Debug::ExecutionStopCause;

TEST(ExecutionStateTest, PublishesOrderedImmutableEventsAndStopGenerations)
{
  ExecutionState state;
  std::vector<std::shared_ptr<const ExecutionEvent>> events;
  const auto client = state.RegisterClient(
      [&](std::shared_ptr<const ExecutionEvent> event) { events.emplace_back(std::move(event)); });

  const auto operation = state.BeginOperation(client, ExecutionOperationKind::Continue);
  ASSERT_TRUE(operation.has_value());
  ASSERT_TRUE(state.PublishContinued(client, *operation, 0x80001000).has_value());
  state.PublishStopped({.cause = ExecutionStopCause::CodeBreakpoint,
                        .pc = 0x80002000,
                        .code_breakpoint_address = 0x80002000});

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0]->kind, ExecutionEventKind::Continued);
  EXPECT_EQ(events[0]->revision, 1u);
  EXPECT_EQ(events[0]->stop_generation, 0u);
  EXPECT_EQ(events[1]->kind, ExecutionEventKind::Stopped);
  EXPECT_EQ(events[1]->revision, 2u);
  EXPECT_EQ(events[1]->stop_generation, 1u);
  EXPECT_EQ(events[1]->origin, client);
  EXPECT_EQ(events[1]->operation_id, *operation);
  EXPECT_EQ(events[1]->code_breakpoint_address, 0x80002000u);
}

TEST(ExecutionStateTest, DataStopCarriesTheAvailableAccessBoundary)
{
  ExecutionState state;
  std::shared_ptr<const ExecutionEvent> received;
  [[maybe_unused]] const auto client = state.RegisterClient(
      [&](std::shared_ptr<const ExecutionEvent> event) { received = std::move(event); });

  state.PublishStopped({.cause = ExecutionStopCause::DataBreakpoint,
                        .pc = 0x80003000,
                        .data_address = 0x80004002,
                        .data_size = 4,
                        .watchpoint_start = 0x80004000,
                        .watchpoint_end = 0x80004007,
                        .data_access = DataAccessType::Write});

  ASSERT_TRUE(received);
  EXPECT_EQ(received->data_address, 0x80004002u);
  EXPECT_EQ(received->data_size, 4u);
  EXPECT_EQ(received->watchpoint_start, 0x80004000u);
  EXPECT_EQ(received->watchpoint_end, 0x80004007u);
  EXPECT_EQ(received->data_access, DataAccessType::Write);
}

TEST(ExecutionStateTest, RejectsOverlappingStepsDeterministically)
{
  ExecutionState state;
  const auto first_client = state.RegisterClient();
  const auto second_client = state.RegisterClient();
  std::atomic<bool> cancelled{false};

  const auto first =
      state.BeginOperation(first_client, ExecutionOperationKind::SourceStepInto, &cancelled);
  ASSERT_TRUE(first.has_value());
  const auto second = state.BeginOperation(second_client, ExecutionOperationKind::StepOut);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error(), "step already in progress");

  state.PublishStopped(
      {.cause = ExecutionStopCause::Step, .origin = first_client, .operation_id = *first});
  EXPECT_TRUE(state.BeginOperation(second_client, ExecutionOperationKind::StepOut).has_value());
}

TEST(ExecutionStateTest, CallbackCanUnregisterItselfAndRetainEvent)
{
  ExecutionState state;
  ExecutionState::ClientId client = 0;
  std::shared_ptr<const ExecutionEvent> retained;
  client = state.RegisterClient([&](std::shared_ptr<const ExecutionEvent> event) {
    retained = event;
    state.UnregisterClient(client);
  });
  const auto operation = state.BeginOperation(client, ExecutionOperationKind::Pause);
  ASSERT_TRUE(operation.has_value());

  state.PublishStopped(
      {.cause = ExecutionStopCause::UserPause, .origin = client, .operation_id = *operation});
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->revision, 1u);
  state.PublishStopped({.cause = ExecutionStopCause::Exception, .exceptions = 1});
  EXPECT_EQ(retained->revision, 1u);
}

TEST(ExecutionStateTest, UnregisterCancelsAndReleasesOwnedStep)
{
  ExecutionState state;
  std::atomic<bool> cancelled{false};
  bool cleaned_up = false;
  const auto client = state.RegisterClient();
  const auto operation =
      state.BeginOperation(client, ExecutionOperationKind::SourceStepInto, &cancelled);
  ASSERT_TRUE(operation.has_value());
  ASSERT_TRUE(state.SetActiveStepCleanup(*operation, [&] { cleaned_up = true; }));

  state.UnregisterClient(client);

  EXPECT_TRUE(cancelled.load());
  EXPECT_TRUE(cleaned_up);
  EXPECT_FALSE(state.IsOperationActive(*operation));
}

TEST(ExecutionStateTest, ResumeCannotResetAnotherClientsActiveStep)
{
  ExecutionState state;
  std::atomic<bool> cancelled{false};
  const auto stepping_client = state.RegisterClient();
  const auto resuming_client = state.RegisterClient();
  const auto step =
      state.BeginOperation(stepping_client, ExecutionOperationKind::SourceStepInto, &cancelled);
  ASSERT_TRUE(step.has_value());

  const auto resume = state.BeginOperation(resuming_client, ExecutionOperationKind::Continue);
  ASSERT_FALSE(resume.has_value());
  EXPECT_EQ(resume.error(), "step already in progress");
  EXPECT_TRUE(state.IsOperationActive(*step));
  EXPECT_FALSE(cancelled.load());
}

TEST(ExecutionStateTest, OnlyStepOwnerCanRequestCancellation)
{
  ExecutionState state;
  std::atomic<bool> cancelled{false};
  const auto owner = state.RegisterClient();
  const auto other = state.RegisterClient();
  ASSERT_TRUE(
      state.BeginOperation(owner, ExecutionOperationKind::SourceStepInto, &cancelled).has_value());

  const auto rejected = state.CancelActiveStep(other);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error(), "step is owned by another debugger client");
  EXPECT_FALSE(cancelled.load());
  EXPECT_TRUE(state.CancelActiveStep(owner).has_value());
  EXPECT_TRUE(cancelled.load());
}

TEST(ExecutionStateTest, OwnerCancelsWorkerlessContinuingStepImmediately)
{
  ExecutionState state;
  const auto owner = state.RegisterClient();
  const auto operation = state.BeginOperation(owner, ExecutionOperationKind::StepOver);
  ASSERT_TRUE(operation.has_value());

  EXPECT_TRUE(state.CancelActiveStep(owner).has_value());
  EXPECT_FALSE(state.IsOperationActive(*operation));
  EXPECT_TRUE(state.BeginOperation(owner, ExecutionOperationKind::Pause).has_value());
}

TEST(ExecutionStateTest, OwnerWaitsForWorkerBeforeStartingReplacementOperation)
{
  ExecutionState state;
  std::atomic<bool> cancelled{false};
  std::atomic<bool> cancellation_finished{false};
  std::atomic<bool> cancellation_succeeded{false};
  const auto owner = state.RegisterClient();
  const auto operation =
      state.BeginOperation(owner, ExecutionOperationKind::SourceStepInto, &cancelled);
  ASSERT_TRUE(operation.has_value());

  std::jthread canceller([&] {
    cancellation_succeeded.store(state.CancelActiveStepAndWait(owner).has_value());
    cancellation_finished.store(true);
  });
  while (!cancelled.load())
    std::this_thread::yield();
  EXPECT_FALSE(cancellation_finished.load());

  state.MarkStepWorkerComplete(*operation);
  canceller.join();
  EXPECT_TRUE(cancellation_finished.load());
  EXPECT_TRUE(cancellation_succeeded.load());
  EXPECT_FALSE(state.IsOperationActive(*operation));
  EXPECT_TRUE(state.BeginOperation(owner, ExecutionOperationKind::Continue).has_value());
}

TEST(ExecutionStateTest, ExternalStopCancelsActiveStepWorker)
{
  ExecutionState state;
  std::atomic<bool> cancelled{false};
  const auto owner = state.RegisterClient();
  const auto operation =
      state.BeginOperation(owner, ExecutionOperationKind::SourceStepInto, &cancelled);
  ASSERT_TRUE(operation.has_value());

  state.PublishStopped({.cause = ExecutionStopCause::Unknown, .pc = 0x80001000});

  EXPECT_TRUE(cancelled.load());
  EXPECT_FALSE(state.IsOperationActive(*operation));
}
}  // namespace

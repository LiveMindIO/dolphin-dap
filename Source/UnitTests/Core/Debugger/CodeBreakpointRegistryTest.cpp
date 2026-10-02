// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
constexpr u32 GUI_ADDRESS = 0x00003100;
constexpr u32 FIRST_ADDRESS = 0x00003200;
constexpr u32 SECOND_ADDRESS = 0x00003300;

class CodeBreakpointRegistryTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    auto& system = Core::System::GetInstance();
    system.GetMemory().Init();
    AddressSpace::Init();
    system.GetCoreTiming().Init();
    Core::DeclareAsCPUThread();
    system.GetCPU().Init(PowerPC::CPUCore::Interpreter);
    system.GetPowerPC().GetBreakPoints().Clear();
  }

  void TearDown() override
  {
    auto& system = Core::System::GetInstance();
    for (const BreakPoints::ClientId client : m_clients)
      Registry().UnregisterClient(client);
    system.GetPowerPC().GetBreakPoints().Clear();
    system.GetCPU().Shutdown();
    AddressSpace::Shutdown();
    system.GetMemory().Shutdown();
    system.GetCoreTiming().Shutdown();
    Core::UndeclareAsCPUThread();
  }

  static BreakPoints& Registry()
  {
    return Core::System::GetInstance().GetPowerPC().GetBreakPoints();
  }

  BreakPoints::ClientId Register(BreakPoints::EventCallback callback = {})
  {
    const BreakPoints::ClientId client = Registry().RegisterClient(std::move(callback));
    m_clients.push_back(client);
    return client;
  }

  std::vector<BreakPoints::ClientId> m_clients;
};

TEST_F(CodeBreakpointRegistryTest, ClientCollectionsReplaceIndependently)
{
  const auto first = Register();
  const auto second = Register();

  ASSERT_TRUE(
      Registry().ReplaceClientSourceBreakpoints(first, "source:a", {{.address = FIRST_ADDRESS}}));
  ASSERT_TRUE(
      Registry().ReplaceClientInstructionBreakpoints(first, {{.address = FIRST_ADDRESS + 4}}));
  ASSERT_TRUE(
      Registry().ReplaceClientSourceBreakpoints(second, "source:b", {{.address = SECOND_ADDRESS}}));

  ASSERT_TRUE(Registry().ReplaceClientSourceBreakpoints(first, "source:a", {}));
  EXPECT_FALSE(Registry().IsAddressBreakPoint(FIRST_ADDRESS));
  EXPECT_TRUE(Registry().IsAddressBreakPoint(FIRST_ADDRESS + 4));
  EXPECT_TRUE(Registry().IsAddressBreakPoint(SECOND_ADDRESS));

  Registry().UnregisterClient(first);
  EXPECT_FALSE(Registry().IsAddressBreakPoint(FIRST_ADDRESS + 4));
  EXPECT_TRUE(Registry().IsAddressBreakPoint(SECOND_ADDRESS));
  Registry().UnregisterClient(second);
}

TEST_F(CodeBreakpointRegistryTest, DisconnectPreservesOtherClientAndLegacyClaims)
{
  (void)Registry().Add(GUI_ADDRESS);
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(first, {{.address = FIRST_ADDRESS}}));
  ASSERT_TRUE(
      Registry().ReplaceClientInstructionBreakpoints(second, {{.address = SECOND_ADDRESS}}));

  Registry().UnregisterClient(first);
  EXPECT_TRUE(Registry().IsAddressBreakPoint(GUI_ADDRESS));
  EXPECT_FALSE(Registry().IsAddressBreakPoint(FIRST_ADDRESS));
  EXPECT_TRUE(Registry().IsAddressBreakPoint(SECOND_ADDRESS));
  ASSERT_EQ(Registry().GetStrings().size(), 1u);

  Registry().UnregisterClient(second);
  EXPECT_TRUE(Registry().IsAddressBreakPoint(GUI_ADDRESS));
}

TEST_F(CodeBreakpointRegistryTest, IncompatibleConditionActionPoliciesFailAtomically)
{
  const auto first = Register();
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(
      first, {{.address = FIRST_ADDRESS, .condition = "r3 == 1"}}));
  const u64 revision = Registry().GetRevision();
  const auto second = Register();

  const auto conflict =
      Registry().ReplaceClientInstructionBreakpoints(second, {{.address = FIRST_ADDRESS,
                                                               .log_on_hit = true,
                                                               .break_on_hit = false,
                                                               .condition = "r4 == 2"},
                                                              {.address = SECOND_ADDRESS}});
  ASSERT_FALSE(conflict);
  EXPECT_NE(conflict.error().find("incompatible breakpoint break/log condition policies"),
            std::string::npos);
  EXPECT_EQ(Registry().GetRevision(), revision);
  EXPECT_FALSE(Registry().IsAddressBreakPoint(SECOND_ADDRESS));
}

TEST_F(CodeBreakpointRegistryTest, EquivalentCanonicalConditionsCanCombineDifferentActions)
{
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(
      first, {{.address = FIRST_ADDRESS, .condition = "r3 == 1"}}));
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(second, {{.address = FIRST_ADDRESS,
                                                                       .log_on_hit = true,
                                                                       .break_on_hit = false,
                                                                       .condition = " r3==1 "}}));

  const auto effective = Registry().GetRegularBreakpoint(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  ASSERT_TRUE(effective->condition.has_value());
  EXPECT_EQ(effective->condition->GetText(), "r3==1");
  EXPECT_TRUE(effective->break_on_hit);
  EXPECT_TRUE(effective->log_on_hit);
}

TEST_F(CodeBreakpointRegistryTest, CompatibleConditionsAggregateDeterministicallyAndEvaluate)
{
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(
      second, {{.address = FIRST_ADDRESS, .condition = "r4 == 2"}}));
  ASSERT_TRUE(Registry().ReplaceClientSourceBreakpoints(
      first, "source:a",
      {{.address = FIRST_ADDRESS, .condition = "r5 == 3"},
       {.address = FIRST_ADDRESS, .condition = "r3 == 1"}}));

  auto effective = Registry().GetRegularBreakpoint(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  ASSERT_TRUE(effective->condition.has_value());
  EXPECT_EQ(effective->condition->GetText(), "(r3==1) || (r4==2) || (r5==3)");

  auto& system = Core::System::GetInstance();
  auto& ppc_state = system.GetPPCState();
  ppc_state.gpr[3] = 0;
  ppc_state.gpr[4] = 2;
  ppc_state.gpr[5] = 0;
  EXPECT_TRUE(EvaluateCondition(system, effective->condition));
  ppc_state.gpr[4] = 0;
  EXPECT_FALSE(EvaluateCondition(system, effective->condition));

  const u64 revision = Registry().GetRevision();
  ASSERT_TRUE(Registry().Add(FIRST_ADDRESS, true, false,
                             Expression::TryParse(effective->condition->GetText())));
  EXPECT_EQ(Registry().GetRevision(), revision);
  ASSERT_TRUE(Registry().ReplaceClientSourceBreakpoints(
      first, "source:a",
      {{.address = FIRST_ADDRESS, .condition = "r3 == 1"},
       {.address = FIRST_ADDRESS, .condition = "r5 == 3"}}));
  EXPECT_EQ(Registry().GetRevision(), revision);
  effective = Registry().GetRegularBreakpoint(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  EXPECT_EQ(effective->condition->GetText(), "(r3==1) || (r4==2) || (r5==3)");
}

TEST_F(CodeBreakpointRegistryTest, UnconditionalEnabledClaimDominatesConditions)
{
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(
      first, {{.address = FIRST_ADDRESS, .condition = "r3 == 1"}}));
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(second, {{.address = FIRST_ADDRESS}}));

  const auto effective = Registry().GetRegularBreakpoint(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  EXPECT_FALSE(effective->condition.has_value());
  EXPECT_TRUE(effective->break_on_hit);
}

TEST_F(CodeBreakpointRegistryTest, DisabledClaimsDoNotBroadenEnabledConditionOrActions)
{
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(
      first, {{.address = FIRST_ADDRESS, .condition = "r3 == 1"}}));
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(second, {{.address = FIRST_ADDRESS,
                                                                       .is_enabled = false,
                                                                       .log_on_hit = true,
                                                                       .break_on_hit = false}}));

  const auto effective = Registry().GetRegularBreakpoint(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  ASSERT_TRUE(effective->condition.has_value());
  EXPECT_EQ(effective->condition->GetText(), "r3==1");
  EXPECT_TRUE(effective->break_on_hit);
  EXPECT_FALSE(effective->log_on_hit);
}

TEST_F(CodeBreakpointRegistryTest, DisabledOnlyClaimsAreInert)
{
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(
      first, {{.address = FIRST_ADDRESS, .is_enabled = false, .condition = "r3 == 1"}}));
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(second, {{.address = FIRST_ADDRESS,
                                                                       .is_enabled = false,
                                                                       .log_on_hit = true,
                                                                       .break_on_hit = false,
                                                                       .condition = "r4 == 2"}}));

  const auto effective = Registry().GetRegularBreakpoint(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  EXPECT_FALSE(effective->is_enabled);
  EXPECT_FALSE(effective->break_on_hit);
  EXPECT_FALSE(effective->log_on_hit);
  EXPECT_FALSE(effective->condition.has_value());
}

TEST_F(CodeBreakpointRegistryTest, EventsAreRevisionedAndTemporaryBreakpointsStayPrivate)
{
  std::vector<std::shared_ptr<const BreakPoints::Event>> events;
  const auto observer = Register(
      [&events](std::shared_ptr<const BreakPoints::Event> event) { events.push_back(event); });
  const auto writer = Register();

  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(writer, {{.address = FIRST_ADDRESS}}));
  (void)Registry().Add(GUI_ADDRESS);
  Registry().SetTemporary(SECOND_ADDRESS);
  Registry().ClearTemporary();
  Registry().UnregisterClient(writer);

  ASSERT_EQ(events.size(), 3u);
  EXPECT_LT(events[0]->revision, events[1]->revision);
  EXPECT_LT(events[1]->revision, events[2]->revision);
  EXPECT_EQ(events[0]->origin, writer);
  EXPECT_FALSE(events[1]->origin.has_value());
  EXPECT_EQ(events[2]->changes.front().reason, BreakPoints::ChangeReason::Removed);
  EXPECT_TRUE(Registry().IsAddressBreakPoint(GUI_ADDRESS));
  EXPECT_FALSE(Registry().IsAddressBreakPoint(FIRST_ADDRESS));
  Registry().UnregisterClient(observer);
}

TEST_F(CodeBreakpointRegistryTest, SnapshotsRemainValidDuringConcurrentReplacement)
{
  const auto writer = Register();
  std::atomic<bool> done = false;
  std::thread update_thread([&] {
    for (u32 i = 0; i < 1000; ++i)
    {
      EXPECT_TRUE(Registry().ReplaceClientInstructionBreakpoints(
          writer, {{.address = FIRST_ADDRESS + (i % 2) * 4}}));
    }
    done.store(true);
  });

  std::vector<std::shared_ptr<const BreakPoints::Snapshot>> retained;
  while (!done.load())
  {
    const auto snapshot = Registry().GetSnapshot();
    retained.push_back(snapshot);
    EXPECT_LE(snapshot->breakpoints.size(), 1u);
    if (!snapshot->breakpoints.empty())
    {
      const u32 address = snapshot->breakpoints.front().address;
      EXPECT_TRUE(address == FIRST_ADDRESS || address == FIRST_ADDRESS + 4);
    }
    EXPECT_TRUE(snapshot->breaking_enabled);
  }
  update_thread.join();

  for (const auto& snapshot : retained)
    ASSERT_LE(snapshot->breakpoints.size(), 1u);
}

TEST_F(CodeBreakpointRegistryTest, ReentrantMutationIsDeliveredAfterCurrentEvent)
{
  std::vector<u64> revisions;
  bool added_gui_breakpoint = false;
  const auto observer = Register([&](std::shared_ptr<const BreakPoints::Event> event) {
    revisions.push_back(event->revision);
    if (!added_gui_breakpoint)
    {
      added_gui_breakpoint = true;
      (void)Registry().Add(GUI_ADDRESS);
    }
  });
  const auto writer = Register();

  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(writer, {{.address = FIRST_ADDRESS}}));
  ASSERT_EQ(revisions.size(), 2u);
  EXPECT_LT(revisions[0], revisions[1]);
  EXPECT_TRUE(Registry().IsAddressBreakPoint(GUI_ADDRESS));
  Registry().UnregisterClient(observer);
}

TEST_F(CodeBreakpointRegistryTest, CallbackCanUnregisterItselfWithoutLaterDelivery)
{
  int callback_count = 0;
  BreakPoints::ClientId observer = 0;
  observer = Register([&](std::shared_ptr<const BreakPoints::Event>) {
    ++callback_count;
    Registry().UnregisterClient(observer);
  });
  const auto writer = Register();

  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(writer, {{.address = FIRST_ADDRESS}}));
  ASSERT_TRUE(
      Registry().ReplaceClientInstructionBreakpoints(writer, {{.address = SECOND_ADDRESS}}));
  EXPECT_EQ(callback_count, 1);
}

TEST_F(CodeBreakpointRegistryTest, UnregisterWaitsForInFlightCallback)
{
  std::atomic<bool> callback_started = false;
  std::atomic<bool> release_callback = false;
  std::atomic<bool> unregister_started = false;
  std::atomic<bool> unregister_returned = false;
  std::atomic<int> callback_count = 0;
  const auto observer = Register([&](std::shared_ptr<const BreakPoints::Event>) {
    ++callback_count;
    callback_started.store(true);
    while (!release_callback.load())
      std::this_thread::yield();
  });
  const auto writer = Register();

  std::thread update_thread([&] {
    EXPECT_TRUE(
        Registry().ReplaceClientInstructionBreakpoints(writer, {{.address = FIRST_ADDRESS}}));
  });
  while (!callback_started.load())
    std::this_thread::yield();

  std::thread unregister_thread([&] {
    unregister_started.store(true);
    Registry().UnregisterClient(observer);
    unregister_returned.store(true);
  });
  while (!unregister_started.load())
    std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(unregister_returned.load());

  release_callback.store(true);
  update_thread.join();
  unregister_thread.join();
  EXPECT_TRUE(unregister_returned.load());
  ASSERT_TRUE(
      Registry().ReplaceClientInstructionBreakpoints(writer, {{.address = SECOND_ADDRESS}}));
  EXPECT_EQ(callback_count.load(), 1);
}

TEST_F(CodeBreakpointRegistryTest, TemporaryOverlapUsesPrivateSnapshotState)
{
  const auto add_result = Registry().Add(GUI_ADDRESS, false, true, {});
  ASSERT_TRUE(add_result) << add_result.error();
  const auto before = Registry().GetRegularBreakpoint(GUI_ADDRESS);
  ASSERT_NE(before, nullptr);
  EXPECT_FALSE(before->break_on_hit);
  EXPECT_TRUE(before->log_on_hit);
  const u64 revision = Registry().GetRevision();

  Registry().SetTemporary(GUI_ADDRESS);
  const auto temporary = Registry().GetBreakpoint(GUI_ADDRESS);
  ASSERT_NE(temporary, nullptr);
  EXPECT_TRUE(temporary->break_on_hit);
  EXPECT_FALSE(temporary->log_on_hit);
  EXPECT_EQ(Registry().GetRevision(), revision);
  EXPECT_EQ(Registry().GetStrings().size(), 1u);

  Registry().ClearTemporary();
  const auto after = Registry().GetBreakpoint(GUI_ADDRESS);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after->address, before->address);
  EXPECT_FALSE(after->break_on_hit);
  EXPECT_FALSE(before->break_on_hit);
}

TEST_F(CodeBreakpointRegistryTest, ConditionalTemporaryClearPreservesReplacement)
{
  Registry().SetTemporary(FIRST_ADDRESS);
  Registry().SetTemporary(SECOND_ADDRESS);

  Registry().ClearTemporary(FIRST_ADDRESS);

  EXPECT_EQ(Registry().GetBreakpoint(FIRST_ADDRESS), nullptr);
  EXPECT_NE(Registry().GetBreakpoint(SECOND_ADDRESS), nullptr);
}

TEST_F(CodeBreakpointRegistryTest, SourceAndInstructionClaimsAtSameAddressAreIndependent)
{
  const auto client = Register();
  const BreakPoints::CodeBreakpoint claim{.address = FIRST_ADDRESS, .condition = "r3 == 1"};
  ASSERT_TRUE(Registry().ReplaceClientSourceBreakpoints(client, "source:a", {claim}));
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(client, {claim}));
  ASSERT_TRUE(Registry().ReplaceClientSourceBreakpoints(client, "source:a", {}));
  EXPECT_TRUE(Registry().IsAddressBreakPoint(FIRST_ADDRESS));
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(client, {}));
  EXPECT_FALSE(Registry().IsAddressBreakPoint(FIRST_ADDRESS));
}

TEST_F(CodeBreakpointRegistryTest, GuiRemoveDeletesEverySiteClaimAndDapCanReAdd)
{
  std::vector<std::shared_ptr<const BreakPoints::Event>> events;
  Register(
      [&](std::shared_ptr<const BreakPoints::Event> event) { events.push_back(std::move(event)); });
  const auto client = Register();
  ASSERT_TRUE(Registry().ReplaceClientSourceBreakpoints(
      client, "source:a", {{.address = GUI_ADDRESS}, {.address = FIRST_ADDRESS}}));
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(client, {{.address = GUI_ADDRESS}}));
  (void)Registry().Add(GUI_ADDRESS);
  ASSERT_EQ(Registry().GetStrings().size(), 1u);
  events.clear();

  EXPECT_TRUE(Registry().Remove(GUI_ADDRESS));
  EXPECT_FALSE(Registry().IsAddressBreakPoint(GUI_ADDRESS));
  EXPECT_TRUE(Registry().IsAddressBreakPoint(FIRST_ADDRESS));
  EXPECT_TRUE(Registry().GetStrings().empty());
  ASSERT_EQ(events.size(), 1u);
  ASSERT_EQ(events.front()->changes.size(), 1u);
  EXPECT_EQ(events.front()->changes.front().reason, BreakPoints::ChangeReason::Removed);

  ASSERT_TRUE(Registry().ReplaceClientSourceBreakpoints(
      client, "source:a", {{.address = GUI_ADDRESS}, {.address = FIRST_ADDRESS}}));
  EXPECT_TRUE(Registry().IsAddressBreakPoint(GUI_ADDRESS));
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events.back()->changes.front().reason, BreakPoints::ChangeReason::New);
}

TEST_F(CodeBreakpointRegistryTest, GuiToggleOfEffectiveDapSiteRemovesEveryClaim)
{
  std::vector<std::shared_ptr<const BreakPoints::Event>> events;
  Register(
      [&](std::shared_ptr<const BreakPoints::Event> event) { events.push_back(std::move(event)); });
  const auto client = Register();
  ASSERT_TRUE(Registry().ReplaceClientSourceBreakpoints(
      client, "source:a", {{.address = GUI_ADDRESS}, {.address = FIRST_ADDRESS}}));
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(client, {{.address = GUI_ADDRESS}}));
  events.clear();

  EXPECT_FALSE(Registry().ToggleBreakPoint(GUI_ADDRESS));
  EXPECT_FALSE(Registry().IsAddressBreakPoint(GUI_ADDRESS));
  EXPECT_TRUE(Registry().IsAddressBreakPoint(FIRST_ADDRESS));
  ASSERT_EQ(events.size(), 1u);
  ASSERT_EQ(events.front()->changes.size(), 1u);
  EXPECT_EQ(events.front()->changes.front().reason, BreakPoints::ChangeReason::Removed);
}

TEST_F(CodeBreakpointRegistryTest, GuiClearDeletesAllRegularClaimsAndTemporaryBreakpoint)
{
  std::vector<std::shared_ptr<const BreakPoints::Event>> events;
  Register(
      [&](std::shared_ptr<const BreakPoints::Event> event) { events.push_back(std::move(event)); });
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(
      Registry().ReplaceClientSourceBreakpoints(first, "source:a", {{.address = FIRST_ADDRESS}}));
  ASSERT_TRUE(
      Registry().ReplaceClientInstructionBreakpoints(second, {{.address = SECOND_ADDRESS}}));
  (void)Registry().Add(GUI_ADDRESS);
  Registry().SetTemporary(GUI_ADDRESS + 4);
  events.clear();

  Registry().Clear();

  EXPECT_TRUE(Registry().GetSnapshot()->breakpoints.empty());
  EXPECT_FALSE(Registry().GetSnapshot()->temporary_breakpoint.has_value());
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events.front()->changes.size(), 3u);
  for (const auto& change : events.front()->changes)
    EXPECT_EQ(change.reason, BreakPoints::ChangeReason::Removed);

  ASSERT_TRUE(
      Registry().ReplaceClientInstructionBreakpoints(second, {{.address = SECOND_ADDRESS}}));
  EXPECT_TRUE(Registry().IsAddressBreakPoint(SECOND_ADDRESS));
}

TEST_F(CodeBreakpointRegistryTest, GuiToggleEnableMutatesDapOnlyAndSharedSites)
{
  std::vector<std::shared_ptr<const BreakPoints::Event>> events;
  Register(
      [&](std::shared_ptr<const BreakPoints::Event> event) { events.push_back(std::move(event)); });
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(
      Registry().ReplaceClientSourceBreakpoints(first, "source:a", {{.address = FIRST_ADDRESS}}));
  events.clear();

  EXPECT_TRUE(Registry().ToggleEnable(FIRST_ADDRESS));
  EXPECT_FALSE(Registry().IsBreakPointEnable(FIRST_ADDRESS));
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events.back()->changes.front().reason, BreakPoints::ChangeReason::Changed);
  EXPECT_TRUE(Registry().ToggleEnable(FIRST_ADDRESS));
  EXPECT_TRUE(Registry().IsBreakPointEnable(FIRST_ADDRESS));
  ASSERT_EQ(events.size(), 2u);

  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(
      second, {{.address = SECOND_ADDRESS, .condition = "r3 == 1"}}));
  (void)Registry().Add(SECOND_ADDRESS, true, false, Expression::TryParse("r3 == 1"));
  events.clear();
  EXPECT_TRUE(Registry().ToggleEnable(SECOND_ADDRESS));
  EXPECT_FALSE(Registry().IsBreakPointEnable(SECOND_ADDRESS));
  EXPECT_TRUE(Registry().ToggleEnable(SECOND_ADDRESS));
  EXPECT_TRUE(Registry().IsBreakPointEnable(SECOND_ADDRESS));
  ASSERT_EQ(events.size(), 2u);
}

TEST_F(CodeBreakpointRegistryTest, GuiEditPreservesStaleClientConditions)
{
  std::vector<std::shared_ptr<const BreakPoints::Event>> events;
  Register(
      [&](std::shared_ptr<const BreakPoints::Event> event) { events.push_back(std::move(event)); });
  const auto first = Register();
  const auto second = Register();
  const BreakPoints::CodeBreakpoint original{.address = FIRST_ADDRESS, .condition = "r3 == 1"};
  ASSERT_TRUE(Registry().ReplaceClientSourceBreakpoints(first, "source:a", {original}));
  ASSERT_TRUE(Registry().ReplaceClientInstructionBreakpoints(second, {original}));
  events.clear();

  ASSERT_TRUE(Registry().Add(FIRST_ADDRESS, true, false, Expression::TryParse("r3 == 2")));

  const auto edited = Registry().GetRegularBreakpoint(FIRST_ADDRESS);
  ASSERT_NE(edited, nullptr);
  ASSERT_TRUE(edited->condition.has_value());
  EXPECT_EQ(edited->condition->GetText(), "(r3==1) || (r3==2)");
  EXPECT_TRUE(edited->break_on_hit);
  EXPECT_FALSE(edited->log_on_hit);
  ASSERT_EQ(events.size(), 1u);
  ASSERT_EQ(events.front()->changes.size(), 1u);
  EXPECT_EQ(events.front()->changes.front().reason, BreakPoints::ChangeReason::Changed);

  const u64 revision = Registry().GetRevision();
  EXPECT_TRUE(Registry().ReplaceClientSourceBreakpoints(first, "source:a", {original}));
  EXPECT_EQ(Registry().GetRevision(), revision);
  EXPECT_TRUE(Registry().ReplaceClientInstructionBreakpoints(second, {original}));
  EXPECT_EQ(Registry().GetRevision(), revision);
}

TEST_F(CodeBreakpointRegistryTest, BreakingEnabledIsCapturedInImmutableSnapshots)
{
  const auto enabled = Registry().GetSnapshot();
  Registry().EnableBreaking(false);
  const auto disabled = Registry().GetSnapshot();
  EXPECT_TRUE(enabled->breaking_enabled);
  EXPECT_FALSE(disabled->breaking_enabled);
  EXPECT_FALSE(Registry().IsBreakingEnabled());
  Registry().EnableBreaking(true);
}
}  // namespace

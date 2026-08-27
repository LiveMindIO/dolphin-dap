// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>
#include <string_view>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/EXI/SlippiInputState.h"
#include "InputCommon/ControllerInterface/Pipes/PipeInputState.h"
#include "InputCommon/ControllerInterface/Pipes/Pipes.h"

namespace
{
TEST(MenuFrameInputStateTest, ShortPayloadRequestsUnsynchronizedInput)
{
  const std::array<u8, 2> payload{};
  const auto state = ExpansionInterface::GetMenuFrameInputState(payload);

  EXPECT_TRUE(state.input_requested);
  EXPECT_FALSE(state.synchronize_gameplay);
}

TEST(MenuFrameInputStateTest, GameplayTelemetryRemainsSynchronized)
{
  std::array<u8, ExpansionInterface::MENU_FRAME_PAYLOAD_KIND_OFFSET + 1> payload{};
  payload[1] = 0x02;
  payload[2] = 0x02;
  const auto state = ExpansionInterface::GetMenuFrameInputState(payload);

  EXPECT_FALSE(state.input_requested);
  EXPECT_TRUE(state.synchronize_gameplay);
}

TEST(MenuFrameInputStateTest, PauseTransitionsRequestUnsynchronizedInput)
{
  std::array<u8, ExpansionInterface::MENU_FRAME_PAYLOAD_KIND_OFFSET + 1> payload{};
  payload[1] = 0x02;
  payload[2] = 0x02;

  for (const u8 payload_kind : {ExpansionInterface::MENU_FRAME_PAYLOAD_KIND_PAUSE_OPEN,
                                ExpansionInterface::MENU_FRAME_PAYLOAD_KIND_PAUSE_CLOSE})
  {
    payload[ExpansionInterface::MENU_FRAME_PAYLOAD_KIND_OFFSET] = payload_kind;
    const auto state = ExpansionInterface::GetMenuFrameInputState(payload);
    EXPECT_TRUE(state.input_requested);
    EXPECT_FALSE(state.synchronize_gameplay);
  }
}

TEST(MenuFrameInputStateTest, NonGameplaySceneRequestsUnsynchronizedInput)
{
  const std::array<u8, 3> payload{0, 0x01, 0x02};
  const auto state = ExpansionInterface::GetMenuFrameInputState(payload);

  EXPECT_TRUE(state.input_requested);
  EXPECT_FALSE(state.synchronize_gameplay);
}

class PipesTest : public testing::Test
{
protected:
  void SetUp() override
  {
    Config::Init();
    ASSERT_EQ(pipe(m_fds), 0);
    ASSERT_NE(fcntl(m_fds[0], F_SETFL, fcntl(m_fds[0], F_GETFL) | O_NONBLOCK), -1);
    m_device = std::make_unique<ciface::Pipes::PipeDevice>(m_fds[0], "TestPipe");
    Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, true);
    ciface::Pipes::g_input_state.store(0);
    ciface::Pipes::g_pending_input_requests.store(0);
    ciface::Pipes::g_input_request_sequence.store(0);
    ciface::Pipes::g_last_input_request_us.store(0);
    ciface::Pipes::g_last_input_request_frame.store(ciface::Pipes::INPUT_FRAME_UNKNOWN);
    ciface::Pipes::g_last_input_request_source.store(
        static_cast<u8>(ciface::Pipes::InputRequestSource::Unknown));
    ciface::Pipes::g_last_consumed_request_sequence.store(0);
    ciface::Pipes::g_last_consumed_request_us.store(0);
    ciface::Pipes::g_last_si_update_us.store(0);
    ciface::Pipes::g_current_input_update = {};
    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
  }

  void TearDown() override
  {
    m_device.reset();
    if (m_fds[1] >= 0)
      close(m_fds[1]);
    Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, false);
    ciface::Pipes::g_input_state.store(0);
    ciface::Pipes::g_pending_input_requests.store(0);
    ciface::Pipes::g_input_request_sequence.store(0);
    ciface::Pipes::g_last_input_request_us.store(0);
    ciface::Pipes::g_last_input_request_frame.store(ciface::Pipes::INPUT_FRAME_UNKNOWN);
    ciface::Pipes::g_last_input_request_source.store(
        static_cast<u8>(ciface::Pipes::InputRequestSource::Unknown));
    ciface::Pipes::g_last_consumed_request_sequence.store(0);
    ciface::Pipes::g_last_consumed_request_us.store(0);
    ciface::Pipes::g_last_si_update_us.store(0);
    ciface::Pipes::g_current_input_update = {};
    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);
    Config::Shutdown();
  }

  void Write(std::string_view commands)
  {
    ASSERT_EQ(write(m_fds[1], commands.data(), commands.size()),
              static_cast<ssize_t>(commands.size()));
  }

  int PendingBytes() const
  {
    int pending = 0;
    EXPECT_EQ(ioctl(m_fds[0], FIONREAD, &pending), 0);
    return pending;
  }

  double ButtonA() const { return m_device->FindInput("Button A")->GetState(); }

  int m_fds[2]{-1, -1};
  std::unique_ptr<ciface::Pipes::PipeDevice> m_device;
};

TEST_F(PipesTest, DoesNotConsumeWhileUnarmed)
{
  Write("PRESS A\nFLUSH\n");
  const int pending = PendingBytes();
  ciface::Pipes::g_current_input_update.synchronize_gameplay = true;

  m_device->UpdateInput();

  EXPECT_EQ(ButtonA(), 0.0);
  EXPECT_EQ(PendingBytes(), pending);
}

TEST_F(PipesTest, RecordsRequestSequenceAndSIUpdateTime)
{
  ciface::Pipes::RecordSIUpdate(123);
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   1234);

  const auto first = ciface::Pipes::GetInputTimingSnapshot();
  EXPECT_EQ(first.request_sequence, 1u);
  EXPECT_GT(first.last_request_us, 123u);
  EXPECT_EQ(first.last_si_update_us, 123u);
  EXPECT_EQ(first.request_frame, 1234);
  EXPECT_EQ(first.request_source, ciface::Pipes::InputRequestSource::FrameBookend);

  ciface::Pipes::CaptureInputState();
  const auto consumed = ciface::Pipes::GetInputTimingSnapshot();
  EXPECT_EQ(consumed.consumed_request_sequence, 1u);
  EXPECT_GE(consumed.last_consumed_request_us, consumed.last_request_us);
  EXPECT_EQ(consumed.request_frame, 1234);

  ciface::Pipes::PublishInputState(true, false);
  EXPECT_EQ(ciface::Pipes::GetInputTimingSnapshot().request_sequence, 1u);

  ciface::Pipes::PublishInputState(false, true);
  EXPECT_EQ(ciface::Pipes::GetInputTimingSnapshot().request_sequence, 2u);
}

TEST_F(PipesTest, NonSIUpdatePreservesSynchronizedBatch)
{
  Write("PRESS A\nFLUSH\n");
  const int pending = PendingBytes();
  ciface::Pipes::PublishInputState(true, true);
  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);

  m_device->UpdateInput();

  EXPECT_EQ(ButtonA(), 0.0);
  EXPECT_EQ(PendingBytes(), pending);
  EXPECT_EQ(ciface::Pipes::CaptureInputState().input_requests, 1u);
}

TEST_F(PipesTest, NonSIUpdatePreservesUnsynchronizedRequestedBatch)
{
  Write("PRESS A\nFLUSH\n");
  const int pending = PendingBytes();
  ciface::Pipes::PublishInputState(false, true);
  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);

  m_device->UpdateInput();

  EXPECT_EQ(ButtonA(), 0.0);
  EXPECT_EQ(PendingBytes(), pending);
  EXPECT_EQ(ciface::Pipes::CaptureInputState().input_requests, 1u);
}

TEST_F(PipesTest, ConsumesOneBatchPerRequest)
{
  Write("PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");
  ciface::Pipes::g_current_input_update.synchronize_gameplay = true;

  ciface::Pipes::g_current_input_update.input_requests = 1;
  m_device->UpdateInput();
  EXPECT_EQ(ButtonA(), 1.0);

  ciface::Pipes::g_current_input_update.input_requests = 0;
  m_device->UpdateInput();
  EXPECT_EQ(ButtonA(), 1.0);

  ciface::Pipes::g_current_input_update.input_requests = 1;
  m_device->UpdateInput();
  EXPECT_EQ(ButtonA(), 0.0);
}

TEST_F(PipesTest, ConsumesRequestsCapturedTogether)
{
  Write("PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");
  ciface::Pipes::PublishInputState(true, true);
  ciface::Pipes::PublishInputState(true, true);

  ciface::Pipes::g_current_input_update = ciface::Pipes::CaptureInputState();
  EXPECT_EQ(ciface::Pipes::g_current_input_update.input_requests, 2u);
  m_device->UpdateInput();

  EXPECT_EQ(ButtonA(), 0.0);
  EXPECT_FALSE(ciface::Pipes::IsInputRequested());
  const auto timing = ciface::Pipes::GetInputTimingSnapshot();
  EXPECT_EQ(timing.request_sequence, 2u);
  EXPECT_EQ(timing.consumed_request_sequence, 2u);
}

TEST_F(PipesTest, DefersRequestRaisedDuringCurrentUpdate)
{
  Write("PRESS A\nFLUSH\n");
  ciface::Pipes::g_current_input_update.synchronize_gameplay = true;

  // EXI raises this request after ControllerInterface captured the current update's false value.
  ciface::Pipes::PublishInputState(true, true);
  m_device->UpdateInput();
  EXPECT_EQ(ButtonA(), 0.0);

  ciface::Pipes::g_current_input_update = ciface::Pipes::CaptureInputState();
  m_device->UpdateInput();
  EXPECT_EQ(ButtonA(), 1.0);
}

TEST_F(PipesTest, DefersModeChangeUntilNextUpdate)
{
  Write("PRESS A\nFLUSH\n");
  ciface::Pipes::g_current_input_update.synchronize_gameplay = true;

  // EXI leaves gameplay after ControllerInterface captured this update's synchronized mode.
  ciface::Pipes::PublishInputState(false, true);
  m_device->UpdateInput();
  EXPECT_EQ(ButtonA(), 0.0);

  ciface::Pipes::g_current_input_update = ciface::Pipes::CaptureInputState();
  m_device->UpdateInput();
  EXPECT_EQ(ButtonA(), 1.0);
}

TEST_F(PipesTest, ConsumesWhileUnarmedOutsideGameplay)
{
  Write("PRESS A\nFLUSH\n");

  m_device->UpdateInput();

  EXPECT_EQ(ButtonA(), 1.0);
  EXPECT_EQ(PendingBytes(), 0);
}

TEST_F(PipesTest, NonblockingModeRetainsDrainBehavior)
{
  Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, false);
  Write("PRESS A\n");

  m_device->UpdateInput();

  EXPECT_EQ(ButtonA(), 1.0);
}
}  // namespace

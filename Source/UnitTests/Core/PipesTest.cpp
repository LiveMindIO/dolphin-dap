// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <memory>
#include <string_view>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "InputCommon/ControllerInterface/Pipes/Pipes.h"

extern std::atomic_bool g_need_input_for_frame;

namespace
{
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
    g_need_input_for_frame.store(false);
  }

  void TearDown() override
  {
    m_device.reset();
    if (m_fds[1] >= 0)
      close(m_fds[1]);
    Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, false);
    g_need_input_for_frame.store(false);
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

  m_device->UpdateInput();

  EXPECT_EQ(ButtonA(), 0.0);
  EXPECT_EQ(PendingBytes(), pending);
}

TEST_F(PipesTest, ConsumesOneBatchPerArm)
{
  Write("PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");

  g_need_input_for_frame.store(true);
  m_device->UpdateInput();
  EXPECT_EQ(ButtonA(), 1.0);

  g_need_input_for_frame.store(false);
  m_device->UpdateInput();
  EXPECT_EQ(ButtonA(), 1.0);

  g_need_input_for_frame.store(true);
  m_device->UpdateInput();
  EXPECT_EQ(ButtonA(), 0.0);
}

TEST_F(PipesTest, NonblockingModeRetainsDrainBehavior)
{
  Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, false);
  Write("PRESS A\n");

  m_device->UpdateInput();

  EXPECT_EQ(ButtonA(), 1.0);
}
}  // namespace

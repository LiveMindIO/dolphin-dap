// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string_view>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "Common/ChunkFile.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/ScopeGuard.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Pipes/PipeInputState.h"
#include "InputCommon/ControllerInterface/Pipes/Pipes.h"

namespace
{
constexpr u32 SI_MMIO_BASE = 0x0C006400;
constexpr u32 SI_CHANNEL_0_IN_HI = SI_MMIO_BASE + 0x04;
constexpr u32 SI_CHANNEL_0_IN_LO = SI_MMIO_BASE + 0x08;
constexpr u32 SI_CHANNEL_1_IN_HI = SI_MMIO_BASE + 0x10;
constexpr u32 SI_STATUS_REG = SI_MMIO_BASE + 0x38;
constexpr u32 SI_RDST0 = 0x20000000;
constexpr u32 SI_RDST1 = 0x00200000;
constexpr u32 SI_NOREP0 = 0x08000000;

class MemoryConfigLayerLoader final : public Config::ConfigLayerLoader
{
public:
  MemoryConfigLayerLoader() : ConfigLayerLoader(Config::LayerType::Base) {}
  void Load(Config::Layer*) override {}
  void Save(Config::Layer*) override {}
};

class CountingSIDevice final : public SerialInterface::ISIDevice
{
public:
  CountingSIDevice(Core::System& system, int device_number)
      : ISIDevice(system, SerialInterface::SIDEVICE_GC_CONTROLLER, device_number)
  {
  }

  SerialInterface::DataResponse GetData(u32& hi, u32& low) override
  {
    if (m_response != SerialInterface::DataResponse::Success)
    {
      ++m_get_data_count;
      return m_response;
    }
    hi = 0x12340000 | m_get_data_count;
    low = 0x56780000 | m_get_data_count;
    ++m_get_data_count;
    return m_response;
  }

  void SendCommand(u32, u8) override {}

  u32 GetDataCount() const { return m_get_data_count; }
  void SetResponse(SerialInterface::DataResponse response) { m_response = response; }

private:
  u32 m_get_data_count = 0;
  SerialInterface::DataResponse m_response = SerialInterface::DataResponse::Success;
};

class PipeBackedSIDevice final : public SerialInterface::ISIDevice
{
public:
  PipeBackedSIDevice(Core::System& system, int device_number,
                     std::shared_ptr<ciface::Pipes::PipeDevice> pipe)
      : ISIDevice(system, SerialInterface::SIDEVICE_GC_CONTROLLER, device_number),
        m_pipe(std::move(pipe))
  {
  }

  SerialInterface::DataResponse GetData(u32& hi, u32& low) override
  {
    ++m_get_data_count;
    const bool pressed = m_pipe->FindInput("Button A")->GetState() > 0.5;
    hi = pressed ? 0x01230000 : 0;
    low = pressed ? 0x04560000 : 0;
    return SerialInterface::DataResponse::Success;
  }

  void SendCommand(u32, u8) override {}
  u32 GetDataCount() const { return m_get_data_count; }

private:
  std::shared_ptr<ciface::Pipes::PipeDevice> m_pipe;
  u32 m_get_data_count = 0;
};

class SerialInterfaceTest : public testing::Test
{
protected:
  void SetUp() override
  {
    Core::DeclareAsCPUThread();
    Config::Init();
    Config::AddLayer(std::make_unique<MemoryConfigLayerLoader>());
    m_initialized_controller_interface = !g_controller_interface.IsInit();
    if (m_initialized_controller_interface)
      g_controller_interface.Initialize(WindowSystemInfo{});

    m_system = &Core::System::GetInstance();
    auto& si = m_system->GetSerialInterface();
    ciface::Pipes::g_input_state.store(0);
    ciface::Pipes::ClearInputRequests();
    ciface::Pipes::g_input_request_sequence.store(0);
    ciface::Pipes::g_last_input_request_us.store(0);
    ciface::Pipes::g_last_input_request_frame.store(ciface::Pipes::INPUT_FRAME_UNKNOWN);
    ciface::Pipes::g_last_input_request_source.store(
        static_cast<u8>(ciface::Pipes::InputRequestSource::Unknown));
    ciface::Pipes::g_last_consumed_request_sequence.store(0);
    ciface::Pipes::g_last_consumed_input_request_us.store(0);
    ciface::Pipes::g_last_consumed_input_request_frame.store(ciface::Pipes::INPUT_FRAME_UNKNOWN);
    ciface::Pipes::g_last_consumed_input_request_source.store(
        static_cast<u8>(ciface::Pipes::InputRequestSource::Unknown));
    ciface::Pipes::g_last_consumed_request_us.store(0);
    ciface::Pipes::g_last_si_update_us.store(0);
    for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
    {
      auto device = std::make_unique<CountingSIDevice>(*m_system, i);
      m_devices[i] = device.get();
      si.ChangeDevice(SerialInterface::SIDEVICE_GC_CONTROLLER, i);
      si.AddDevice(std::move(device));
    }

    SerialInterface::SerialInterfaceManager clean_si{*m_system};
    for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
      clean_si.AddDevice(std::make_unique<CountingSIDevice>(*m_system, i));
    std::array<u8, 4096> clean_state{};
    u8* clean_state_ptr = clean_state.data();
    PointerWrap writer(&clean_state_ptr, clean_state.size(), PointerWrap::Mode::Write);
    clean_si.DoState(writer);
    clean_state_ptr = clean_state.data();
    PointerWrap reader(&clean_state_ptr, clean_state.size(), PointerWrap::Mode::Read);
    si.DoState(reader);

    m_mapping = std::make_unique<MMIO::Mapping>();
    si.RegisterMMIO(m_mapping.get(), SI_MMIO_BASE);
    Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, true);
  }

  void TearDown() override
  {
    ciface::Pipes::g_input_state.store(0);
    ciface::Pipes::ClearInputRequests();
    ciface::Pipes::g_input_request_sequence.store(0);
    ciface::Pipes::g_last_input_request_us.store(0);
    ciface::Pipes::g_last_input_request_frame.store(ciface::Pipes::INPUT_FRAME_UNKNOWN);
    ciface::Pipes::g_last_input_request_source.store(
        static_cast<u8>(ciface::Pipes::InputRequestSource::Unknown));
    ciface::Pipes::g_last_consumed_request_sequence.store(0);
    ciface::Pipes::g_last_consumed_input_request_us.store(0);
    ciface::Pipes::g_last_consumed_input_request_frame.store(ciface::Pipes::INPUT_FRAME_UNKNOWN);
    ciface::Pipes::g_last_consumed_input_request_source.store(
        static_cast<u8>(ciface::Pipes::InputRequestSource::Unknown));
    ciface::Pipes::g_last_consumed_request_us.store(0);
    ciface::Pipes::g_last_si_update_us.store(0);
    Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, false);
    m_mapping.reset();
    for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
    {
      m_system->GetSerialInterface().ChangeDevice(SerialInterface::SIDEVICE_NONE, i);
      m_system->GetSerialInterface().RemoveDevice(i);
    }
    if (m_initialized_controller_interface)
      g_controller_interface.Shutdown();
    Config::Shutdown();
    Core::UndeclareAsCPUThread();
  }

  Core::System* m_system = nullptr;
  std::unique_ptr<MMIO::Mapping> m_mapping;
  std::array<CountingSIDevice*, SerialInterface::MAX_SI_CHANNELS> m_devices{};
  bool m_initialized_controller_interface = false;
};

TEST_F(SerialInterfaceTest, HostUpdateDoesNotConsumeSlippiRequest)
{
  ciface::Pipes::PublishInputState(true, true);
  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);

  g_controller_interface.UpdateInput();

  const auto state = ciface::Pipes::CaptureInputState();
  EXPECT_EQ(state.input_requests, 1u);
  EXPECT_TRUE(state.synchronize_gameplay);
}

TEST_F(SerialInterfaceTest, OnSIReadDefersAndLatchesOnePoll)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  auto& si = m_system->GetSerialInterface();

  si.UpdateDevices();
  EXPECT_EQ(m_devices[0]->GetDataCount(), 0u);

  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO), 0u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 0u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x12340000u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO), 0x56780000u);

  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x12340000u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);

  si.UpdateDevices();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x12340001u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 2u);
}

TEST_F(SerialInterfaceTest, OnSIReadReflectsLatePipeBatchesInCurrentResponse)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  auto& si = m_system->GetSerialInterface();

  int fds[2]{-1, -1};
  ASSERT_EQ(pipe(fds), 0);
  Common::ScopeGuard close_fds([&] {
    if (fds[0] >= 0)
      close(fds[0]);
    if (fds[1] >= 0)
      close(fds[1]);
  });
  ASSERT_NE(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK), -1);

  const int read_fd = fds[0];
  auto pipe_device = std::make_shared<ciface::Pipes::PipeDevice>(read_fd, "LateInputTestPipe");
  fds[0] = -1;
  ASSERT_TRUE(g_controller_interface.AddDevice(pipe_device));
  Common::ScopeGuard remove_pipe([pipe_device] {
    g_controller_interface.RemoveDevice(
        [pipe_device](const ciface::Core::Device* device) { return device == pipe_device.get(); });
  });

  auto si_device = std::make_unique<PipeBackedSIDevice>(*m_system, 0, pipe_device);
  auto* const si_device_ptr = si_device.get();
  si.AddDevice(std::move(si_device));

  constexpr std::string_view commands = "RELEASE A\nFLUSH\nPRESS A\nFLUSH\n";
  ASSERT_EQ(write(fds[1], commands.data(), commands.size()), static_cast<ssize_t>(commands.size()));
  ciface::Pipes::PublishInputState(true, false);

  si.UpdateDevices();
  int pending = 0;
  ASSERT_EQ(ioctl(read_fd, FIONREAD, &pending), 0);
  EXPECT_EQ(pending, static_cast<int>(commands.size()));
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 0.0);
  EXPECT_EQ(si_device_ptr->GetDataCount(), 0u);

  ciface::Pipes::PublishInputState(true, true);
  ciface::Pipes::PublishInputState(true, true);

  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO), 0x04560000u);
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 1.0);
  EXPECT_EQ(si_device_ptr->GetDataCount(), 1u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(si_device_ptr->GetDataCount(), 1u);

  ASSERT_EQ(ioctl(read_fd, FIONREAD, &pending), 0);
  EXPECT_EQ(pending, 0);
  EXPECT_FALSE(ciface::Pipes::IsInputRequested());
  const auto timing = ciface::Pipes::GetInputTimingSnapshot();
  EXPECT_EQ(timing.request_sequence, 2u);
  EXPECT_EQ(timing.consumed_request_sequence, timing.request_sequence);
  EXPECT_GE(timing.last_consumed_request_us, timing.last_request_us);
}

TEST_F(SerialInterfaceTest, OnSIReadReflectsBookendAfterPreviousResponseWasLatched)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  auto& si = m_system->GetSerialInterface();

  int fds[2]{-1, -1};
  ASSERT_EQ(pipe(fds), 0);
  Common::ScopeGuard close_fds([&] {
    if (fds[0] >= 0)
      close(fds[0]);
    if (fds[1] >= 0)
      close(fds[1]);
  });
  ASSERT_NE(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK), -1);

  auto pipe_device = std::make_shared<ciface::Pipes::PipeDevice>(fds[0], "PostPollInputTestPipe");
  fds[0] = -1;
  ASSERT_TRUE(g_controller_interface.AddDevice(pipe_device));
  Common::ScopeGuard remove_pipe([pipe_device] {
    g_controller_interface.RemoveDevice(
        [pipe_device](const ciface::Core::Device* device) { return device == pipe_device.get(); });
  });

  auto si_device = std::make_unique<PipeBackedSIDevice>(*m_system, 0, pipe_device);
  auto* const si_device_ptr = si_device.get();
  si.AddDevice(std::move(si_device));

  constexpr std::string_view commands = "PRESS A\nFLUSH\n";
  ASSERT_EQ(write(fds[1], commands.data(), commands.size()), static_cast<ssize_t>(commands.size()));
  ciface::Pipes::PublishInputState(true, false);

  si.UpdateDevices();
  for (u32 channel = 0; channel != SerialInterface::MAX_SI_CHANNELS; ++channel)
    m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI + 0xC * channel);
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 0.0);
  EXPECT_EQ(si_device_ptr->GetDataCount(), 1u);

  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   100);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 1.0);
  EXPECT_EQ(si_device_ptr->GetDataCount(), 2u);
  EXPECT_FALSE(ciface::Pipes::IsInputRequested());
  EXPECT_NE(m_mapping->Read<u32>(*m_system, SI_STATUS_REG) & SI_RDST1, 0u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_1_IN_HI), 0x12340001u);
  EXPECT_EQ(m_devices[1]->GetDataCount(), 2u);
}

TEST_F(SerialInterfaceTest, OnSIReadDefersNewRequestBetweenChannelReads)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  auto& si = m_system->GetSerialInterface();

  si.UpdateDevices();
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   100);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x12340000u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);
  EXPECT_EQ(m_devices[1]->GetDataCount(), 0u);

  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   101);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_1_IN_HI), 0x12340000u);
  EXPECT_EQ(m_devices[1]->GetDataCount(), 1u);
  EXPECT_TRUE(ciface::Pipes::IsInputRequested());

  si.UpdateDevices();
  EXPECT_FALSE(ciface::Pipes::IsInputRequested());
}

TEST_F(SerialInterfaceTest, OnSIReadLeavesCoalescedMenuRequestForNextGeneration)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  auto& si = m_system->GetSerialInterface();

  ciface::Pipes::PublishInputState(false, true, ciface::Pipes::InputRequestSource::MenuFrame);
  ciface::Pipes::PublishInputState(false, true, ciface::Pipes::InputRequestSource::MenuFrame);
  si.UpdateDevices();
  EXPECT_TRUE(ciface::Pipes::IsInputRequested());

  m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI);
  EXPECT_TRUE(ciface::Pipes::IsInputRequested());

  for (u32 channel = 1; channel != SerialInterface::MAX_SI_CHANNELS; ++channel)
    m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI + 0xC * channel);
  si.UpdateDevices();
  EXPECT_FALSE(ciface::Pipes::IsInputRequested());
}

TEST_F(SerialInterfaceTest, OnSIReadRefreshesScheduledResponseBeforeItIsLatched)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  auto& si = m_system->GetSerialInterface();

  int fds[2]{-1, -1};
  ASSERT_EQ(pipe(fds), 0);
  Common::ScopeGuard close_fds([&] {
    if (fds[0] >= 0)
      close(fds[0]);
    if (fds[1] >= 0)
      close(fds[1]);
  });
  ASSERT_NE(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK), -1);

  const int read_fd = fds[0];
  auto pipe_device = std::make_shared<ciface::Pipes::PipeDevice>(read_fd, "PhaseInputTestPipe");
  fds[0] = -1;
  ASSERT_TRUE(g_controller_interface.AddDevice(pipe_device));
  Common::ScopeGuard remove_pipe([pipe_device] {
    g_controller_interface.RemoveDevice(
        [pipe_device](const ciface::Core::Device* device) { return device == pipe_device.get(); });
  });

  auto si_device = std::make_unique<PipeBackedSIDevice>(*m_system, 0, pipe_device);
  auto* const si_device_ptr = si_device.get();
  si.AddDevice(std::move(si_device));

  constexpr std::string_view commands = "PRESS A\nFLUSH\nRELEASE A\nFLUSH\nPRESS A\nFLUSH\n";
  ASSERT_EQ(write(fds[1], commands.data(), commands.size()), static_cast<ssize_t>(commands.size()));
  ciface::Pipes::PublishInputState(true, false);

  // N arrives after the scheduled update and is refreshed by the first SI read.
  si.UpdateDevices();
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   100);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 1.0);
  for (u32 channel = 1; channel != SerialInterface::MAX_SI_CHANNELS; ++channel)
    m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI + 0xC * channel);

  // N+1 is consumed by the scheduled update, but N+2 supersedes it before any channel latches it.
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   101);
  si.UpdateDevices();
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 0.0);
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   102);

  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 1.0);
  EXPECT_FALSE(ciface::Pipes::IsInputRequested());
  EXPECT_EQ(si_device_ptr->GetDataCount(), 2u);
}

TEST_F(SerialInterfaceTest, ConsolePollingBehaviorIsUnchanged)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"Console"});
  auto& si = m_system->GetSerialInterface();

  si.UpdateDevices();

  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x12340000u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO), 0x56780000u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);
}

TEST_F(SerialInterfaceTest, OnSIReadRequiresBlockingPipes)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, false);
  auto& si = m_system->GetSerialInterface();

  si.UpdateDevices();

  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x12340000u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);
}

TEST_F(SerialInterfaceTest, LeavingOnSIReadClearsDeferredPoll)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  auto& si = m_system->GetSerialInterface();
  si.UpdateDevices();
  EXPECT_EQ(m_devices[0]->GetDataCount(), 0u);

  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"Console"});
  si.UpdateDevices();
  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);

  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x12340000u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);
}

TEST_F(SerialInterfaceTest, OnSIReadAppliesResponseErrorsAtFirstRead)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  m_devices[0]->SetResponse(SerialInterface::DataResponse::ErrorNoResponse);
  auto& si = m_system->GetSerialInterface();
  si.UpdateDevices();

  EXPECT_NE(m_mapping->Read<u32>(*m_system, SI_STATUS_REG) & SI_RDST0, 0u);
  EXPECT_NE(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI) & 0x40000000, 0u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);

  const u32 status = m_mapping->Read<u32>(*m_system, SI_STATUS_REG);
  EXPECT_EQ(status & SI_RDST0, 0u);
  EXPECT_NE(status & SI_NOREP0, 0u);
}

TEST_F(SerialInterfaceTest, SaveStateRestoresDeferredPollAndPipeRequest)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  auto& si = m_system->GetSerialInterface();
  si.UpdateDevices();
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   100);
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   101);
  const u64 saved_request_us = ciface::Pipes::GetInputTimingSnapshot().last_request_us;

  std::array<u8, 4096> state{};
  u8* state_ptr = state.data();
  PointerWrap writer(&state_ptr, state.size(), PointerWrap::Mode::Write);
  si.DoState(writer);

  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   999);
  ciface::Pipes::CaptureInputState();
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"Console"});
  si.UpdateDevices();
  const u32 polls_before_load = m_devices[0]->GetDataCount();

  state_ptr = state.data();
  PointerWrap reader(&state_ptr, state.size(), PointerWrap::Mode::Read);
  si.DoState(reader);
  EXPECT_EQ(ciface::Pipes::GetInputTimingSnapshot().last_request_us, saved_request_us);

  const auto pipe_state = ciface::Pipes::CaptureInputState();
  EXPECT_EQ(pipe_state.input_requests, 2u);
  EXPECT_TRUE(pipe_state.synchronize_gameplay);
  EXPECT_EQ(pipe_state.request_sequence, 2u);
  EXPECT_EQ(pipe_state.request_frame, 101);
  EXPECT_EQ(pipe_state.request_source, ciface::Pipes::InputRequestSource::FrameBookend);
  const auto timing = ciface::Pipes::GetInputTimingSnapshot();
  EXPECT_EQ(timing.consumed_request_sequence, timing.request_sequence);

  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI);
  EXPECT_EQ(m_devices[0]->GetDataCount(), polls_before_load + 1);
}

TEST_F(SerialInterfaceTest, SaveStateRestoresInputReadyForReadTimeRefresh)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  auto& si = m_system->GetSerialInterface();
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   100);
  si.UpdateDevices();

  std::array<u8, 4096> state{};
  u8* state_ptr = state.data();
  PointerWrap writer(&state_ptr, state.size(), PointerWrap::Mode::Write);
  si.DoState(writer);

  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"Console"});
  si.UpdateDevices();
  const u32 polls_before_load = m_devices[0]->GetDataCount();

  state_ptr = state.data();
  PointerWrap reader(&state_ptr, state.size(), PointerWrap::Mode::Read);
  si.DoState(reader);
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                   101);

  m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI);
  EXPECT_EQ(m_devices[0]->GetDataCount(), polls_before_load + 1);
  EXPECT_FALSE(ciface::Pipes::IsInputRequested());
  for (u32 channel = 1; channel != SerialInterface::MAX_SI_CHANNELS; ++channel)
    m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI + 0xC * channel);

  si.UpdateDevices();
  EXPECT_FALSE(ciface::Pipes::IsInputRequested());
  m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI);
  EXPECT_EQ(m_devices[0]->GetDataCount(), polls_before_load + 2);
}
}  // namespace

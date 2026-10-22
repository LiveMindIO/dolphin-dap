// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include "../StubHost.h"

#include <array>
#include <chrono>
#include <memory>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include "Common/ChunkFile.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/ScopeGuard.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/Movie.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Pipes/PipeInputState.h"
#ifndef _WIN32
#include "InputCommon/ControllerInterface/Pipes/Pipes.h"
#endif
#include "InputCommon/InputConfig.h"

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

#ifndef _WIN32
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
#endif

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
      Config::SetCurrent(Config::GetInfoForSIDevice(i), SerialInterface::SIDEVICE_GC_CONTROLLER);
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
      Config::SetCurrent(Config::GetInfoForSIDevice(i), SerialInterface::SIDEVICE_NONE);
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

#ifndef _WIN32
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

#endif

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

#ifndef _WIN32
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

#endif

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

TEST_F(SerialInterfaceTest, OnSIReadDoesNotRequireBlockingPipes)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, false);
  auto& si = m_system->GetSerialInterface();

  si.UpdateDevices();

  EXPECT_EQ(m_devices[0]->GetDataCount(), 0u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x12340000u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 1u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO), 0x56780000u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x12340001u);
  EXPECT_EQ(m_devices[0]->GetDataCount(), 2u);
  EXPECT_EQ(si.GetPollXLines(), 123u);
}

TEST_F(SerialInterfaceTest, DefaultPollingMatchesLegacySlippi)
{
  EXPECT_EQ(Config::MAIN_POLLING_METHOD.GetDefaultValue(), "OnSIRead");
}

#ifndef _WIN32
class CompleteAcquisitionTest : public SerialInterfaceTest
{
protected:
  void SetUp() override
  {
    SerialInterfaceTest::SetUp();
    ASSERT_TRUE(Pad::GetConfig()->ControllersNeedToBeCreated());
    for (int i = 0; i < 4; ++i)
      Pad::GetConfig()->CreateController<GCPad>(i);
    Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
    Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, false);
    for (int i = 0; i < 2; ++i)
      BindPipe(i);
    ciface::Pipes::BeginGameplayInput();
  }

  void BindPipe(int i)
  {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    ASSERT_NE(fcntl(fds[0], F_SETFL, O_NONBLOCK), -1);
    m_writers[i] = fds[1];
    m_pipes[i] =
        std::make_shared<ciface::Pipes::PipeDevice>(fds[0], "Complete" + std::to_string(i));
    ASSERT_TRUE(g_controller_interface.AddDevice(m_pipes[i]));
    ciface::Core::DeviceQualifier qualifier;
    qualifier.FromDevice(m_pipes[i].get());
    Pad::GetConfig()->GetController(i)->SetDefaultDevice(qualifier);
    auto device = std::make_unique<PipeBackedSIDevice>(*m_system, i, m_pipes[i]);
    m_pipe_si[i] = device.get();
    m_system->GetSerialInterface().AddDevice(std::move(device));
  }

  void TearDown() override
  {
    m_system->GetSerialInterface().Shutdown();
    Pad::GetConfig()->ClearControllers();
    for (int i = 0; i < 4; ++i)
    {
      if (m_writers[i] >= 0)
        close(m_writers[i]);
      g_controller_interface.RemoveDevice(
          [&](const ciface::Core::Device* device) { return device == m_pipes[i].get(); });
    }
    SerialInterfaceTest::TearDown();
  }

  void Write(int port, std::string_view text)
  {
    EXPECT_EQ(write(m_writers[port], text.data(), text.size()), static_cast<ssize_t>(text.size()));
  }

  void Bookend(int frame)
  {
    ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend,
                                     frame);
    m_system->GetSerialInterface().UpdateDevices();
  }

  std::array<int, 4> m_writers{{-1, -1, -1, -1}};
  std::array<std::shared_ptr<ciface::Pipes::PipeDevice>, 4> m_pipes{};
  std::array<PipeBackedSIDevice*, 4> m_pipe_si{};
};

TEST_F(CompleteAcquisitionTest, DelayedFullFlushCannotCreatePermanentOneFrameDebt)
{
  // An aligned cohort, followed by the recorded underflow ordering.
  Write(0, "RELEASE A\nFLUSH\n");
  Write(1, "RELEASE A\nFLUSH\n");
  Bookend(100);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  Bookend(101);
  std::thread writer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Write(0, "PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Write(1, "PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");
  });
  const u32 first = m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI);
  writer.join();
  EXPECT_EQ(first, 0x01230000u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_1_IN_HI), 0x01230000u);
  for (u32 port = 0; port < 2; ++port)
  {
    EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO + 12 * port), 0x04560000u);
    EXPECT_EQ(m_pipe_si[port]->GetDataCount(), 2u);
  }
  for (u32 port = 2; port < 4; ++port)
    EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO + 12 * port), 0x56780001u);
  // A scheduled poll without another real bookend must not consume release.
  m_system->GetSerialInterface().UpdateDevices();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  Bookend(102);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_1_IN_HI), 0u);
}

TEST_F(CompleteAcquisitionTest, SaveStateLoadInvalidatesStagedAndPartialExternalBatches)
{
  Write(0, "PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");
  Write(1, "PRESS A\nSET MAIN .9");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
  EXPECT_EQ(m_pipes[0]->PrepareBatch(deadline, [] { return false; }),
            ciface::Pipes::PipeDevice::Acquisition::Complete);
  EXPECT_EQ(m_pipes[1]->PrepareBatch(deadline, [] { return false; }),
            ciface::Pipes::PipeDevice::Acquisition::TimedOut);
  Bookend(1);
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend, 2);
  auto& si = m_system->GetSerialInterface();
  const auto stops = UnitTests::GetHostStopRequestCount();
  std::array<u8, 4096> state{};
  u8* ptr = state.data();
  PointerWrap writer(&ptr, state.size(), PointerWrap::Mode::Write);
  si.DoState(writer);
  // Saving does not commit staging, stop emulation, or consume the writer turn.
  EXPECT_EQ(UnitTests::GetHostStopRequestCount(), stops);
  EXPECT_EQ(m_pipes[0]->FindInput("Button A")->GetState(), 0.0);
  EXPECT_TRUE(ciface::Pipes::IsInputRequested());
  for (int load = 0; load < 2; ++load)
  {
    ptr = state.data();
    PointerWrap reader(&ptr, state.size(), PointerWrap::Mode::Read);
    si.DoState(reader);
    EXPECT_EQ(UnitTests::GetHostStopRequestCount(), stops + 1);
    EXPECT_EQ(ciface::Pipes::g_gameplay_bookend_sequence.load(), 0u);
    EXPECT_FALSE(ciface::Pipes::IsInputRequested());
    EXPECT_NE(m_mapping->Read<u32>(*m_system, SI_STATUS_REG) & SI_NOREP0, 0u);
    EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_STATUS_REG) & (SI_RDST0 | SI_RDST1), 0u);
    for (int port = 0; port < 2; ++port)
    {
      // Neither an old completed stage nor newly supplied suffix can commit.
      Write(port, " .1\nFLUSH\n");
      m_pipes[port]->CommitBatch();
      EXPECT_EQ(m_pipes[port]->FindInput("Button A")->GetState(), 0.0);
      EXPECT_EQ(m_pipes[port]->PrepareBatch(std::chrono::steady_clock::now() +
                                                std::chrono::milliseconds(10),
                                            [] { return false; }),
                ciface::Pipes::PipeDevice::Acquisition::Disconnected);
    }
    Bookend(2);
    EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
    EXPECT_EQ(m_pipe_si[0]->GetDataCount(), 0u);
  }
}

TEST_F(CompleteAcquisitionTest, MenuStateCannotResumeNewerExternalWriter)
{
  ciface::Pipes::PublishInputState(false, true, ciface::Pipes::InputRequestSource::MenuFrame);
  auto& si = m_system->GetSerialInterface();
  std::array<u8, 4096> state{};
  u8* ptr = state.data();
  PointerWrap writer(&ptr, state.size(), PointerWrap::Mode::Write);
  si.DoState(writer);
  Write(0, "PRESS A\nFLUSH\n");
  Write(1, "PRESS A\nFLUSH\n");
  Bookend(1);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  const auto stops = UnitTests::GetHostStopRequestCount();
  ptr = state.data();
  PointerWrap reader(&ptr, state.size(), PointerWrap::Mode::Read);
  si.DoState(reader);
  EXPECT_EQ(UnitTests::GetHostStopRequestCount(), stops + 1);
  Bookend(2);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  EXPECT_EQ(m_pipe_si[0]->GetDataCount(), 1u);
}

TEST_F(CompleteAcquisitionTest, LoadCannotClearFaultAndFreshSessionNeedsFreshPipeDevices)
{
  auto& si = m_system->GetSerialInterface();
  std::array<u8, 4096> state{};
  u8* ptr = state.data();
  PointerWrap writer(&ptr, state.size(), PointerWrap::Mode::Write);
  si.DoState(writer);
  close(m_writers[1]);
  m_writers[1] = -1;
  Write(0, "PRESS A\nFLUSH\n");
  Bookend(1);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  const auto stops = UnitTests::GetHostStopRequestCount();
  ptr = state.data();
  PointerWrap reader(&ptr, state.size(), PointerWrap::Mode::Read);
  si.DoState(reader);
  EXPECT_EQ(UnitTests::GetHostStopRequestCount(), stops);
  EXPECT_NE(m_mapping->Read<u32>(*m_system, SI_STATUS_REG) & SI_NOREP0, 0u);
  si.Shutdown();
  ciface::Pipes::BeginGameplayInput();
  EXPECT_EQ(
      m_pipes[0]->PrepareBatch(std::chrono::steady_clock::now() + std::chrono::milliseconds(10),
                               [] { return false; }),
      ciface::Pipes::PipeDevice::Acquisition::Disconnected);
  for (int port = 0; port < 2; ++port)
  {
    g_controller_interface.RemoveDevice(
        [&](const ciface::Core::Device* device) { return device == m_pipes[port].get(); });
    if (m_writers[port] >= 0)
      close(m_writers[port]);
    BindPipe(port);
    Write(port, "PRESS A\nFLUSH\n");
  }
  ciface::Pipes::BeginGameplayInput();
  Bookend(1);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(m_pipe_si[0]->GetDataCount(), 1u);
}

TEST_F(CompleteAcquisitionTest, RealBookendBeforeNextScheduledPollStillRequiresCompleteBatch)
{
  Write(0, "RELEASE A\nFLUSH\n");
  Write(1, "RELEASE A\nFLUSH\n");
  Bookend(1);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  ciface::Pipes::PublishInputState(true, true, ciface::Pipes::InputRequestSource::FrameBookend, 2);
  std::thread writer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Write(0, "PRESS A\nFLUSH\n");
    Write(1, "PRESS A\nFLUSH\n");
  });
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  writer.join();
}

TEST_F(CompleteAcquisitionTest, PartialCommandsAndCompletedPortRemainPrivateUntilWholeFlushCohort)
{
  Write(0, "PRESS A\nSET MAIN .9 .1\nSET C .2 .8\nSET L .7\n");
  Bookend(1);
  std::thread writer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(m_pipes[0]->FindInput("Button A")->GetState(), 0.0);
    EXPECT_EQ(m_pipes[0]->FindInput("Axis MAIN X +")->GetState(), 0.5);
    Write(0, "FLUSH\nRELEASE A\nFLUSH\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(m_pipes[0]->FindInput("Button A")->GetState(), 0.0);
    Write(1, "PRESS A\nFLUSH\n");
  });
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  writer.join();
  EXPECT_EQ(m_pipes[0]->FindInput("Axis MAIN X +")->GetState(), 0.8);
  EXPECT_DOUBLE_EQ(m_pipes[0]->FindInput("Axis C X -")->GetState(), 0.6);
  EXPECT_DOUBLE_EQ(m_pipes[0]->FindInput("Axis C Y +")->GetState(), 0.6);
  EXPECT_DOUBLE_EQ(m_pipes[0]->FindInput("Axis L +")->GetState(), 0.7);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_1_IN_HI), 0x01230000u);
}

TEST_F(CompleteAcquisitionTest, WatchdogFaultNeverPacksOrCommitsPartialGeneration)
{
  std::thread writer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    Write(0, "PRESS A\nFLUSH\n");
  });
  Bookend(1);
  const auto start = std::chrono::steady_clock::now();
  const auto stops = UnitTests::GetHostStopRequestCount();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  writer.join();
  EXPECT_GE(elapsed, std::chrono::milliseconds(900));
  EXPECT_LT(elapsed, std::chrono::milliseconds(1400));
  EXPECT_EQ(m_pipe_si[0]->GetDataCount(), 0u);
  EXPECT_EQ(m_pipe_si[1]->GetDataCount(), 0u);
  EXPECT_EQ(UnitTests::GetHostStopRequestCount(), stops + 1);
  EXPECT_EQ(m_pipes[0]->FindInput("Button A")->GetState(), 0.0);
  const auto status = m_mapping->Read<u32>(*m_system, SI_STATUS_REG);
  EXPECT_NE(status & SI_NOREP0, 0u);
  EXPECT_EQ(status & (SI_RDST0 | SI_RDST1), 0u);
  Write(1, "PRESS A\nFLUSH\n");
  Bookend(2);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO), 0u);
  EXPECT_EQ(m_pipe_si[0]->GetDataCount(), 0u);
  EXPECT_EQ(UnitTests::GetHostStopRequestCount(), stops + 1);
  m_mapping->Write<u32>(*m_system, SI_MMIO_BASE + 0x80, 0x40000000);
  m_mapping->Write<u32>(*m_system, SI_MMIO_BASE + 0x34, 0x00000701);
  EXPECT_NE(m_mapping->Read<u32>(*m_system, SI_MMIO_BASE + 0x34) & (1u << 29), 0u);
}

TEST_F(CompleteAcquisitionTest, EstablishedWriterEofIsImmediateFaultNotStaleSuccess)
{
  Write(0, "PRESS A\nFLUSH\n");
  close(m_writers[1]);
  m_writers[1] = -1;
  Bookend(1);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
  EXPECT_EQ(m_pipe_si[0]->GetDataCount(), 0u);
  EXPECT_EQ(m_pipes[0]->FindInput("Button A")->GetState(), 0.0);
  EXPECT_NE(m_mapping->Read<u32>(*m_system, SI_STATUS_REG) & SI_NOREP0, 0u);
}

TEST_F(CompleteAcquisitionTest, HeaderHasNoWriterResponseWait)
{
  m_system->GetSerialInterface().UpdateDevices();
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
  EXPECT_EQ(m_pipe_si[0]->GetDataCount(), 1u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_STATUS_REG) & SI_NOREP0, 0u);
}

TEST_F(CompleteAcquisitionTest, LifecycleCancellationDoesNotCommitOrPack)
{
  Write(0, "PRESS A\nFLUSH\n");
  Bookend(1);
  std::thread stop([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ciface::Pipes::PublishInputState(false, false);
  });
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  stop.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
  EXPECT_EQ(m_pipe_si[0]->GetDataCount(), 0u);
  EXPECT_EQ(m_pipes[0]->FindInput("Button A")->GetState(), 0.0);
}

TEST_F(CompleteAcquisitionTest, PhysicalAndUnboundPipesAreNotWaitedOn)
{
  Pad::GetConfig()->GetController(1)->SetDefaultDevice(
      ciface::Core::DeviceQualifier{"SDL", 0, "physical"});
  m_system->GetSerialInterface().AddDevice(std::make_unique<CountingSIDevice>(*m_system, 1));
  Write(0, "PRESS A\nFLUSH\n");
  Bookend(1);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_1_IN_HI), 0x12340000u);
}

TEST_F(CompleteAcquisitionTest, ExplicitPipeReferenceUnderPhysicalDefaultIsAcquired)
{
  for (int port = 0; port < 2; ++port)
    Pad::GetConfig()->GetController(port)->SetDefaultDevice(
        ciface::Core::DeviceQualifier{"SDL", 0, "physical"});
  ciface::Core::DeviceQualifier binding;
  binding.FromDevice(m_pipes[0].get());
  Pad::GetGroup(0, PadGroup::Buttons)
      ->controls[0]
      ->control_ref->SetExpression(
          std::string{"`"}.append(binding.ToString()).append(":Button A`"));
  Bookend(1);
  std::thread writer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Write(0, "PRESS A\nFLUSH\n");
  });
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  writer.join();
}

TEST_F(CompleteAcquisitionTest, ReplacementIsResolvedFromCurrentBindingNotOldPipeObject)
{
  g_controller_interface.RemoveDevice(
      [&](const ciface::Core::Device* device) { return device == m_pipes[0].get(); });
  auto old = m_pipes[0];
  Write(0, "RELEASE A\nFLUSH\n");
  close(m_writers[0]);
  BindPipe(0);
  Write(0, "PRESS A\nFLUSH\n");
  Write(1, "PRESS A\nFLUSH\n");
  Bookend(1);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(old->FindInput("Button A")->GetState(), 0.0);
}

TEST_F(CompleteAcquisitionTest, AllFourActualBindingsAcquiredIncludingUnpluggedSiPorts)
{
  BindPipe(2);
  BindPipe(3);
  Config::SetCurrent(Config::GetInfoForSIDevice(3), SerialInterface::SIDEVICE_NONE);
  m_system->GetSerialInterface().AddDevice(SerialInterface::SIDEVICE_NONE, 3);
  for (int port = 0; port < 4; ++port)
    Write(port, "PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");
  Bookend(1);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  for (int port = 0; port < 4; ++port)
    EXPECT_EQ(m_pipes[port]->FindInput("Button A")->GetState(), 1.0);
  for (int port = 0; port < 3; ++port)
    EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO + 12 * port), 0x04560000u);
  Bookend(2);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  for (int port = 0; port < 4; ++port)
    EXPECT_EQ(m_pipes[port]->FindInput("Button A")->GetState(), 0.0);
}

TEST_F(CompleteAcquisitionTest, FourActivePortsKeepAllLoWordsInOneGeneration)
{
  BindPipe(2);
  BindPipe(3);
  for (int port = 0; port < 4; ++port)
    Write(port, "PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");
  Bookend(1);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  for (int port : {3, 1, 2, 0})
  {
    EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI + 12 * port), 0x01230000u);
    EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO + 12 * port), 0x04560000u);
    EXPECT_EQ(m_pipe_si[port]->GetDataCount(), 1u);
  }
  Bookend(2);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI + 36), 0u);
  for (int port = 0; port < 4; ++port)
  {
    EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO + 12 * port), 0u);
    EXPECT_EQ(m_pipe_si[port]->GetDataCount(), 2u);
  }
}

TEST_F(CompleteAcquisitionTest, TwoPadsBoundToSamePipeConsumeOnlyOneBatch)
{
  ciface::Core::DeviceQualifier binding;
  binding.FromDevice(m_pipes[0].get());
  Pad::GetConfig()->GetController(1)->SetDefaultDevice(binding);
  m_system->GetSerialInterface().AddDevice(
      std::make_unique<PipeBackedSIDevice>(*m_system, 1, m_pipes[0]));
  Write(0, "PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");
  Bookend(1);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_1_IN_HI), 0x01230000u);
  Bookend(2);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
}

TEST_F(CompleteAcquisitionTest, BareFlushIsOneHeldInputTurnNotDeduplicatedOrDropped)
{
  for (int port = 0; port < 2; ++port)
    Write(port, "PRESS A\nFLUSH\nFLUSH\nRELEASE A\nFLUSH\n");
  for (int frame = 1; frame <= 3; ++frame)
  {
    Bookend(frame);
    const auto expected = frame < 3 ? 0x01230000u : 0u;
    EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), expected);
    EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_1_IN_HI), expected);
    EXPECT_EQ(m_pipe_si[0]->GetDataCount(), static_cast<u32>(frame));
  }
}

TEST_F(CompleteAcquisitionTest, NewHeaderRetiresEligibilityWithoutWaitingForAResponse)
{
  Write(0, "PRESS A\nFLUSH\n");
  Write(1, "PRESS A\nFLUSH\n");
  Bookend(1);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  ciface::Pipes::BeginGameplayInput();
  m_system->GetSerialInterface().UpdateDevices();
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
  Write(0, "RELEASE A\nFLUSH\n");
  Write(1, "RELEASE A\nFLUSH\n");
  Bookend(2);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
}

TEST_F(CompleteAcquisitionTest, NativeSessionResetCannotPublishPreviouslyFaultedPipeCohort)
{
  Write(0, "PRESS A\nFLUSH\n");
  Bookend(1);
  std::thread cancel([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ciface::Pipes::PublishInputState(false, false);
  });
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  cancel.join();
  auto& si = m_system->GetSerialInterface();
  si.Shutdown();
  m_system->GetCoreTiming().Init();
  Common::ScopeGuard shutdown_timing([&] { m_system->GetCoreTiming().Shutdown(); });
  si.Init();
  EXPECT_EQ(ciface::Pipes::g_input_state.load(), 0u);
  for (int port = 0; port < 2; ++port)
  {
    auto device = std::make_unique<PipeBackedSIDevice>(*m_system, port, m_pipes[port]);
    m_pipe_si[port] = device.get();
    si.AddDevice(std::move(device));
    Write(port, "PRESS A\nFLUSH\n");
  }
  ciface::Pipes::BeginGameplayInput();
  Bookend(2);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  EXPECT_EQ(m_pipe_si[0]->GetDataCount(), 0u);
  EXPECT_EQ(m_pipes[0]->FindInput("Button A")->GetState(), 0.0);
  EXPECT_NE(m_mapping->Read<u32>(*m_system, SI_STATUS_REG) & SI_NOREP0, 0u);
}

TEST_F(CompleteAcquisitionTest, MissingActualBindingFaultsWithoutUsingRetainedDevice)
{
  g_controller_interface.RemoveDevice(
      [&](const ciface::Core::Device* device) { return device == m_pipes[1].get(); });
  Write(0, "PRESS A\nFLUSH\n");
  Bookend(1);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  EXPECT_EQ(m_pipe_si[0]->GetDataCount(), 0u);
  EXPECT_NE(m_mapping->Read<u32>(*m_system, SI_STATUS_REG) & SI_NOREP0, 0u);
}

TEST_F(CompleteAcquisitionTest, DirectTransferUsesSameCompleteCohortAsAllHiLoReads)
{
  auto& si = m_system->GetSerialInterface();
  for (int port = 0; port < 2; ++port)
  {
    Pad::GetGroup(port, PadGroup::Buttons)->controls[0]->control_ref->SetExpression("`Button A`");
    Pad::GetConfig()->GetController(port)->UpdateReferences(g_controller_interface);
    si.AddDevice(std::make_unique<SerialInterface::CSIDevice_GCController>(
        *m_system, SerialInterface::SIDEVICE_GC_CONTROLLER, port));
  }
  Bookend(1);
  std::thread writer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Write(0, "PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Write(1, "PRESS A\nFLUSH\nRELEASE A\nFLUSH\n");
  });
  m_mapping->Write<u32>(*m_system, SI_MMIO_BASE + 0x80, 0x40000000);
  m_mapping->Write<u32>(*m_system, SI_MMIO_BASE + 0x34, 0x00000701);
  writer.join();
  const auto high = m_mapping->Read<u32>(*m_system, SI_MMIO_BASE + 0x80);
  const auto low = m_mapping->Read<u32>(*m_system, SI_MMIO_BASE + 0x84);
  EXPECT_NE(high & 0x01000000, 0u);
  EXPECT_EQ(high, m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI));
  EXPECT_EQ(low, m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO));
  EXPECT_EQ(high, m_mapping->Read<u32>(*m_system, SI_CHANNEL_1_IN_HI));
  EXPECT_EQ(low, m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO + 12));
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_MMIO_BASE + 0x34) & (1u << 29), 0u);
  Bookend(2);
  m_mapping->Write<u32>(*m_system, SI_MMIO_BASE + 0x80, 0x40000000);
  m_mapping->Write<u32>(*m_system, SI_MMIO_BASE + 0x34, 0x00000701);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_MMIO_BASE + 0x80) & 0x01000000, 0u);
}

class CompleteAcquisitionCpuTest : public CompleteAcquisitionTest,
                                   public testing::WithParamInterface<bool>
{
protected:
  void SetUp() override
  {
    CompleteAcquisitionTest::SetUp();
    m_system->GetMemory().Init();
    AddressSpace::Init();
    m_system->GetCoreTiming().Init();
    m_system->GetCPU().Init(PowerPC::CPUCore::Interpreter);
    m_system->GetCPU().SetStepping(false);
  }

  void TearDown() override
  {
    m_system->GetCPU().Shutdown();
    m_system->GetMemory().Shutdown();
    AddressSpace::Shutdown();
    m_system->GetCoreTiming().Shutdown();
    // Discard the stop job in this unbooted test core, before the next fixture.
    Core::HostDispatchJobs(*m_system);
    CompleteAcquisitionTest::TearDown();
  }
};

TEST_P(CompleteAcquisitionCpuTest, ShutdownCancelsButPauseFinishesRealPendingCohort)
{
  ASSERT_EQ(m_system->GetCPU().GetState(), CPU::State::Running);
  Write(0, "PRESS A\nFLUSH\n");
  Bookend(1);
  std::thread pause([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (GetParam())
      m_system->GetCPU().Stop();
    else
    {
      m_system->GetCPU().BreakWithoutDebugStop();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      Write(1, "PRESS A\nFLUSH\n");
    }
  });
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), GetParam() ? 0u : 0x01230000u);
  pause.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
  EXPECT_EQ(m_pipe_si[0]->GetDataCount(), GetParam() ? 0u : 1u);
  EXPECT_EQ(m_pipe_si[1]->GetDataCount(), GetParam() ? 0u : 1u);
  EXPECT_EQ(m_pipes[0]->FindInput("Button A")->GetState(), GetParam() ? 0.0 : 1.0);
  if (GetParam())
    EXPECT_NE(m_mapping->Read<u32>(*m_system, SI_STATUS_REG) & SI_NOREP0, 0u);
  else
    EXPECT_EQ(m_system->GetCPU().GetState(), CPU::State::Stepping);
}

INSTANTIATE_TEST_SUITE_P(PauseAndStop, CompleteAcquisitionCpuTest, testing::Bool());

TEST_F(SerialInterfaceTest, NonblockingGameplayConsumesEachBatchOnlyAtGuestRead)
{
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, false);
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
  auto pipe_device = std::make_shared<ciface::Pipes::PipeDevice>(read_fd, "LegacyPollingTestPipe");
  fds[0] = -1;
  ASSERT_TRUE(g_controller_interface.AddDevice(pipe_device));
  Common::ScopeGuard remove_pipe([pipe_device] {
    g_controller_interface.RemoveDevice(
        [pipe_device](const ciface::Core::Device* device) { return device == pipe_device.get(); });
  });
  auto si_device = std::make_unique<PipeBackedSIDevice>(*m_system, 0, pipe_device);
  auto* const si_device_ptr = si_device.get();
  si.AddDevice(std::move(si_device));
  ciface::Pipes::PublishInputState(true, false);

  // No writer response or bookend is required to advance nonblocking polling.
  si.UpdateDevices();
  EXPECT_EQ(si_device_ptr->GetDataCount(), 0u);
  constexpr std::string_view press = "PRESS A\nFLUSH\n";
  ASSERT_EQ(write(fds[1], press.data(), press.size()), static_cast<ssize_t>(press.size()));
  si.UpdateDevices();
  EXPECT_EQ(si_device_ptr->GetDataCount(), 0u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(si_device_ptr->GetDataCount(), 1u);

  constexpr std::string_view release = "RELEASE A\nFLUSH\n";
  ASSERT_EQ(write(fds[1], release.data(), release.size()), static_cast<ssize_t>(release.size()));
  for (const auto channel : {ciface::InputChannel::Host, ciface::InputChannel::FreeLook})
  {
    g_controller_interface.SetCurrentInputChannel(channel);
    g_controller_interface.UpdateInput();
    EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 1.0);
  }
  // Updating the device must not overwrite the already latched HI/LO pair.
  si.UpdateDevices();
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 1.0);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO), 0x04560000u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO), 0u);
  EXPECT_EQ(si_device_ptr->GetDataCount(), 2u);

  // Neither separate scheduled updates nor coalesced writes may erase a pulse.
  ASSERT_EQ(write(fds[1], press.data(), press.size()), static_cast<ssize_t>(press.size()));
  si.UpdateDevices();
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 0.0);
  ASSERT_EQ(write(fds[1], release.data(), release.size()), static_cast<ssize_t>(release.size()));
  si.UpdateDevices();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(si_device_ptr->GetDataCount(), 3u);
  // Other channels must observe this generation without draining another batch.
  m_mapping->Read<u32>(*m_system, SI_CHANNEL_1_IN_HI);
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 1.0);
  si.UpdateDevices();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);

  constexpr std::string_view pulse = "PRESS A\nFLUSH\nRELEASE A\nFLUSH\n";
  ASSERT_EQ(write(fds[1], pulse.data(), pulse.size()), static_cast<ssize_t>(pulse.size()));
  si.UpdateDevices();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  si.UpdateDevices();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
}

class FallbackPipePollingTest : public SerialInterfaceTest, public testing::WithParamInterface<bool>
{
};

TEST_P(FallbackPipePollingTest, ScheduledAndHostUpdatesConsumeWhenGuestSamplingIsIneffective)
{
  const bool recording = GetParam();
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{recording ? "OnSIRead" : "Console"});
  Config::SetCurrent(Config::SLIPPI_BLOCKING_PIPES, false);
  auto& movie = m_system->GetMovie();
  if (recording)
  {
    for (unsigned i = 0; i < MAX_BBMOTES; ++i)
      Wiimote::GetConfig()->CreateController<WiimoteEmu::Wiimote>(i);
  }
  Common::ScopeGuard stop_recording([&] {
    if (recording)
    {
      movie.EndPlayInput(false);
      Wiimote::GetConfig()->ClearControllers();
    }
  });
  if (recording)
  {
    ASSERT_TRUE(
        movie.BeginRecordingInput({Movie::ControllerType::GC, Movie::ControllerType::None,
                                   Movie::ControllerType::None, Movie::ControllerType::None},
                                  {}));
  }

  auto& si = m_system->GetSerialInterface();
  ASSERT_FALSE(si.IsPollingOnSIRead());
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  Common::ScopeGuard close_writer([&] { close(fds[1]); });
  ASSERT_NE(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK), -1);
  auto pipe_device = std::make_shared<ciface::Pipes::PipeDevice>(fds[0], "FallbackPollingTestPipe");
  ASSERT_TRUE(g_controller_interface.AddDevice(pipe_device));
  Common::ScopeGuard remove_pipe([pipe_device] {
    g_controller_interface.RemoveDevice(
        [pipe_device](const ciface::Core::Device* device) { return device == pipe_device.get(); });
  });
  auto si_device = std::make_unique<PipeBackedSIDevice>(*m_system, 0, pipe_device);
  auto* const si_device_ptr = si_device.get();
  si.AddDevice(std::move(si_device));
  ciface::Pipes::BeginGameplayInput();

  constexpr std::string_view press = "PRESS A\nFLUSH\n";
  ASSERT_EQ(write(fds[1], press.data(), press.size()), static_cast<ssize_t>(press.size()));
  si.UpdateDevices();
  EXPECT_EQ(si_device_ptr->GetDataCount(), 1u);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0x01230000u);
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 1.0);

  constexpr std::string_view release = "RELEASE A\nFLUSH\n";
  ASSERT_EQ(write(fds[1], release.data(), release.size()), static_cast<ssize_t>(release.size()));
  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);
  g_controller_interface.UpdateInput();
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 0.0);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_LO), 0x04560000u);
  EXPECT_EQ(si_device_ptr->GetDataCount(), 1u);
  si.UpdateDevices();
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);

  constexpr std::string_view pulse = "PRESS A\nFLUSH\nRELEASE A\nFLUSH\n";
  ASSERT_EQ(write(fds[1], pulse.data(), pulse.size()), static_cast<ssize_t>(pulse.size()));
  si.UpdateDevices();
  EXPECT_EQ(pipe_device->FindInput("Button A")->GetState(), 0.0);
  EXPECT_EQ(m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI), 0u);
}

INSTANTIATE_TEST_SUITE_P(ConsoleAndMovieRecording, FallbackPipePollingTest, testing::Bool());
#endif

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

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <memory>

#include "Common/ChunkFile.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Pipes/PipeInputState.h"

namespace
{
constexpr u32 SI_MMIO_BASE = 0x0C006400;
constexpr u32 SI_CHANNEL_0_IN_HI = SI_MMIO_BASE + 0x04;
constexpr u32 SI_CHANNEL_0_IN_LO = SI_MMIO_BASE + 0x08;
constexpr u32 SI_STATUS_REG = SI_MMIO_BASE + 0x38;
constexpr u32 SI_RDST0 = 0x20000000;
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
  EXPECT_TRUE(state.input_requested);
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
  ciface::Pipes::PublishInputState(true, true);

  std::array<u8, 4096> state{};
  u8* state_ptr = state.data();
  PointerWrap writer(&state_ptr, state.size(), PointerWrap::Mode::Write);
  si.DoState(writer);

  ciface::Pipes::CaptureInputState();
  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"Console"});
  si.UpdateDevices();
  const u32 polls_before_load = m_devices[0]->GetDataCount();

  state_ptr = state.data();
  PointerWrap reader(&state_ptr, state.size(), PointerWrap::Mode::Read);
  si.DoState(reader);

  const auto pipe_state = ciface::Pipes::CaptureInputState();
  EXPECT_TRUE(pipe_state.input_requested);
  EXPECT_TRUE(pipe_state.synchronize_gameplay);

  Config::SetCurrent(Config::MAIN_POLLING_METHOD, std::string{"OnSIRead"});
  m_mapping->Read<u32>(*m_system, SI_CHANNEL_0_IN_HI);
  EXPECT_EQ(m_devices[0]->GetDataCount(), polls_before_load + 1);
}
}  // namespace

// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/SI/SI.h"

#include <array>
#include <cstring>
#include <memory>

#if defined(_DEBUG)
#include <vector>

#include "Common/StringUtil.h"
#endif

#include "Common/BitField.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Common/Timer.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI_DeviceGBA.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Pipes/PipeInputState.h"

namespace SerialInterface
{
constexpr u64 INPUT_TIMING_LOG_SAMPLE_INTERVAL = 60;

// SI Internal Hardware Addresses
enum
{
  SI_CHANNEL_0_OUT = 0x00,
  SI_CHANNEL_0_IN_HI = 0x04,
  SI_CHANNEL_0_IN_LO = 0x08,
  SI_CHANNEL_1_OUT = 0x0C,
  SI_CHANNEL_1_IN_HI = 0x10,
  SI_CHANNEL_1_IN_LO = 0x14,
  SI_CHANNEL_2_OUT = 0x18,
  SI_CHANNEL_2_IN_HI = 0x1C,
  SI_CHANNEL_2_IN_LO = 0x20,
  SI_CHANNEL_3_OUT = 0x24,
  SI_CHANNEL_3_IN_HI = 0x28,
  SI_CHANNEL_3_IN_LO = 0x2C,
  SI_POLL = 0x30,
  SI_COM_CSR = 0x34,
  SI_STATUS_REG = 0x38,
  SI_EXI_CLOCK_COUNT = 0x3C,
  SI_IO_BUFFER = 0x80,
};

SerialInterfaceManager::SerialInterfaceManager(Core::System& system) : m_system(system)
{
}

SerialInterfaceManager::~SerialInterfaceManager() = default;

static constexpr u32 GetRDSTBit(u32 channel)
{
  // Returns bit for RDST0,1,2,3
  return 0x20000000 >> (channel * 8);
}

void SerialInterfaceManager::SetNoResponse(u32 channel)
{
  // raise the NO RESPONSE error
  switch (channel)
  {
  case 0:
    m_status_reg.NOREP0 = 1;
    break;
  case 1:
    m_status_reg.NOREP1 = 1;
    break;
  case 2:
    m_status_reg.NOREP2 = 1;
    break;
  case 3:
    m_status_reg.NOREP3 = 1;
    break;
  }
}

void SerialInterfaceManager::ChangeDeviceCallback(Core::System& system, u64 user_data,
                                                  s64 cycles_late)
{
  // The purpose of this callback is to simply re-enable device changes.
  auto& si = system.GetSerialInterface();
  si.m_channel[user_data].has_recent_device_unplug = false;
}

void SerialInterfaceManager::UpdateInterrupts()
{
  // check if we have to update the RDSTINT flag
  if (m_status_reg.RDST0 || m_status_reg.RDST1 || m_status_reg.RDST2 || m_status_reg.RDST3)
  {
    m_com_csr.RDSTINT = 1;
  }
  else
  {
    m_com_csr.RDSTINT = 0;
  }

  // check if we have to generate an interrupt
  const bool generate_interrupt = (m_com_csr.RDSTINT & m_com_csr.RDSTINTMSK) != 0 ||
                                  (m_com_csr.TCINT & m_com_csr.TCINTMSK) != 0;

  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_SI,
                                                generate_interrupt);
}

void SerialInterfaceManager::GenerateSIInterrupt(SIInterruptType type)
{
  switch (type)
  {
  case INT_RDSTINT:
    m_com_csr.RDSTINT = 1;
    break;
  case INT_TCINT:
    m_com_csr.TCINT = 1;
    break;
  }

  UpdateInterrupts();
}

constexpr u32 SI_XFER_LENGTH_MASK = 0x7f;

// Translate [0,1,2,...,126,127] to [128,1,2,...,126,127]
constexpr s32 ConvertSILengthField(u32 field)
{
  return ((field - 1) & SI_XFER_LENGTH_MASK) + 1;
}

void SerialInterfaceManager::GlobalRunSIBuffer(Core::System& system, u64 user_data, s64 cycles_late)
{
  system.GetSerialInterface().RunSIBuffer(user_data, cycles_late);
}

void SerialInterfaceManager::RunSIBuffer(u64 user_data, s64 cycles_late)
{
  if (m_com_csr.TSTART)
  {
    const s32 request_length = ConvertSILengthField(m_com_csr.OUTLNGTH);
    const s32 expected_response_length = ConvertSILengthField(m_com_csr.INLNGTH);

#if defined(_DEBUG)
    const std::vector<u8> request_copy(m_si_buffer.data(), m_si_buffer.data() + request_length);
#endif

    auto* const device = m_channel[m_com_csr.CHANNEL].device.get();
    const s32 actual_response_length = device->RunBuffer(m_si_buffer.data(), request_length);

    DEBUG_LOG_FMT(SERIALINTERFACE,
                  "RunSIBuffer  chan: {}  request_length: {}  expected_response_length: {}  "
                  "actual_response_length: {}",
                  m_com_csr.CHANNEL, request_length, expected_response_length,
                  actual_response_length);
    if (actual_response_length > 0 && expected_response_length != actual_response_length)
    {
#if defined(_DEBUG)
      WARN_LOG_FMT(
          SERIALINTERFACE,
          "RunSIBuffer: expected_response_length({}) != actual_response_length({}): request: {}",
          expected_response_length, actual_response_length, Common::BytesToHexString(request_copy));
#else
      WARN_LOG_FMT(SERIALINTERFACE,
                   "RunSIBuffer: expected_response_length({}) != actual_response_length({})",
                   expected_response_length, actual_response_length);
#endif
    }

    // TODO:
    // 1) Wait a reasonable amount of time for the result to be available:
    //    request is N bytes, ends with a stop bit
    //    response in M bytes, ends with a stop bit
    //    processing controller-side takes K us (investigate?)
    //    each bit takes 4us ([3us low/1us high] for a 0, [1us low/3us high] for a 1)
    //    time until response is available is at least: K + ((N*8 + 1) + (M*8 + 1)) * 4 us
    // 2) Investigate the timeout period for NOREP0
    if (actual_response_length != 0)
    {
      m_com_csr.TSTART = 0;
      m_com_csr.COMERR = actual_response_length < 0;
      if (actual_response_length < 0)
        SetNoResponse(m_com_csr.CHANNEL);
      GenerateSIInterrupt(INT_TCINT);
    }
    else
    {
      m_system.GetCoreTiming().ScheduleEvent(device->TransferInterval() - cycles_late,
                                             m_event_type_tranfer_pending);
    }
  }
}

void SerialInterfaceManager::DoState(PointerWrap& p)
{
  u8 pipe_input_state = ciface::Pipes::g_input_state.load(std::memory_order_acquire);
  u64 pipe_input_request_sequence =
      ciface::Pipes::g_input_request_sequence.load(std::memory_order_acquire);
  u64 pipe_input_request_us =
      ciface::Pipes::g_last_input_request_us.load(std::memory_order_relaxed);
  s32 pipe_input_request_frame =
      ciface::Pipes::g_last_input_request_frame.load(std::memory_order_relaxed);
  u8 pipe_input_request_source =
      ciface::Pipes::g_last_input_request_source.load(std::memory_order_relaxed);
  u64 consumed_request_sequence =
      ciface::Pipes::g_last_consumed_request_sequence.load(std::memory_order_relaxed);
  u64 consumed_input_request_us =
      ciface::Pipes::g_last_consumed_input_request_us.load(std::memory_order_relaxed);
  s32 consumed_input_request_frame =
      ciface::Pipes::g_last_consumed_input_request_frame.load(std::memory_order_relaxed);
  u8 consumed_input_request_source =
      ciface::Pipes::g_last_consumed_input_request_source.load(std::memory_order_relaxed);
  u64 consumed_request_us =
      ciface::Pipes::g_last_consumed_request_us.load(std::memory_order_relaxed);
  p.Do(pipe_input_state);
  {
    std::lock_guard lock(ciface::Pipes::g_input_request_mutex);
    u64 pending_pipe_input_requests = ciface::Pipes::g_pending_input_request_queue.size();
    p.Do(pending_pipe_input_requests);
    if (p.IsReadMode())
    {
      ciface::Pipes::g_pending_input_request_queue.clear();
      for (u64 i = 0; i != pending_pipe_input_requests; ++i)
      {
        u8 synchronize_gameplay = 0;
        u64 sequence = 0;
        u64 request_us = 0;
        s32 frame = ciface::Pipes::INPUT_FRAME_UNKNOWN;
        u8 source = 0;
        p.Do(synchronize_gameplay);
        p.Do(sequence);
        p.Do(request_us);
        p.Do(frame);
        p.Do(source);
        ciface::Pipes::g_pending_input_request_queue.push_back(
            {synchronize_gameplay != 0, sequence, request_us, frame,
             static_cast<ciface::Pipes::InputRequestSource>(source)});
      }
      ciface::Pipes::g_pending_input_requests.store(pending_pipe_input_requests,
                                                    std::memory_order_release);
    }
    else
    {
      for (const auto& request : ciface::Pipes::g_pending_input_request_queue)
      {
        u8 synchronize_gameplay = request.synchronize_gameplay;
        u64 sequence = request.sequence;
        u64 request_us = request.request_us;
        s32 frame = request.frame;
        u8 source = static_cast<u8>(request.source);
        p.Do(synchronize_gameplay);
        p.Do(sequence);
        p.Do(request_us);
        p.Do(frame);
        p.Do(source);
      }
    }
  }
  p.Do(pipe_input_request_sequence);
  p.Do(pipe_input_request_us);
  p.Do(pipe_input_request_frame);
  p.Do(pipe_input_request_source);
  p.Do(consumed_request_sequence);
  p.Do(consumed_input_request_us);
  p.Do(consumed_input_request_frame);
  p.Do(consumed_input_request_source);
  p.Do(consumed_request_us);
  if (p.IsReadMode())
  {
    ciface::Pipes::g_input_state.store(pipe_input_state, std::memory_order_release);
    ciface::Pipes::g_input_request_sequence.store(pipe_input_request_sequence,
                                                  std::memory_order_release);
    ciface::Pipes::g_last_input_request_us.store(pipe_input_request_us, std::memory_order_relaxed);
    ciface::Pipes::g_last_input_request_frame.store(pipe_input_request_frame,
                                                    std::memory_order_relaxed);
    ciface::Pipes::g_last_input_request_source.store(pipe_input_request_source,
                                                     std::memory_order_relaxed);
    ciface::Pipes::g_last_consumed_request_sequence.store(consumed_request_sequence,
                                                          std::memory_order_relaxed);
    ciface::Pipes::g_last_consumed_input_request_us.store(consumed_input_request_us,
                                                          std::memory_order_relaxed);
    ciface::Pipes::g_last_consumed_input_request_frame.store(consumed_input_request_frame,
                                                             std::memory_order_relaxed);
    ciface::Pipes::g_last_consumed_input_request_source.store(consumed_input_request_source,
                                                              std::memory_order_relaxed);
    ciface::Pipes::g_last_consumed_request_us.store(consumed_request_us, std::memory_order_relaxed);
    ciface::Pipes::g_current_input_update = {};
  }

  for (int i = 0; i < MAX_SI_CHANNELS; i++)
  {
    p.Do(m_channel[i].in_hi.hex);
    p.Do(m_channel[i].in_lo.hex);
    p.Do(m_channel[i].out.hex);
    p.Do(m_channel[i].has_recent_device_unplug);
    p.Do(m_channel[i].poll_phase);
    p.Do(m_channel[i].pipe_request_sequence);
    p.Do(m_channel[i].pipe_request_frame);
    p.Do(m_channel[i].pipe_request_source);

    const std::unique_ptr<ISIDevice>& device = m_channel[i].device;
    SIDevices type = device->GetDeviceType();
    p.Do(type);

    if (type != device->GetDeviceType())
    {
      AddDevice(SIDevice_Create(m_system, type, i));
    }

    device->DoState(p);
  }

  p.Do(m_poll);
  p.Do(m_com_csr);
  p.Do(m_status_reg);
  p.Do(m_exi_clock_count);
  p.Do(m_si_buffer);
}

void SerialInterfaceManager::RegisterEvents()
{
  auto& core_timing = m_system.GetCoreTiming();

  m_event_type_change_device = core_timing.RegisterEvent("ChangeSIDevice", ChangeDeviceCallback);
  m_event_type_tranfer_pending = core_timing.RegisterEvent("SITransferPending", GlobalRunSIBuffer);

  for (u32 i = 0; i != MAX_SI_CHANNELS; ++i)
  {
    auto& channel = m_channel[i];
    m_event_types_device[i] =
        core_timing.RegisterEvent(fmt::format("SIEventChannel{}", i),
                                  [&channel](Core::System&, u64 user_data, s64 cycles_late) {
                                    channel.device->OnEvent(user_data, cycles_late);
                                  });
  }
}

void SerialInterfaceManager::ScheduleEvent(int device_number, s64 cycles_into_future, u64 userdata)
{
  auto& core_timing = m_system.GetCoreTiming();
  core_timing.ScheduleEvent(cycles_into_future, m_event_types_device[device_number], userdata);
}

void SerialInterfaceManager::RemoveEvent(int device_number)
{
  auto& core_timing = m_system.GetCoreTiming();
  core_timing.RemoveEvent(m_event_types_device[device_number]);
}

void SerialInterfaceManager::Init()
{
  RegisterEvents();

  for (int i = 0; i < MAX_SI_CHANNELS; i++)
  {
    m_channel[i].out.hex = 0;
    m_channel[i].in_hi.hex = 0;
    m_channel[i].in_lo.hex = 0;
    m_channel[i].has_recent_device_unplug = false;
    m_channel[i].poll_phase = PendingPollPhase::None;
    m_channel[i].pipe_request_sequence = 0;
    m_channel[i].pipe_request_frame = ciface::Pipes::INPUT_FRAME_UNKNOWN;
    m_channel[i].pipe_request_source = static_cast<u8>(ciface::Pipes::InputRequestSource::Unknown);

    auto& movie = m_system.GetMovie();
    if (movie.IsMovieActive())
    {
      m_desired_device_types[i] = SIDEVICE_NONE;

      if (movie.IsUsingGBA(i))
      {
        m_desired_device_types[i] = SIDEVICE_GC_GBA_EMULATED;
      }
      else if (movie.IsUsingPad(i))
      {
        const SIDevices current = Config::Get(Config::GetInfoForSIDevice(i));
        // GC pad-compatible devices can be used for both playing and recording
        if (movie.IsUsingBongo(i))
          m_desired_device_types[i] = SIDEVICE_GC_TARUKONGA;
        else if (SIDevice_IsGCController(current))
          m_desired_device_types[i] = current;
        else
          m_desired_device_types[i] = SIDEVICE_GC_CONTROLLER;
      }
    }
    else if (!NetPlay::IsNetPlayRunning())
    {
      m_desired_device_types[i] = Config::Get(Config::GetInfoForSIDevice(i));
    }

    AddDevice(m_desired_device_types[i], i);
  }

  m_poll.hex = 0;
  m_poll.X = 492;

  m_com_csr.hex = 0;

  m_status_reg.hex = 0;

  m_exi_clock_count.hex = 0;

  // Supposedly set on reset, but logs from real Wii don't look like it is...
  // m_exi_clock_count.LOCK = 1;

  m_si_buffer = {};
}

void SerialInterfaceManager::Shutdown()
{
  for (int i = 0; i < MAX_SI_CHANNELS; i++)
    RemoveDevice(i);
  GBAConnectionWaiter_Shutdown();
}

void SerialInterfaceManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  // Register SI buffer direct accesses.
  const u32 io_buffer_base = base | SI_IO_BUFFER;
  for (size_t i = 0; i < m_si_buffer.size(); i += sizeof(u32))
  {
    const u32 address = base | static_cast<u32>(io_buffer_base + i);

    mmio->Register(address, MMIO::ComplexRead<u32>([i](Core::System& system, u32) {
                     const auto& si = system.GetSerialInterface();
                     u32 val;
                     std::memcpy(&val, &si.m_si_buffer[i], sizeof(val));
                     return Common::swap32(val);
                   }),
                   MMIO::ComplexWrite<u32>([i](Core::System& system, u32, u32 val) {
                     auto& si = system.GetSerialInterface();
                     val = Common::swap32(val);
                     std::memcpy(&si.m_si_buffer[i], &val, sizeof(val));
                   }));
  }
  for (size_t i = 0; i < m_si_buffer.size(); i += sizeof(u16))
  {
    const u32 address = base | static_cast<u32>(io_buffer_base + i);

    mmio->Register(address, MMIO::ComplexRead<u16>([i](Core::System& system, u32) {
                     const auto& si = system.GetSerialInterface();
                     u16 val;
                     std::memcpy(&val, &si.m_si_buffer[i], sizeof(val));
                     return Common::swap16(val);
                   }),
                   MMIO::ComplexWrite<u16>([i](Core::System& system, u32, u16 val) {
                     auto& si = system.GetSerialInterface();
                     val = Common::swap16(val);
                     std::memcpy(&si.m_si_buffer[i], &val, sizeof(val));
                   }));
  }

  // In and out for the 4 SI channels.
  for (u32 i = 0; i < u32(MAX_SI_CHANNELS); ++i)
  {
    // We need to clear the RDST bit for the SI channel when reading.
    const u32 clear_rdst = ~GetRDSTBit(i);

    mmio->Register(base | (SI_CHANNEL_0_OUT + 0xC * i),
                   MMIO::DirectRead<u32>(&m_channel[i].out.hex),
                   MMIO::DirectWrite<u32>(&m_channel[i].out.hex));
    mmio->Register(
        base | (SI_CHANNEL_0_IN_HI + 0xC * i),
        MMIO::ComplexRead<u32>([i, clear_rdst](Core::System& system, u32) {
          auto& si = system.GetSerialInterface();
          const bool request_pending =
              si.m_channel[i].device->GetDeviceType() == SIDEVICE_GC_CONTROLLER &&
              ciface::Pipes::IsInputRequested();

          // An unread scheduled response may be refreshed by a newer bookend. Once any channel
          // latches that generation, preserve it for the remaining channels.
          const bool late_refresh = request_pending &&
                                    ciface::Pipes::IsSynchronizedInputRequested() &&
                                    !si.IsPipeResponsePartiallyLatched();
          if (si.IsPollingOnSIRead() &&
              (si.m_channel[i].poll_phase != PendingPollPhase::None || late_refresh))
          {
            const u64 read_us = Common::Timer::NowUs();
            u64 refresh_duration_us = 0;
            if (late_refresh)
            {
              g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
              const bool input_ready = g_controller_interface.UpdatePipeInput();
              if (input_ready)
              {
                const auto refreshed_timing = ciface::Pipes::GetInputTimingSnapshot();
                for (u32 channel_index = 0; channel_index != MAX_SI_CHANNELS; ++channel_index)
                {
                  auto& channel = si.m_channel[channel_index];
                  if (channel.device->GetDeviceType() == SIDEVICE_GC_CONTROLLER)
                  {
                    channel.poll_phase = PendingPollPhase::InputReady;
                    channel.pipe_request_sequence = refreshed_timing.consumed_request_sequence;
                    channel.pipe_request_frame = refreshed_timing.consumed_request_frame;
                    channel.pipe_request_source =
                        static_cast<u8>(refreshed_timing.consumed_request_source);
                    si.m_status_reg.hex |= GetRDSTBit(channel_index);
                  }
                }
              }
              refresh_duration_us = Common::Timer::NowUs() - read_us;
            }
            else if (request_pending && si.m_channel[i].poll_phase == PendingPollPhase::InputReady)
            {
              const auto pending_timing = ciface::Pipes::GetInputTimingSnapshot();
              DEBUG_LOG_FMT(SLIPPI_INPUT,
                            "event=input_deferred_to_next_poll channel={} owned_sequence={} "
                            "owned_frame={} pending_sequence={} pending_frame={} source={}",
                            i, si.m_channel[i].pipe_request_sequence,
                            si.m_channel[i].pipe_request_frame, pending_timing.request_sequence,
                            pending_timing.request_frame,
                            ciface::Pipes::InputRequestSourceName(pending_timing.request_source));
            }
            const u64 poll_us = Common::Timer::NowUs();
            si.PollDevice(i);
            DEBUG_LOG_FMT(
                SLIPPI_INPUT,
                "event=si_response_latched channel={} sequence={} frame={} source={} poll_us={}", i,
                si.m_channel[i].pipe_request_sequence, si.m_channel[i].pipe_request_frame,
                ciface::Pipes::InputRequestSourceName(
                    static_cast<ciface::Pipes::InputRequestSource>(
                        si.m_channel[i].pipe_request_source)),
                Common::Timer::NowUs() - poll_us);
            si.m_channel[i].poll_phase = PendingPollPhase::None;
            const auto timing = ciface::Pipes::GetInputTimingSnapshot();
            if (late_refresh && timing.consumed_request_sequence != 0 &&
                timing.consumed_request_sequence % INPUT_TIMING_LOG_SAMPLE_INTERVAL == 0)
            {
              INFO_LOG_FMT(SLIPPI_INPUT,
                           "event=input_consumed stage=deferred_si_read sequence={} "
                           "frame={} source={} channel={} request_us={} si_update_us={} read_us={} "
                           "consumed_us={} refresh_duration_us={} poll_duration_us={}",
                           timing.consumed_request_sequence, timing.consumed_request_frame,
                           ciface::Pipes::InputRequestSourceName(timing.consumed_request_source), i,
                           timing.consumed_request_us, timing.last_si_update_us, read_us,
                           timing.last_consumed_request_us, refresh_duration_us,
                           Common::Timer::NowUs() - poll_us);
            }
          }
          si.m_status_reg.hex &= clear_rdst;
          si.UpdateInterrupts();
          return si.m_channel[i].in_hi.hex;
        }),
        MMIO::DirectWrite<u32>(&m_channel[i].in_hi.hex));
    mmio->Register(base | (SI_CHANNEL_0_IN_LO + 0xC * i),
                   MMIO::ComplexRead<u32>([i, clear_rdst](Core::System& system, u32) {
                     auto& si = system.GetSerialInterface();
                     si.m_status_reg.hex &= clear_rdst;
                     si.UpdateInterrupts();
                     return si.m_channel[i].in_lo.hex;
                   }),
                   MMIO::DirectWrite<u32>(&m_channel[i].in_lo.hex));
  }

  mmio->Register(base | SI_POLL, MMIO::DirectRead<u32>(&m_poll.hex),
                 MMIO::DirectWrite<u32>(&m_poll.hex));

  mmio->Register(base | SI_COM_CSR, MMIO::DirectRead<u32>(&m_com_csr.hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& si = system.GetSerialInterface();
                   const USIComCSR tmp_com_csr(val);

                   si.m_com_csr.CHANNEL = tmp_com_csr.CHANNEL.Value();
                   si.m_com_csr.INLNGTH = tmp_com_csr.INLNGTH.Value();
                   si.m_com_csr.OUTLNGTH = tmp_com_csr.OUTLNGTH.Value();
                   si.m_com_csr.RDSTINTMSK = tmp_com_csr.RDSTINTMSK.Value();
                   si.m_com_csr.TCINTMSK = tmp_com_csr.TCINTMSK.Value();

                   if (tmp_com_csr.RDSTINT)
                     si.m_com_csr.RDSTINT = 0;
                   if (tmp_com_csr.TCINT)
                     si.m_com_csr.TCINT = 0;

                   // be careful: run si-buffer after updating the INT flags
                   if (tmp_com_csr.TSTART)
                   {
                     if (si.m_com_csr.TSTART)
                       system.GetCoreTiming().RemoveEvent(si.m_event_type_tranfer_pending);
                     si.m_com_csr.TSTART = 1;
                     si.RunSIBuffer(0, 0);
                   }

                   if (!si.m_com_csr.TSTART)
                     si.UpdateInterrupts();
                 }));

  mmio->Register(base | SI_STATUS_REG, MMIO::DirectRead<u32>(&m_status_reg.hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& si = system.GetSerialInterface();
                   const USIStatusReg tmp_status(val);

                   // clear bits ( if (tmp.bit) SISR.bit=0 )
                   if (tmp_status.NOREP0)
                     si.m_status_reg.NOREP0 = 0;
                   if (tmp_status.COLL0)
                     si.m_status_reg.COLL0 = 0;
                   if (tmp_status.OVRUN0)
                     si.m_status_reg.OVRUN0 = 0;
                   if (tmp_status.UNRUN0)
                     si.m_status_reg.UNRUN0 = 0;

                   if (tmp_status.NOREP1)
                     si.m_status_reg.NOREP1 = 0;
                   if (tmp_status.COLL1)
                     si.m_status_reg.COLL1 = 0;
                   if (tmp_status.OVRUN1)
                     si.m_status_reg.OVRUN1 = 0;
                   if (tmp_status.UNRUN1)
                     si.m_status_reg.UNRUN1 = 0;

                   if (tmp_status.NOREP2)
                     si.m_status_reg.NOREP2 = 0;
                   if (tmp_status.COLL2)
                     si.m_status_reg.COLL2 = 0;
                   if (tmp_status.OVRUN2)
                     si.m_status_reg.OVRUN2 = 0;
                   if (tmp_status.UNRUN2)
                     si.m_status_reg.UNRUN2 = 0;

                   if (tmp_status.NOREP3)
                     si.m_status_reg.NOREP3 = 0;
                   if (tmp_status.COLL3)
                     si.m_status_reg.COLL3 = 0;
                   if (tmp_status.OVRUN3)
                     si.m_status_reg.OVRUN3 = 0;
                   if (tmp_status.UNRUN3)
                     si.m_status_reg.UNRUN3 = 0;

                   // send command to devices
                   if (tmp_status.WR)
                   {
                     si.m_channel[0].device->SendCommand(si.m_channel[0].out.hex, si.m_poll.EN0);
                     si.m_channel[1].device->SendCommand(si.m_channel[1].out.hex, si.m_poll.EN1);
                     si.m_channel[2].device->SendCommand(si.m_channel[2].out.hex, si.m_poll.EN2);
                     si.m_channel[3].device->SendCommand(si.m_channel[3].out.hex, si.m_poll.EN3);

                     si.m_status_reg.WR = 0;
                     si.m_status_reg.WRST0 = 0;
                     si.m_status_reg.WRST1 = 0;
                     si.m_status_reg.WRST2 = 0;
                     si.m_status_reg.WRST3 = 0;
                   }
                 }));

  mmio->Register(base | SI_EXI_CLOCK_COUNT, MMIO::DirectRead<u32>(&m_exi_clock_count.hex),
                 MMIO::DirectWrite<u32>(&m_exi_clock_count.hex));
}

void SerialInterfaceManager::RemoveDevice(int device_number)
{
  m_channel.at(device_number).device.reset();
}

void SerialInterfaceManager::AddDevice(std::unique_ptr<ISIDevice> device)
{
  const int device_number = device->GetDeviceNumber();

  // Delete the old device
  RemoveDevice(device_number);

  // Set the new one
  m_channel.at(device_number).device = std::move(device);
  m_channel.at(device_number).poll_phase = PendingPollPhase::None;
  m_channel.at(device_number).pipe_request_sequence = 0;
}

void SerialInterfaceManager::AddDevice(const SIDevices device, int device_number)
{
  AddDevice(SIDevice_Create(m_system, device, device_number));
}

void SerialInterfaceManager::ChangeDevice(SIDevices device, int channel)
{
  // Actual device change will happen in UpdateDevices.
  m_desired_device_types[channel] = device;
}

void SerialInterfaceManager::ChangeDeviceDeterministic(SIDevices device, int channel)
{
  if (channel < 0 || channel >= MAX_SI_CHANNELS)
    return;
  if (m_channel[channel].has_recent_device_unplug)
    return;

  if (GetDeviceType(channel) != SIDEVICE_NONE)
  {
    // Detach the current device before switching to the new one.
    device = SIDEVICE_NONE;
  }

  // TODO: Resetting this state may not be necessary or accurate.
  m_channel[channel].out.hex = 0;
  m_channel[channel].in_hi.hex = 0;
  m_channel[channel].in_lo.hex = 0;
  m_channel[channel].poll_phase = PendingPollPhase::None;
  m_channel[channel].pipe_request_sequence = 0;

  AddDevice(device, channel);

  if (device == SIDEVICE_NONE)
  {
    // Prevent additional device changes on this channel for one second.
    m_channel[channel].has_recent_device_unplug = true;
    m_system.GetCoreTiming().ScheduleEvent(m_system.GetSystemTimers().GetTicksPerSecond(),
                                           m_event_type_change_device, channel);
  }
}

void SerialInterfaceManager::UpdateDevices()
{
  // Check for device change requests:
  for (int i = 0; i != MAX_SI_CHANNELS; ++i)
  {
    const SIDevices current_type = GetDeviceType(i);
    const SIDevices desired_type = m_desired_device_types[i];

    if (current_type != desired_type)
    {
      ChangeDeviceDeterministic(desired_type, i);
    }
  }

  const bool polling_on_si_read = IsPollingOnSIRead();
  const u64 update_us = Common::Timer::NowUs();
  if (polling_on_si_read)
    ciface::Pipes::RecordSIUpdate(update_us);

  // Update inputs at the rate of SI
  // Typically 120hz but is variable
  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
  const bool pipe_input_ready = g_controller_interface.UpdateInput();

  const auto timing = ciface::Pipes::GetInputTimingSnapshot();
  if (polling_on_si_read && timing.last_consumed_request_us >= update_us &&
      timing.consumed_request_sequence != 0 &&
      timing.consumed_request_sequence % INPUT_TIMING_LOG_SAMPLE_INTERVAL == 0)
  {
    INFO_LOG_FMT(SLIPPI_INPUT,
                 "event=input_consumed stage=scheduled_si_update sequence={} frame={} source={} "
                 "request_us={} "
                 "si_update_us={} consumed_us={} update_duration_us={} request_pending_after={}",
                 timing.consumed_request_sequence, timing.consumed_request_frame,
                 ciface::Pipes::InputRequestSourceName(timing.consumed_request_source),
                 timing.consumed_request_us, update_us, timing.last_consumed_request_us,
                 Common::Timer::NowUs() - update_us, ciface::Pipes::IsInputRequested());
  }

  if (polling_on_si_read)
  {
    DEBUG_LOG_FMT(SLIPPI_INPUT,
                  "event=si_poll_armed phase={} sequence={} frame={} source={} "
                  "request_pending_after={}",
                  pipe_input_ready ? "input_ready" : "awaiting_input",
                  pipe_input_ready ? timing.consumed_request_sequence : timing.request_sequence,
                  pipe_input_ready ? timing.consumed_request_frame : timing.request_frame,
                  ciface::Pipes::InputRequestSourceName(
                      pipe_input_ready ? timing.consumed_request_source : timing.request_source),
                  ciface::Pipes::IsInputRequested());

    for (u32 i = 0; i != MAX_SI_CHANNELS; ++i)
    {
      if (m_channel[i].device->GetDeviceType() == SIDEVICE_GC_CONTROLLER)
      {
        m_channel[i].poll_phase =
            pipe_input_ready ? PendingPollPhase::InputReady : PendingPollPhase::AwaitingInput;
        m_channel[i].pipe_request_sequence =
            pipe_input_ready ? timing.consumed_request_sequence : 0;
        m_channel[i].pipe_request_frame =
            pipe_input_ready ? timing.consumed_request_frame : ciface::Pipes::INPUT_FRAME_UNKNOWN;
        m_channel[i].pipe_request_source =
            static_cast<u8>(pipe_input_ready ? timing.consumed_request_source :
                                               ciface::Pipes::InputRequestSource::Unknown);
        m_status_reg.hex |= GetRDSTBit(i);
      }
      else
      {
        m_channel[i].poll_phase = PendingPollPhase::None;
        m_channel[i].pipe_request_sequence = 0;
        PollDevice(i);
      }
    }
    UpdateInterrupts();
    return;
  }

  // Update channels and set the status bit if there's new data
  for (u32 i = 0; i != MAX_SI_CHANNELS; ++i)
  {
    m_channel[i].poll_phase = PendingPollPhase::None;
    m_channel[i].pipe_request_sequence = 0;
    PollDevice(i);
  }

  UpdateInterrupts();
}

bool SerialInterfaceManager::IsPollingOnSIRead() const
{
  return Config::Get(Config::SLIPPI_BLOCKING_PIPES) &&
         Config::Get(Config::MAIN_POLLING_METHOD) == "OnSIRead" &&
         !m_system.GetMovie().IsMovieActive() && !NetPlay::IsNetPlayRunning();
}

bool SerialInterfaceManager::IsPipeResponsePartiallyLatched() const
{
  for (const auto& ready_channel : m_channel)
  {
    if (ready_channel.device->GetDeviceType() != SIDEVICE_GC_CONTROLLER ||
        ready_channel.poll_phase != PendingPollPhase::InputReady)
    {
      continue;
    }

    for (const auto& channel : m_channel)
    {
      if (channel.device->GetDeviceType() == SIDEVICE_GC_CONTROLLER &&
          channel.poll_phase == PendingPollPhase::None &&
          channel.pipe_request_sequence == ready_channel.pipe_request_sequence)
      {
        return true;
      }
    }
  }
  return false;
}

void SerialInterfaceManager::PollDevice(u32 channel)
{
  // ERRLATCH is sticky across successful transfers.
  u32 errlatch = m_channel[channel].in_hi.ERRLATCH.Value();
  switch (m_channel[channel].device->GetData(m_channel[channel].in_hi.hex,
                                             m_channel[channel].in_lo.hex))
  {
  case DataResponse::Success:
    m_status_reg.hex |= GetRDSTBit(channel);
    break;
  case DataResponse::ErrorNoResponse:
    SetNoResponse(channel);
    [[fallthrough]];
  case DataResponse::NoData:
    errlatch = 1;
    m_channel[channel].in_hi.ERRSTAT = 1;
    break;
  }

  m_channel[channel].in_hi.ERRLATCH = errlatch;
}

SIDevices SerialInterfaceManager::GetDeviceType(int channel) const
{
  if (channel < 0 || channel >= MAX_SI_CHANNELS || !m_channel[channel].device)
    return SIDEVICE_NONE;

  return m_channel[channel].device->GetDeviceType();
}

u32 SerialInterfaceManager::GetPollXLines()
{
  return m_poll.X;
}

}  // namespace SerialInterface

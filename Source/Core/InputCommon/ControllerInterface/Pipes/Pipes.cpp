// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/Pipes/Pipes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <locale>
#include <map>
#include <poll.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

extern std::atomic_bool g_need_input_for_frame;            // From EXI_DeviceSlippi.cpp
extern std::atomic_bool g_synchronize_input_for_gameplay;  // From EXI_DeviceSlippi.cpp

namespace ciface::Pipes
{
static const std::array<std::string, 12> s_button_tokens{
    {"A", "B", "X", "Y", "Z", "START", "L", "R", "D_UP", "D_DOWN", "D_LEFT", "D_RIGHT"}};

static const std::array<std::string, 2> s_shoulder_tokens{{"L", "R"}};

static const std::array<std::string, 2> s_axis_tokens{{"MAIN", "C"}};

static double StringToDouble(const std::string& text)
{
  std::istringstream is(text);
  // ignore current locale
  is.imbue(std::locale::classic());
  double result;
  is >> result;
  return result;
}

class InputBackend final : public ciface::InputBackend
{
public:
  using ciface::InputBackend::InputBackend;
  void PopulateDevices() override;
};

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}

void InputBackend::PopulateDevices()
{
  // Search the Pipes directory for files that we can open in read-only,
  // non-blocking mode. The device name is the virtual name of the file.
  File::FSTEntry fst;
  std::string dir_path = File::GetUserPath(D_PIPES_IDX);
  if (!File::Exists(dir_path))
    return;
  fst = File::ScanDirectoryTree(dir_path, false);
  if (!fst.isDirectory)
    return;
  for (unsigned int i = 0; i < fst.size; ++i)
  {
    const File::FSTEntry& child = fst.children[i];
    if (child.isDirectory)
      continue;
    int fd = open(child.physicalName.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;
    g_controller_interface.AddDevice(std::make_shared<PipeDevice>(fd, child.virtualName));
  }
}

PipeDevice::PipeDevice(int fd, std::string name) : m_fd(fd), m_name(std::move(name))
{
  for (const auto& tok : s_button_tokens)
  {
    PipeInput* btn = new PipeInput("Button " + tok);
    AddInput(btn);
    m_buttons[tok] = btn;
  }
  for (const auto& tok : s_shoulder_tokens)
  {
    AddAxis(tok, 0.0);
  }
  for (const auto& tok : s_axis_tokens)
  {
    AddAxis(tok + " X", 0.5);
    AddAxis(tok + " Y", 0.5);
  }
}

PipeDevice::~PipeDevice()
{
  close(m_fd);
}

Core::DeviceRemoval PipeDevice::UpdateInput()
{
  const bool blocking_pipes = Config::Get(Config::SLIPPI_BLOCKING_PIPES);
  const bool wait_for_input = blocking_pipes && g_need_input_for_frame.load();
  if (blocking_pipes && g_synchronize_input_for_gameplay.load() && !wait_for_input)
    return Core::DeviceRemoval::Keep;

  while (true)
  {
    std::size_t newline = m_buf.find('\n');
    while (newline != std::string::npos)
    {
      std::string command = m_buf.substr(0, newline);
      m_buf.erase(0, newline + 1);
      if (ParseCommand(command) && wait_for_input)
        return Core::DeviceRemoval::Keep;
      newline = m_buf.find('\n');
    }

    char buf[32];
    const ssize_t bytes_read = read(m_fd, buf, sizeof buf);
    if (bytes_read > 0)
    {
      m_buf.append(buf, bytes_read);
      continue;
    }
    if (bytes_read == 0)
      return Core::DeviceRemoval::Keep;
    if (errno == EINTR)
      continue;
    if (errno != EAGAIN)
      return Core::DeviceRemoval::Keep;
    if (!wait_for_input)
      return Core::DeviceRemoval::Keep;

    pollfd descriptor{m_fd, POLLIN, 0};
    const int poll_result = poll(&descriptor, 1, 100);
    if (poll_result < 0 && errno != EINTR)
      return Core::DeviceRemoval::Keep;
    if (poll_result == 0)
    {
      const ::Core::State state = ::Core::GetState(::Core::System::GetInstance());
      if (state == ::Core::State::Stopping || state == ::Core::State::Uninitialized)
        return Core::DeviceRemoval::Keep;
    }
    else if (descriptor.revents & (POLLERR | POLLNVAL))
    {
      return Core::DeviceRemoval::Keep;
    }
  }
}

void PipeDevice::AddAxis(const std::string& name, double value)
{
  // Dolphin uses separate axes for left/right, which complicates things.
  PipeInput* ax_hi = new PipeInput("Axis " + name + " +");
  ax_hi->SetState(value);
  PipeInput* ax_lo = new PipeInput("Axis " + name + " -");
  ax_lo->SetState(value);
  m_axes[name + " +"] = ax_hi;
  m_axes[name + " -"] = ax_lo;
  AddFullAnalogSurfaceInputs(ax_lo, ax_hi);
}

void PipeDevice::SetAxis(const std::string& entry, double value)
{
  value = std::clamp(value, 0.0, 1.0);
  double hi = std::max(0.0, value - 0.5) * 2.0;
  double lo = (0.5 - std::min(0.5, value)) * 2.0;
  auto search_hi = m_axes.find(entry + " +");
  if (search_hi != m_axes.end())
    search_hi->second->SetState(hi);
  auto search_lo = m_axes.find(entry + " -");
  if (search_lo != m_axes.end())
    search_lo->second->SetState(lo);
}

bool PipeDevice::ParseCommand(const std::string& command)
{
  if (command == "FLUSH")
    return true;

  const std::vector<std::string> tokens = SplitString(command, ' ');
  if (tokens.size() < 2 || tokens.size() > 4)
    return false;
  if (tokens[0] == "PRESS" || tokens[0] == "RELEASE")
  {
    auto search = m_buttons.find(tokens[1]);
    if (search != m_buttons.end())
      search->second->SetState(tokens[0] == "PRESS" ? 1.0 : 0.0);
  }
  else if (tokens[0] == "SET")
  {
    if (tokens.size() == 3)
    {
      double value = StringToDouble(tokens[2]);
      SetAxis(tokens[1], (value / 2.0) + 0.5);
    }
    else if (tokens.size() == 4)
    {
      double x = StringToDouble(tokens[2]);
      double y = StringToDouble(tokens[3]);
      SetAxis(tokens[1] + " X", x);
      SetAxis(tokens[1] + " Y", y);
    }
  }
  return false;
}
}  // namespace ciface::Pipes

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace DAP
{
class DapTransport
{
public:
  explicit DapTransport(int socket);
  ~DapTransport();

  DapTransport(const DapTransport&) = delete;
  DapTransport& operator=(const DapTransport&) = delete;

  std::optional<std::string> ReadMessage();
  bool WriteMessage(std::string_view body);

  int GetSocket() const { return m_socket; }

  // Relinquish ownership of the socket without invalidating it: the transport
  // keeps using the fd for I/O but will not close it on destruction. Used when
  // the socket's lifetime is owned elsewhere (e.g. DAP::Deinit closes it to
  // unblock the accept/read on shutdown).
  void ReleaseSocket();

private:
  bool ReadExact(void* data, size_t size);

  int m_socket = -1;
  bool m_owns_socket = true;
};
}  // namespace DAP

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapTransport.h"

#include <string>

#ifdef _WIN32
#include <WinSock2.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "Core/Debugger/DAP/DapFraming.h"

namespace DAP
{
DapTransport::DapTransport(int socket) : m_socket(socket)
{
}

DapTransport::~DapTransport()
{
  if (m_socket != -1)
  {
#ifdef _WIN32
    closesocket(m_socket);
#else
    close(m_socket);
#endif
    m_socket = -1;
  }
}

void DapTransport::ReleaseSocket()
{
  m_socket = -1;
}

bool DapTransport::ReadExact(void* data, size_t size)
{
  auto* out = static_cast<char*>(data);
  while (size > 0)
  {
    const ssize_t received = recv(m_socket, out, static_cast<int>(size), MSG_WAITALL);
    if (received <= 0)
      return false;

    out += received;
    size -= static_cast<size_t>(received);
  }
  return true;
}

std::optional<std::string> DapTransport::ReadMessage()
{
  return Framing::DecodeMessage([this](void* data, size_t size) { return ReadExact(data, size); });
}

bool DapTransport::WriteMessage(std::string_view body)
{
  const std::string framed = Framing::EncodeMessage(body);

  const char* data = framed.data();
  size_t size = framed.size();
  while (size > 0)
  {
    const ssize_t sent = send(m_socket, data, static_cast<int>(size), 0);
    if (sent <= 0)
      return false;
    data += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}
}  // namespace DAP

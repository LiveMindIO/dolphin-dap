// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapTransport.h"

#include <charconv>
#include <string>

#include <fmt/format.h>

#ifdef _WIN32
#include <WinSock2.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "Common/Logging/Log.h"

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
  std::string header_line;
  header_line.reserve(128);

  while (true)
  {
    char c = 0;
    if (!ReadExact(&c, 1))
      return std::nullopt;

    if (c == '\r')
    {
      char lf = 0;
      if (!ReadExact(&lf, 1) || lf != '\n')
        return std::nullopt;
      break;
    }

    if (c == '\n')
      break;

    header_line.push_back(c);
  }

  if (header_line.empty())
    return std::string{};

  constexpr std::string_view prefix = "Content-Length: ";
  if (header_line.rfind(prefix, 0) != 0)
  {
    ERROR_LOG_FMT(CONSOLE, "DAP: unexpected header line: {}", header_line);
    return std::nullopt;
  }

  size_t content_length = 0;
  const std::string_view length_str{header_line.data() + prefix.size(),
                                    header_line.size() - prefix.size()};
  const auto [ptr, ec] = std::from_chars(length_str.data(), length_str.data() + length_str.size(),
                                         content_length);
  if (ec != std::errc{} || ptr != length_str.data() + length_str.size())
  {
    ERROR_LOG_FMT(CONSOLE, "DAP: invalid Content-Length: {}", header_line);
    return std::nullopt;
  }

  char separator = 0;
  if (!ReadExact(&separator, 1) || separator != '\r')
    return std::nullopt;
  if (!ReadExact(&separator, 1) || separator != '\n')
    return std::nullopt;

  std::string body;
  body.resize(content_length);
  if (content_length > 0 && !ReadExact(body.data(), content_length))
    return std::nullopt;

  return body;
}

bool DapTransport::WriteMessage(std::string_view body)
{
  const std::string header =
      fmt::format("Content-Length: {}\r\n\r\n", body.size());

  auto write_all = [this](const char* data, size_t size) {
    while (size > 0)
    {
      const ssize_t sent = send(m_socket, data, static_cast<int>(size), 0);
      if (sent <= 0)
        return false;
      data += sent;
      size -= static_cast<size_t>(sent);
    }
    return true;
  };

  return write_all(header.data(), header.size()) && write_all(body.data(), body.size());
}
}  // namespace DAP

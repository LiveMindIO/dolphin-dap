// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DAP.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <string.h>
#include <thread>

#ifdef _WIN32
#include <WinSock2.h>
#include <ws2tcpip.h>
typedef SSIZE_T ssize_t;
#define SHUT_RDWR SD_BOTH
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "Common/Logging/Log.h"
#include "Common/SocketContext.h"
#include "Core/Debugger/DAP/DapSession.h"
#include "Core/Debugger/DAP/DapTransport.h"

namespace DAP
{
static std::optional<Common::SocketContext> s_socket_context;
static std::thread s_io_thread;
static std::atomic<bool> s_shutting_down{false};
static std::atomic<bool> s_active{false};
static int s_listen_sock = -1;
static int s_client_sock = -1;

static void CloseSocket(int& sock)
{
  if (sock == -1)
    return;

  shutdown(sock, SHUT_RDWR);
#ifdef _WIN32
  closesocket(sock);
#else
  close(sock);
#endif
  sock = -1;
}

static void IOThreadMain()
{
  INFO_LOG_FMT(CONSOLE, "DAP: waiting for client to connect...");

  sockaddr_storage client_addr{};
  socklen_t client_addrlen = sizeof(client_addr);
  s_client_sock =
      accept(s_listen_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addrlen);
  CloseSocket(s_listen_sock);

  if (s_client_sock < 0 || s_shutting_down.load())
  {
    ERROR_LOG_FMT(CONSOLE, "DAP: failed to accept client.");
    s_active.store(false);
    return;
  }

  INFO_LOG_FMT(CONSOLE, "DAP: client connected.");

  DapTransport transport{s_client_sock};
  transport.ReleaseSocket();

  if (!RunHandshake(transport))
  {
    ERROR_LOG_FMT(CONSOLE, "DAP: handshake failed.");
    s_active.store(false);
    return;
  }

  while (!s_shutting_down.load())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  s_active.store(false);
}

static void InitGeneric(int domain, const sockaddr* server_addr, socklen_t server_addrlen)
{
  s_socket_context.emplace();
  s_shutting_down.store(false);

  s_listen_sock = socket(domain, SOCK_STREAM, 0);
  if (s_listen_sock == -1)
  {
    ERROR_LOG_FMT(CONSOLE, "DAP: failed to create socket.");
    return;
  }

  const int on = 1;
  if (setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on),
                 sizeof(on)) < 0)
  {
    ERROR_LOG_FMT(CONSOLE, "DAP: setsockopt failed.");
  }

  if (bind(s_listen_sock, server_addr, server_addrlen) < 0)
  {
    ERROR_LOG_FMT(CONSOLE, "DAP: failed to bind socket.");
    CloseSocket(s_listen_sock);
    return;
  }

  if (listen(s_listen_sock, 1) < 0)
  {
    ERROR_LOG_FMT(CONSOLE, "DAP: failed to listen on socket.");
    CloseSocket(s_listen_sock);
    return;
  }

  s_active.store(true);
  s_io_thread = std::thread(IOThreadMain);
}

#ifndef _WIN32
void InitLocal(const char* socket_path)
{
  unlink(socket_path);

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, socket_path);

  InitGeneric(PF_LOCAL, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  INFO_LOG_FMT(CONSOLE, "DAP: listening on unix socket {}", socket_path);
}
#endif

void Init(u32 port)
{
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u16>(port));
  addr.sin_addr.s_addr = INADDR_ANY;

  InitGeneric(PF_INET, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  INFO_LOG_FMT(CONSOLE, "DAP: listening on port {}", port);
}

void Deinit()
{
  if (!s_active.load() && s_listen_sock == -1 && s_client_sock == -1 && !s_io_thread.joinable())
    return;

  s_shutting_down.store(true);
  CloseSocket(s_listen_sock);
  CloseSocket(s_client_sock);

  if (s_io_thread.joinable())
    s_io_thread.join();

  s_socket_context.reset();
  s_active.store(false);
  s_shutting_down.store(false);
}

bool IsActive()
{
  return s_active.load();
}
}  // namespace DAP

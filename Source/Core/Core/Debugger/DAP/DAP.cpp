// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DAP.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string.h>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <WinSock2.h>
#include <ws2tcpip.h>
typedef SSIZE_T ssize_t;
#define SHUT_RDWR SD_BOTH
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "Common/Logging/Log.h"
#include "Common/SocketContext.h"
#include "Core/Debugger/DAP/DapSession.h"
#include "Core/Debugger/DAP/DapTransport.h"
#include "Core/System.h"

namespace DAP
{
static std::optional<Common::SocketContext> s_socket_context;
static std::thread s_accept_thread;
static std::atomic<bool> s_shutting_down{false};
static std::atomic<bool> s_active{false};
static int s_listen_sock = -1;

// A connected DAP client session. The DAP layer owns the client fd (closed in
// `ReapFinishedSessions` after the session ends, or by `Deinit` during forced
// shutdown); the transport's `ReleaseSocket()` keeps its destructor from
// double-closing.
//
// Heap-allocated (`std::unique_ptr`) so the `SessionHandle` address stays
// stable across vector reallocations; `RunClient` holds a raw pointer to it
// for its lifetime and flips `done` from the session thread on exit so the
// accept thread can `join` and free it without ambiguity.
struct SessionHandle
{
  std::thread thread;
  int client_fd = -1;
  std::atomic<bool> done{false};
};

static std::mutex s_sessions_mutex;
static std::vector<std::unique_ptr<SessionHandle>> s_sessions;

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

static void RunClient(SessionHandle* handle)
{
  const int client_fd = handle->client_fd;
  INFO_LOG_FMT(CONSOLE, "DAP: client connected (fd={}).", client_fd);

  DapTransport transport{client_fd};
  transport.ReleaseSocket();
  RunSession(transport, Core::System::GetInstance());

  INFO_LOG_FMT(CONSOLE, "DAP: client session ended (fd={}).", client_fd);
  handle->done.store(true);
}

// Wait up to `timeout_ms` for a new connection on `listen_fd`. Returns the
// accepted fd (>= 0), 0 on timeout, or -1 on error / shutdown.
static int TimedAccept(int listen_fd, int timeout_ms)
{
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(listen_fd, &readfds);

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  const int ready = select(listen_fd + 1, &readfds, nullptr, nullptr, &tv);
  if (ready <= 0)
    return ready < 0 ? -1 : 0;

  sockaddr_storage client_addr{};
  socklen_t client_addrlen = sizeof(client_addr);
  return accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_addrlen);
}

// Joins and removes session handles whose `RunClient` has finished. Called
// periodically from `AcceptLoop` so client-disconnect-then-reconnect cycles
// don't accumulate dead thread handles / leaked client fds.
static void ReapFinishedSessions()
{
  std::vector<std::unique_ptr<SessionHandle>> finished;

  {
    std::lock_guard lock(s_sessions_mutex);
    for (auto it = s_sessions.begin(); it != s_sessions.end();)
    {
      if ((*it)->done.load())
      {
        finished.push_back(std::move(*it));
        it = s_sessions.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  // Join outside the mutex so we don't block `Deinit` from making progress.
  for (auto& handle : finished)
  {
    CloseSocket(handle->client_fd);
    if (handle->thread.joinable())
      handle->thread.join();
  }
}

static void AcceptLoop()
{
  INFO_LOG_FMT(CONSOLE, "DAP: listening for clients...");

  while (!s_shutting_down.load())
  {
    const int client_fd = TimedAccept(s_listen_sock, 200);

    if (client_fd < 0)
    {
      // `select`/`accept` return < 0 on shutdown (listen socket closed).
      if (!s_shutting_down.load())
        ERROR_LOG_FMT(CONSOLE, "DAP: accept failed (errno {}).", errno);
      break;
    }
    if (client_fd == 0)
    {
      // Timed out waiting for a connection; use the chance to reap finished
      // session threads so quit-and-reconnect cycles don't leak.
      ReapFinishedSessions();
      continue;
    }

    if (s_shutting_down.load())
    {
      int fd = client_fd;
      CloseSocket(fd);
      break;
    }

    auto handle = std::make_unique<SessionHandle>();
    handle->client_fd = client_fd;
    handle->thread = std::thread(RunClient, handle.get());

    {
      std::lock_guard lock(s_sessions_mutex);
      s_sessions.push_back(std::move(handle));
    }
  }

  // Final reap of any sessions that finished while we were waiting on
  // `select`/`accept` during shutdown.
  ReapFinishedSessions();
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

  // Allow pending connections to queue so reconnects during the brief window
  // between one session ending and `AcceptLoop` looping back don't fail.
  if (listen(s_listen_sock, 4) < 0)
  {
    ERROR_LOG_FMT(CONSOLE, "DAP: failed to listen on socket.");
    CloseSocket(s_listen_sock);
    return;
  }

  s_active.store(true);
  s_accept_thread = std::thread(AcceptLoop);
}

#ifndef _WIN32
void InitLocal(const char* socket_path)
{
  // DESNOTE(jbarber, 2026-07-21): Bound the path against sun_path's capacity
  // (108 on Linux, 104 on macOS, varies elsewhere). A path longer than
  // sizeof(sun_un::sun_path) - 1 would silently truncate / overflow, so reject
  // it up front with an error and leave DAP disabled.
  const size_t path_len = std::strlen(socket_path);
  if (path_len >= sizeof(sockaddr_un::sun_path))
  {
    ERROR_LOG_FMT(CONSOLE,
                  "DAP: unix socket path too long ({} chars, max {}): {}",
                  path_len, sizeof(sockaddr_un::sun_path) - 1, socket_path);
    return;
  }

  unlink(socket_path);

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  InitGeneric(PF_LOCAL, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  if (s_active.load())
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
  // DESNOTE(jbarber, 2026-07-21): InitGeneric sets s_active only after a
  // successful bind+listen. Logging "listening" unconditionally would
  // mislead users when bind failed (port in use, permission denied, etc.)
  // and no server is actually accepting connections.
  if (s_active.load())
    INFO_LOG_FMT(CONSOLE, "DAP: listening on port {}", port);
}

void Deinit()
{
  const bool empty = [&] {
    std::lock_guard lock(s_sessions_mutex);
    return s_sessions.empty();
  }();

  if (!s_active.load() && s_listen_sock == -1 && !s_accept_thread.joinable() && empty)
    return;

  s_shutting_down.store(true);

  // Close the listening socket first so `AcceptLoop` exits its `select` and
  // stops spawning new session threads.
  CloseSocket(s_listen_sock);

  // Closing every live client fd unblocks each session's `recv` (it sees EOF
  // / `select` reports the fd as readable), so the still-running session
  // threads exit and we can `join` them below.
  {
    std::lock_guard lock(s_sessions_mutex);
    for (auto& handle : s_sessions)
      CloseSocket(handle->client_fd);
  }

  if (s_accept_thread.joinable())
    s_accept_thread.join();

  // Drain any remaining sessions. Some finished naturally between `Deinit`
  // closing the listen socket and reaching here (the accept thread's final
  // `ReapFinishedSessions` already took those); join the threads that were
  // still running when `AcceptLoop` exited.
  std::vector<std::unique_ptr<SessionHandle>> sessions;
  {
    std::lock_guard lock(s_sessions_mutex);
    sessions.swap(s_sessions);
  }
  for (auto& handle : sessions)
  {
    CloseSocket(handle->client_fd);
    if (handle->thread.joinable())
      handle->thread.join();
  }

  s_socket_context.reset();
  s_active.store(false);
  s_shutting_down.store(false);
}

bool IsActive()
{
  return s_active.load();
}
}  // namespace DAP

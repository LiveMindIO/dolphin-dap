// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "Core/Debugger/DAP/DAP.h"

#ifndef _WIN32
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
class DapServerTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_path = "/tmp/dolphin-dap-test-" + std::to_string(getpid()) + ".sock";
    DAP::InitLocal(m_path.c_str());
    ASSERT_TRUE(DAP::IsActive());
  }

  void TearDown() override
  {
    DAP::Deinit();
    unlink(m_path.c_str());
  }

  int Connect() const
  {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
      return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, m_path.c_str(), sizeof(address.sun_path) - 1);
    for (int attempt = 0; attempt < 100; ++attempt)
    {
      if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
        return fd;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    close(fd);
    return -1;
  }

  bool IsOpenWithoutData(int fd) const
  {
    pollfd poll_fd{fd, POLLIN | POLLHUP, 0};
    return poll(&poll_fd, 1, 100) == 0;
  }

  bool WaitForEof(int fd) const
  {
    pollfd poll_fd{fd, POLLIN | POLLHUP, 0};
    if (poll(&poll_fd, 1, 2000) <= 0)
      return false;
    char byte = 0;
    return recv(fd, &byte, 1, 0) == 0;
  }

  std::string m_path;
};

TEST_F(DapServerTest, RejectsClientAboveConcurrentLimitAndShutsDownIdleClients)
{
  std::array<int, 3> clients{Connect(), Connect(), Connect()};
  ASSERT_GE(clients[0], 0);
  ASSERT_GE(clients[1], 0);
  ASSERT_GE(clients[2], 0);
  EXPECT_TRUE(IsOpenWithoutData(clients[0]));
  EXPECT_TRUE(IsOpenWithoutData(clients[1]));

  EXPECT_TRUE(WaitForEof(clients[2]));

  DAP::Deinit();
  EXPECT_FALSE(DAP::IsActive());
  EXPECT_NE(access(m_path.c_str(), F_OK), 0);
  EXPECT_TRUE(WaitForEof(clients[0]));
  EXPECT_TRUE(WaitForEof(clients[1]));
  for (int fd : clients)
    close(fd);
}

TEST_F(DapServerTest, DuplicateInitLeavesOriginalServerActive)
{
  DAP::InitLocal(m_path.c_str());
  EXPECT_TRUE(DAP::IsActive());
  const int client = Connect();
  ASSERT_GE(client, 0);
  EXPECT_TRUE(IsOpenWithoutData(client));
  close(client);
}

TEST_F(DapServerTest, RefusesToReplaceRegularFileAtSocketPath)
{
  DAP::Deinit();
  {
    std::ofstream file(m_path);
    ASSERT_TRUE(file.good());
    file << "keep";
  }

  DAP::InitLocal(m_path.c_str());
  EXPECT_FALSE(DAP::IsActive());

  std::ifstream file(m_path);
  std::string contents;
  file >> contents;
  EXPECT_EQ(contents, "keep");
}
}  // namespace
#endif

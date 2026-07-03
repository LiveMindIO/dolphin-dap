// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// End-to-end integration test for the DAP session command loop. A connected
// socketpair stands in for the client<->adapter socket (no TCP, no network), and
// RunSession runs on a background thread that declares itself the CPU thread so
// the DapDebugController's CPUThreadGuards are lightweight no-ops. This drives the
// full stack -- framing, JSON parsing, command dispatch, response/event
// serialization -- against a real (un-booted) Core::System, no ISO required.

#include <array>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <picojson.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapFraming.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/Debugger/DAP/DapSession.h"
#include "Core/Debugger/DAP/DapTransport.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifndef _WIN32
namespace
{
constexpr u32 CODE_ADDRESS = 0x00003100;
constexpr u32 DATA_ADDRESS = 0x00004000;

// A DAP client over one end of a socketpair. Reads are bounded by a socket
// receive timeout so a protocol failure surfaces as a test failure instead of
// hanging the suite.
class TestClient
{
public:
  explicit TestClient(int fd) : m_fd(fd)
  {
    timeval tv{};
    tv.tv_sec = 5;
    setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  void Send(std::string_view body)
  {
    const std::string framed = DAP::Framing::EncodeMessage(body);
    size_t sent = 0;
    while (sent < framed.size())
    {
      const ssize_t n = send(m_fd, framed.data() + sent, framed.size() - sent, 0);
      ASSERT_GT(n, 0) << "send failed";
      sent += static_cast<size_t>(n);
    }
  }

  // Reads exactly one framed message and parses it as a JSON object.
  std::optional<picojson::object> Receive()
  {
    const std::optional<std::string> body = DAP::Framing::DecodeMessage(
        [this](void* buf, size_t size) { return ReadExact(buf, size); });
    if (!body)
      return std::nullopt;
    return DAP::Json::ParseObject(*body);
  }

private:
  bool ReadExact(void* buf, size_t size)
  {
    auto* out = static_cast<char*>(buf);
    size_t got = 0;
    while (got < size)
    {
      const ssize_t n = recv(m_fd, out + got, size - got, 0);
      if (n <= 0)
        return false;
      got += static_cast<size_t>(n);
    }
    return true;
  }

  int m_fd;
};

class DapSessionTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    auto& system = Core::System::GetInstance();
    auto& memory = system.GetMemory();
    memory.Init();
    AddressSpace::Init();

    // Register/MSR mutation asserts it runs on the CPU thread. Borrow that role
    // just for setup; the session runs on its own thread that declares itself.
    Core::DeclareAsCPUThread();
    auto& power_pc = system.GetPowerPC();
    power_pc.Reset();
    auto& ppc_state = system.GetPPCState();
    ppc_state.msr.IR = 0;
    ppc_state.msr.DR = 0;
    power_pc.MSRUpdated();
    power_pc.GetBreakPoints().Clear();

    // ori r0,r0,0 (nop) then blr, plus a data pattern for readMemory.
    const std::array<u8, 8> code{{0x60, 0x00, 0x00, 0x00, 0x4e, 0x80, 0x00, 0x20}};
    memory.CopyToEmu(CODE_ADDRESS, code.data(), code.size());
    const std::array<u8, 4> data{{0xde, 0xad, 0xbe, 0xef}};
    memory.CopyToEmu(DATA_ADDRESS, data.data(), data.size());
    Core::UndeclareAsCPUThread();

    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, m_fds), 0);
    m_server = std::thread([this] {
      // Declaring the session thread as the CPU thread keeps the controller's
      // CPUThreadGuards from trying to PauseAndLock an un-booted core.
      Core::DeclareAsCPUThread();
      DAP::DapTransport transport(m_fds[1]);
      DAP::RunSession(transport, Core::System::GetInstance());
      Core::UndeclareAsCPUThread();
    });
  }

  void TearDown() override
  {
    // Guarantee the server's blocking recv sees EOF and RunSession returns, even
    // if a test left the session mid-handshake or with an unanswered read.
    shutdown(m_fds[0], SHUT_RDWR);
    if (m_server.joinable())
      m_server.join();
    close(m_fds[0]);

    auto& system = Core::System::GetInstance();
    system.GetPowerPC().GetBreakPoints().Clear();
    AddressSpace::Shutdown();
    system.GetMemory().Shutdown();
  }

  // Runs the initialize/configurationDone handshake and consumes the entry stop.
  void Handshake(TestClient& client)
  {
    client.Send(R"({
      "seq": 1,
      "type": "request",
      "command": "initialize",
      "arguments": {}
    })");
    const auto init = client.Receive();
    ASSERT_TRUE(init.has_value());
    EXPECT_EQ(init->at("command").to_str(), "initialize");
    EXPECT_TRUE(init->at("success").get<bool>());

    client.Send(R"({
      "seq": 2,
      "type": "request",
      "command": "configurationDone"
    })");
    const auto config = client.Receive();
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->at("command").to_str(), "configurationDone");

    const auto stopped = client.Receive();
    ASSERT_TRUE(stopped.has_value());
    EXPECT_EQ(stopped->at("event").to_str(), "stopped");
    EXPECT_EQ(stopped->at("body").get<picojson::object>().at("reason").to_str(), "entry");
  }

  int m_client_fd() const { return m_fds[0]; }

  int m_fds[2] = {-1, -1};
  std::thread m_server;
};

TEST_F(DapSessionTest, InitializeAdvertisesCapabilities)
{
  TestClient client(m_client_fd());
  client.Send(R"({
    "seq": 1,
    "type": "request",
    "command": "initialize",
    "arguments": {}
  })");

  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE(response->at("success").get<bool>());
  const auto& caps =
      response->at("body").get<picojson::object>().at("capabilities").get<picojson::object>();
  EXPECT_TRUE(caps.at("supportsReadMemoryRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsWriteMemoryRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsDisassembleRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsSetVariable").get<bool>());
  EXPECT_TRUE(caps.at("supportsStackTraceRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsDataBreakpoints").get<bool>());
}

TEST_F(DapSessionTest, SetBreakpointsResolvesAgainstSourceBase)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "setBreakpoints",
    "arguments": {
      "source": {"name": "0x80003100"},
      "breakpoints": [{"line": 0}, {"line": 1}]
    }
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->at("command").to_str(), "setBreakpoints");
  const auto& breakpoints =
      response->at("body").get<picojson::object>().at("breakpoints").get<picojson::array>();
  ASSERT_EQ(breakpoints.size(), 2u);
  EXPECT_TRUE(breakpoints[0].get<picojson::object>().at("verified").get<bool>());
  EXPECT_TRUE(breakpoints[1].get<picojson::object>().at("verified").get<bool>());

  auto& bps = Core::System::GetInstance().GetPowerPC().GetBreakPoints();
  EXPECT_TRUE(bps.IsAddressBreakPoint(0x80003100));
  EXPECT_TRUE(bps.IsAddressBreakPoint(0x80003104));

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, ReadMemoryReturnsBase64OfRam)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "readMemory",
    "arguments": {"memoryReference": "0x00004000", "count": 4}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE(response->at("success").get<bool>());
  const auto& body = response->at("body").get<picojson::object>();
  const auto decoded = DAP::Json::Base64Decode(body.at("data").to_str());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, (std::vector<u8>{0xde, 0xad, 0xbe, 0xef}));

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, WriteMemoryThenReadBack)
{
  TestClient client(m_client_fd());
  Handshake(client);

  // "TWFu" == "Man" == {0x4d,0x61,0x6e}
  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "writeMemory",
    "arguments": {"memoryReference": "0x00004000", "data": "TWFu"}
  })");
  const auto write_response = client.Receive();
  ASSERT_TRUE(write_response.has_value());
  ASSERT_TRUE(write_response->at("success").get<bool>());
  EXPECT_EQ(write_response->at("body").get<picojson::object>().at("bytesWritten").get<double>(),
            3.0);

  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "readMemory",
    "arguments": {"memoryReference": "0x00004000", "count": 3}
  })");
  const auto read_response = client.Receive();
  ASSERT_TRUE(read_response.has_value());
  const auto decoded = DAP::Json::Base64Decode(
      read_response->at("body").get<picojson::object>().at("data").to_str());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, (std::vector<u8>{0x4d, 0x61, 0x6e}));

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, DisassembleReturnsInstructions)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "disassemble",
    "arguments": {"memoryReference": "0x00003100", "instructionCount": 2}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  const auto& instructions =
      response->at("body").get<picojson::object>().at("instructions").get<picojson::array>();
  ASSERT_EQ(instructions.size(), 2u);
  EXPECT_NE(instructions[0].get<picojson::object>().at("instruction").to_str().find("nop"),
            std::string::npos);
  EXPECT_NE(instructions[1].get<picojson::object>().at("instruction").to_str().find("blr"),
            std::string::npos);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, VariablesReturnsRegisters)
{
  Core::System::GetInstance().GetPPCState().gpr[3] = 0x12345678;

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "variables",
    "arguments": {"variablesReference": 1000}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  const auto& variables =
      response->at("body").get<picojson::object>().at("variables").get<picojson::array>();
  EXPECT_EQ(variables.size(), 32u);
  EXPECT_EQ(variables[0].get<picojson::object>().at("name").to_str(), "r0");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, SetVariableUpdatesRegisterAndReturnsFormattedValue)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "setVariable",
    "arguments": {
      "variablesReference": 1000,
      "name": "r3",
      "value": "0x12345678"
    }
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->at("success").get<bool>());
  EXPECT_EQ(response->at("body").get<picojson::object>().at("value").to_str(), "0x12345678");
  EXPECT_EQ(Core::System::GetInstance().GetPPCState().gpr[3], 0x12345678u);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, ThreadsAndStackTraceReturnPpcState)
{
  auto& ppc_state = Core::System::GetInstance().GetPPCState();
  ppc_state.pc = CODE_ADDRESS;
  LR(ppc_state) = CODE_ADDRESS + 4;

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "threads"
  })");
  const auto threads_response = client.Receive();
  ASSERT_TRUE(threads_response.has_value());
  const auto& threads =
      threads_response->at("body").get<picojson::object>().at("threads").get<picojson::array>();
  ASSERT_EQ(threads.size(), 1u);
  EXPECT_EQ(threads[0].get<picojson::object>().at("id").get<double>(), 1.0);

  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "stackTrace",
    "arguments": {"threadId": 1}
  })");
  const auto stack_response = client.Receive();
  ASSERT_TRUE(stack_response.has_value());
  const auto& stack_frames =
      stack_response->at("body").get<picojson::object>().at("stackFrames").get<picojson::array>();
  ASSERT_GE(stack_frames.size(), 1u);
  EXPECT_EQ(stack_frames[0].get<picojson::object>().at("instructionPointerReference").to_str(),
            "0x00003100");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, EvaluateReturnsExpressionResult)
{
  Core::System::GetInstance().GetPPCState().gpr[3] = 0x12345678;

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "evaluate",
    "arguments": {"expression": "r3"}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->at("success").get<bool>());
  EXPECT_EQ(response->at("body").get<picojson::object>().at("result").to_str(), "0x12345678");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, EvaluateInvalidExpressionFails)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "evaluate",
    "arguments": {"expression": "this is not valid"}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_FALSE(response->at("success").get<bool>());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, SetDataBreakpointsUnresolvedDataIdInstallsNothing)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "setDataBreakpoints",
    "arguments": {
      "breakpoints": [{"dataId": "not-hex", "accessType": "write"}]
    }
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->at("success").get<bool>());
  const auto& breakpoints =
      response->at("body").get<picojson::object>().at("breakpoints").get<picojson::array>();
  ASSERT_EQ(breakpoints.size(), 1u);
  EXPECT_FALSE(breakpoints[0].get<picojson::object>().at("verified").get<bool>());
  EXPECT_FALSE(Core::System::GetInstance().GetPowerPC().GetMemChecks().HasAny());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, SetDataBreakpointsInstallsWatchpoint)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "setDataBreakpoints",
    "arguments": {
      "breakpoints": [{"dataId": "0x00003100", "accessType": "write"}]
    }
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->at("success").get<bool>());
  EXPECT_NE(Core::System::GetInstance().GetPowerPC().GetMemChecks().GetMemCheck(CODE_ADDRESS),
            nullptr);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, StackTraceWithUnknownThreadFails)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "stackTrace",
    "arguments": {"threadId": 7}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_FALSE(response->at("success").get<bool>());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, StepCommandsRespondAndEmitStopped)
{
  TestClient client(m_client_fd());
  Handshake(client);

  // next, stepIn and stepOut must all be recognized (stepOut previously fell
  // through to the "unsupported" error) and each acknowledges with a response
  // followed by a stopped(step) event.
  const auto expect_step = [&](std::string_view command) {
    client.Send(
        std::string(R"({"type":"request","seq":3,"command":")").append(command).append(R"("})"));

    const auto response = client.Receive();
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->at("command").to_str(), command);
    EXPECT_TRUE(response->at("success").get<bool>());

    const auto stopped = client.Receive();
    ASSERT_TRUE(stopped.has_value());
    EXPECT_EQ(stopped->at("event").to_str(), "stopped");
    EXPECT_EQ(stopped->at("body").get<picojson::object>().at("reason").to_str(), "step");
  };

  expect_step("stepIn");
  expect_step("next");
  expect_step("stepOut");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, UnknownCommandReturnsError)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "frobnicate"
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->at("command").to_str(), "frobnicate");
  EXPECT_FALSE(response->at("success").get<bool>());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}
}  // namespace
#endif  // _WIN32

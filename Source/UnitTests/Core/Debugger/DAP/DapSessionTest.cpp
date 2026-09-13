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

#include "../DWARF/DwarfTestFixture.h"
#include "Common/CommonTypes.h"
#include "Common/SymbolDB.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapFraming.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/Debugger/DAP/DapSession.h"
#include "Core/Debugger/DAP/DapSource.h"
#include "Core/Debugger/DAP/DapTransport.h"
#include "Core/Debugger/DWARF/DwarfImport.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCSymbolDB.h"
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
    power_pc.GetSymbolDB().Clear();

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

  // Runs the initialize/launch/configurationDone handshake and consumes the
  // entry stop.
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

    const auto initialized = client.Receive();
    ASSERT_TRUE(initialized.has_value());
    EXPECT_EQ(initialized->at("event").to_str(), "initialized");

    // DESNOTE(jbarber, 2026-07-21): launch before configurationDone so the
    // entry-stop gate (which waits on both) fires the entry stop here. With
    // the deferred-policy fix, configurationDone without a prior launch would
    // no longer fire the stop immediately.
    client.Send(R"({
      "seq": 5,
      "type": "request",
      "command": "launch",
      "arguments": {}
    })");
    const auto launch_resp = client.Receive();
    ASSERT_TRUE(launch_resp.has_value());
    EXPECT_EQ(launch_resp->at("command").to_str(), "launch");

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
  const auto& caps = response->at("body").get<picojson::object>();
  EXPECT_TRUE(caps.at("supportsConfigurationDoneRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsReadMemoryRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsWriteMemoryRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsDisassembleRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsSetVariable").get<bool>());
  EXPECT_TRUE(caps.at("supportsStackTraceRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsDataBreakpoints").get<bool>());
  EXPECT_TRUE(caps.at("supportsInstructionBreakpoints").get<bool>());
  EXPECT_TRUE(caps.at("supportsGotoTargetsRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsExceptionInfoRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsLoadedSourcesRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsRestartRequest").get<bool>());
  EXPECT_TRUE(caps.at("supportsDolphinMemoryRegions").get<bool>());
  EXPECT_TRUE(caps.at("supportsDolphinMemoryScan").get<bool>());
  EXPECT_TRUE(caps.at("supportsDolphinPointerChain").get<bool>());
}

TEST_F(DapSessionTest, MemoryRegionsReportsMem1)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({"seq":20,"type":"request","command":"dolphin_memoryRegions"})");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE(response->at("success").get<bool>());
  const auto& body = response->at("body").get<picojson::object>();
  EXPECT_EQ(body.at("byteOrder").to_str(), "big");
  EXPECT_EQ(body.at("pointerSize").get<double>(), 4.0);
  const auto& regions = body.at("regions").get<picojson::array>();
  ASSERT_FALSE(regions.empty());
  const auto& mem1 = regions.front().get<picojson::object>();
  EXPECT_EQ(mem1.at("id").to_str(), "mem1");
  EXPECT_EQ(mem1.at("baseAddress").to_str(), "0x80000000");
}

TEST_F(DapSessionTest, ResolvePointerChainReturnsSteps)
{
  const std::array<u8, 4> first{{0x00, 0x00, 0x40, 0x20}};
  const std::array<u8, 4> second{{0x00, 0x00, 0x50, 0x20}};
  auto& memory = Core::System::GetInstance().GetMemory();
  memory.CopyToEmu(0x4000, first.data(), first.size());
  memory.CopyToEmu(0x4030, second.data(), second.size());

  TestClient client(m_client_fd());
  Handshake(client);
  client.Send(R"({
    "seq":20,"type":"request","command":"dolphin_resolvePointerChain",
    "arguments":{"baseAddress":"0x00004000","offsets":[16,-4]}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE(response->at("success").get<bool>());
  const auto& body = response->at("body").get<picojson::object>();
  EXPECT_EQ(body.at("finalAddress").to_str(), "0x0000501c");
  const auto& steps = body.at("steps").get<picojson::array>();
  ASSERT_EQ(steps.size(), 2u);
  const auto& first_step = steps[0].get<picojson::object>();
  EXPECT_EQ(first_step.at("address").to_str(), "0x00004000");
  EXPECT_EQ(first_step.at("pointerValue").to_str(), "0x00004020");
  EXPECT_EQ(first_step.at("offset").get<double>(), 16.0);
  EXPECT_EQ(first_step.at("resultAddress").to_str(), "0x00004030");
  const auto& second_step = steps[1].get<picojson::object>();
  EXPECT_EQ(second_step.at("offset").get<double>(), -4.0);
}

TEST_F(DapSessionTest, ResolvePointerChainReportsUnreadablePointer)
{
  TestClient client(m_client_fd());
  Handshake(client);
  client.Send(R"({
    "seq":20,"type":"request","command":"dolphin_resolvePointerChain",
    "arguments":{"baseAddress":"0x0c000000","offsets":[0]}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_FALSE(response->at("success").get<bool>());
  EXPECT_NE(response->at("message").to_str().find("cannot read pointer"), std::string::npos);
}

TEST_F(DapSessionTest, MemoryScanCompletesByEventAndReturnsResults)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq":21,"type":"request","command":"dolphin_memoryScanStart",
    "arguments":{
      "regions":["mem1"],
      "ranges":[{"start":"0x80004000","end":"0x80004004"}],
      "dataType":"u32","filter":"exact","value":"3735928559",
      "pauseDuringScan":true
    }
  })");
  const auto accepted = client.Receive();
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(accepted->at("success").get<bool>());
  const auto& accepted_body = accepted->at("body").get<picojson::object>();
  const int scan_id = static_cast<int>(accepted_body.at("scanId").get<double>());
  EXPECT_TRUE(accepted_body.at("pauseDuringScan").get<bool>());

  const auto completed = client.Receive();
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->at("event").to_str(), "dolphin_memoryScanCompleted");
  const auto& completed_body = completed->at("body").get<picojson::object>();
  EXPECT_EQ(static_cast<int>(completed_body.at("scanId").get<double>()), scan_id);
  EXPECT_EQ(completed_body.at("generation").get<double>(), 1.0);
  EXPECT_EQ(completed_body.at("resultCount").get<double>(), 1.0);

  client.Send(fmt::format(
      R"({{"seq":22,"type":"request","command":"dolphin_memoryScanResults",
           "arguments":{{"scanId":{},"start":0,"count":10}}}})",
      scan_id));
  const auto results_response = client.Receive();
  ASSERT_TRUE(results_response.has_value());
  ASSERT_TRUE(results_response->at("success").get<bool>());
  const auto& results_body = results_response->at("body").get<picojson::object>();
  EXPECT_EQ(results_body.at("totalResults").get<double>(), 1.0);
  const auto& results = results_body.at("results").get<picojson::array>();
  ASSERT_EQ(results.size(), 1u);
  const auto& result = results.front().get<picojson::object>();
  EXPECT_EQ(result.at("address").to_str(), "0x80004000");
  EXPECT_EQ(result.at("scannedValue").to_str(), "3735928559");
  EXPECT_EQ(result.at("raw").to_str(), "3q2+7w==");

  client.Send(fmt::format(
      R"({{"seq":25,"type":"request","command":"dolphin_memoryScanRemoveResults",
           "arguments":{{"scanId":{},"addresses":["0x80004000"]}}}})",
      scan_id));
  const auto removed = client.Receive();
  ASSERT_TRUE(removed.has_value());
  ASSERT_TRUE(removed->at("success").get<bool>());
  const auto& removed_body = removed->at("body").get<picojson::object>();
  EXPECT_EQ(removed_body.at("generation").get<double>(), 2.0);
  EXPECT_EQ(removed_body.at("resultCount").get<double>(), 0.0);
  EXPECT_EQ(removed_body.at("removedCount").get<double>(), 1.0);
  EXPECT_TRUE(removed_body.at("canUndo").get<bool>());

  client.Send(fmt::format(
      R"({{"seq":26,"type":"request","command":"dolphin_memoryScanUndo",
           "arguments":{{"scanId":{}}}}})",
      scan_id));
  const auto undone = client.Receive();
  ASSERT_TRUE(undone.has_value());
  ASSERT_TRUE(undone->at("success").get<bool>());
  const auto& undone_body = undone->at("body").get<picojson::object>();
  EXPECT_EQ(undone_body.at("generation").get<double>(), 1.0);
  EXPECT_EQ(undone_body.at("resultCount").get<double>(), 1.0);
  EXPECT_FALSE(undone_body.at("canUndo").get<bool>());
}

TEST_F(DapSessionTest, MemoryScanRefineNotifiesWithoutStatusPolling)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq":23,"type":"request","command":"dolphin_memoryScanStart",
    "arguments":{
      "ranges":[{"start":"0x80004000","end":"0x80004004"}],
      "dataType":"u32","filter":"unknown","pauseDuringScan":true
    }
  })");
  const auto accepted = client.Receive();
  ASSERT_TRUE(accepted.has_value());
  const int scan_id =
      static_cast<int>(accepted->at("body").get<picojson::object>().at("scanId").get<double>());
  const auto first_completed = client.Receive();
  ASSERT_TRUE(first_completed.has_value());
  EXPECT_EQ(first_completed->at("event").to_str(), "dolphin_memoryScanCompleted");

  const std::array<u8, 4> changed{{0xde, 0xad, 0xbe, 0xee}};
  Core::System::GetInstance().GetMemory().CopyToEmu(DATA_ADDRESS, changed.data(), changed.size());

  client.Send(fmt::format(
      R"({{"seq":24,"type":"request","command":"dolphin_memoryScanRefine",
           "arguments":{{"scanId":{},"filter":"changed"}}}})",
      scan_id));
  const auto refine_accepted = client.Receive();
  ASSERT_TRUE(refine_accepted.has_value());
  EXPECT_TRUE(refine_accepted->at("success").get<bool>());
  EXPECT_TRUE(
      refine_accepted->at("body").get<picojson::object>().at("pauseDuringScan").get<bool>());

  const auto refined = client.Receive();
  ASSERT_TRUE(refined.has_value());
  EXPECT_EQ(refined->at("event").to_str(), "dolphin_memoryScanCompleted");
  const auto& body = refined->at("body").get<picojson::object>();
  EXPECT_EQ(body.at("generation").get<double>(), 2.0);
  EXPECT_EQ(body.at("resultCount").get<double>(), 1.0);
}

TEST_F(DapSessionTest, MemoryScanTerminalEventPrecedesNextJobResponse)
{
  TestClient client(m_client_fd());
  Handshake(client);
  client.Send(R"({
    "seq":33,"type":"request","command":"dolphin_memoryScanStart",
    "arguments":{"ranges":[{"start":"0x80004000","end":"0x80004004"}],
      "dataType":"u32","filter":"unknown","pauseDuringScan":true}
  })");
  const auto accepted = client.Receive();
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(accepted->at("success").get<bool>());
  const int scan_id =
      static_cast<int>(accepted->at("body").get<picojson::object>().at("scanId").get<double>());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  client.Send(fmt::format(
      R"({{"seq":34,"type":"request","command":"dolphin_memoryScanRefine",
           "arguments":{{"scanId":{},"filter":"unchanged"}}}})",
      scan_id));
  const auto completed = client.Receive();
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->at("event").to_str(), "dolphin_memoryScanCompleted");
  const auto refined = client.Receive();
  ASSERT_TRUE(refined.has_value());
  EXPECT_EQ(refined->at("type").to_str(), "response");
  EXPECT_TRUE(refined->at("success").get<bool>());
}

TEST_F(DapSessionTest, MemoryScanReturnsBytePatternResult)
{
  TestClient client(m_client_fd());
  Handshake(client);
  client.Send(R"({
    "seq":27,"type":"request","command":"dolphin_memoryScanStart",
    "arguments":{"ranges":[{"start":"0x80004000","end":"0x80004004"}],
      "dataType":"bytes","filter":"exact","value":"3q2+7w==",
      "aligned":false,"pauseDuringScan":true}
  })");
  const auto accepted = client.Receive();
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(accepted->at("success").get<bool>());
  const int scan_id =
      static_cast<int>(accepted->at("body").get<picojson::object>().at("scanId").get<double>());
  const auto completed = client.Receive();
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->at("body").get<picojson::object>().at("resultCount").get<double>(), 1.0);

  client.Send(fmt::format(
      R"({{"seq":28,"type":"request","command":"dolphin_memoryScanResults",
           "arguments":{{"scanId":{},"count":10}}}})",
      scan_id));
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE(response->at("success").get<bool>());
  const auto& results =
      response->at("body").get<picojson::object>().at("results").get<picojson::array>();
  ASSERT_EQ(results.size(), 1u);
  const auto& result = results[0].get<picojson::object>();
  EXPECT_EQ(result.at("scannedValue").to_str(), "3q2+7w==");
  EXPECT_EQ(result.at("raw").to_str(), "3q2+7w==");
}

TEST_F(DapSessionTest, MemoryScanReturnsStringResult)
{
  const std::array<u8, 5> text{{'h', 'E', 'l', 'L', 'o'}};
  Core::System::GetInstance().GetMemory().CopyToEmu(DATA_ADDRESS, text.data(), text.size());
  TestClient client(m_client_fd());
  Handshake(client);
  client.Send(R"({
    "seq":29,"type":"request","command":"dolphin_memoryScanStart",
    "arguments":{"ranges":[{"start":"0x80004000","end":"0x80004005"}],
      "dataType":"string","filter":"exact","value":"Hello",
      "encoding":"ascii","caseSensitive":false,"aligned":false,
      "pauseDuringScan":true}
  })");
  const auto accepted = client.Receive();
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(accepted->at("success").get<bool>());
  const int scan_id =
      static_cast<int>(accepted->at("body").get<picojson::object>().at("scanId").get<double>());
  ASSERT_TRUE(client.Receive().has_value());
  client.Send(fmt::format(
      R"({{"seq":30,"type":"request","command":"dolphin_memoryScanResults",
           "arguments":{{"scanId":{},"count":10}}}})",
      scan_id));
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  const auto& results =
      response->at("body").get<picojson::object>().at("results").get<picojson::array>();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].get<picojson::object>().at("scannedValue").to_str(), "hElLo");
  EXPECT_EQ(results[0].get<picojson::object>().at("raw").to_str(), "aEVsTG8=");
}

TEST_F(DapSessionTest, MemoryScanReturnsPpcDisassembly)
{
  TestClient client(m_client_fd());
  Handshake(client);
  client.Send(R"({
    "seq":31,"type":"request","command":"dolphin_memoryScanStart",
    "arguments":{"ranges":[{"start":"0x80003100","end":"0x80003108"}],
      "dataType":"ppcInstruction","filter":"mnemonic","value":"nop",
      "pauseDuringScan":true}
  })");
  const auto accepted = client.Receive();
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(accepted->at("success").get<bool>());
  const int scan_id =
      static_cast<int>(accepted->at("body").get<picojson::object>().at("scanId").get<double>());
  ASSERT_TRUE(client.Receive().has_value());
  client.Send(fmt::format(
      R"({{"seq":32,"type":"request","command":"dolphin_memoryScanResults",
           "arguments":{{"scanId":{},"count":10}}}})",
      scan_id));
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  const auto& results =
      response->at("body").get<picojson::object>().at("results").get<picojson::array>();
  ASSERT_EQ(results.size(), 1u);
  const auto& result = results[0].get<picojson::object>();
  EXPECT_EQ(result.at("scannedValue").to_str(), "0x60000000");
  EXPECT_NE(result.at("disassembly").to_str().find("nop"), std::string::npos);
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
      "breakpoints": [{"line": 1}, {"line": 2}]
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

TEST_F(DapSessionTest, RealtimeWatchSubscribeAndCancelRoundTrip)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "dolphin_realtimeWatch",
    "arguments": {"memoryReference": "0x00004000", "count": 4}
  })");
  const auto sub = client.Receive();
  ASSERT_TRUE(sub.has_value());
  ASSERT_TRUE(sub->at("success").get<bool>());
  EXPECT_EQ(sub->at("command").to_str(), "dolphin_realtimeWatch");
  const auto& sub_body = sub->at("body").get<picojson::object>();
  const double watch_id = sub_body.at("watchId").get<double>();
  EXPECT_GT(watch_id, 0.0);
  EXPECT_EQ(sub_body.at("address").to_str(), "0x00004000");
  EXPECT_EQ(sub_body.at("count").get<double>(), 4.0);

  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "dolphin_realtimeWatchCancel",
    "arguments": {"watchId": 999}
  })");
  const auto cancel_miss = client.Receive();
  ASSERT_TRUE(cancel_miss.has_value());
  EXPECT_FALSE(cancel_miss->at("success").get<bool>());

  client.Send(std::string(R"({
    "seq": 5,
    "type": "request",
    "command": "dolphin_realtimeWatchCancel",
    "arguments": { "watchId": )") +
              std::to_string(static_cast<int>(watch_id)) + R"( }
  })");
  const auto cancel = client.Receive();
  ASSERT_TRUE(cancel.has_value());
  ASSERT_TRUE(cancel->at("success").get<bool>());
  EXPECT_EQ(cancel->at("command").to_str(), "dolphin_realtimeWatchCancel");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

// DESNOTE(jbarber, 2026-07-26): dolphin_freeze installs a private freeze range
// in the global memchecks store. dolphin_unfreeze must remove it. Bugbot #74.
TEST_F(DapSessionTest, FreezeAndUnfreezeRemovesMemcheck)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "dolphin_freeze",
    "arguments": {"memoryReference": "0x00004000", "count": 4,
                  "data": "AAAAAA=="}
  })");
  const auto freeze = client.Receive();
  ASSERT_TRUE(freeze.has_value());
  ASSERT_TRUE(freeze->at("success").get<bool>());
  const double watch_id = freeze->at("body").get<picojson::object>().at("watchId").get<double>();
  EXPECT_GT(watch_id, 0.0);
  // Freeze memcheck is installed.
  EXPECT_TRUE(Core::System::GetInstance().GetPowerPC().GetMemChecks().HasAny());

  client.Send(std::string(R"({
    "seq": 4,
    "type": "request",
    "command": "dolphin_unfreeze",
    "arguments": {"watchId": )") +
              std::to_string(static_cast<int>(watch_id)) + R"( }
  })");
  const auto unfreeze = client.Receive();
  ASSERT_TRUE(unfreeze.has_value());
  ASSERT_TRUE(unfreeze->at("success").get<bool>());
  // Freeze memcheck removed.
  EXPECT_FALSE(Core::System::GetInstance().GetPowerPC().GetMemChecks().HasAny());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

// DESNOTE(jbarber, 2026-07-26): Cancelling a frozen watch must also remove the
// freeze memcheck — without this, the memcheck is orphaned with no owning
// watch. Bugbot #74.
TEST_F(DapSessionTest, CancelFrozenWatchRemovesFreezeMemcheck)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "dolphin_freeze",
    "arguments": {"memoryReference": "0x00004000", "count": 4,
                  "data": "AAAAAA=="}
  })");
  const auto freeze = client.Receive();
  ASSERT_TRUE(freeze.has_value());
  ASSERT_TRUE(freeze->at("success").get<bool>());
  const double watch_id = freeze->at("body").get<picojson::object>().at("watchId").get<double>();
  EXPECT_TRUE(Core::System::GetInstance().GetPowerPC().GetMemChecks().HasAny());

  // Cancel the frozen watch — should remove the freeze memcheck.
  client.Send(std::string(R"({
    "seq": 4,
    "type": "request",
    "command": "dolphin_realtimeWatchCancel",
    "arguments": {"watchId": )") +
              std::to_string(static_cast<int>(watch_id)) + R"( }
  })");
  const auto cancel = client.Receive();
  ASSERT_TRUE(cancel.has_value());
  ASSERT_TRUE(cancel->at("success").get<bool>());
  EXPECT_FALSE(Core::System::GetInstance().GetPowerPC().GetMemChecks().HasAny());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

// DESNOTE(jbarber, 2026-07-26): Re-freezing the same watch must not leak a
// memcheck entry — the old freeze must be removed before installing the new.
// Bugbot #76.
TEST_F(DapSessionTest, ReFreezeSameWatchDoesNotLeakMemcheck)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "dolphin_freeze",
    "arguments": {"memoryReference": "0x00004000", "count": 4,
                  "data": "AAAAAA=="}
  })");
  const auto freeze1 = client.Receive();
  ASSERT_TRUE(freeze1.has_value());
  ASSERT_TRUE(freeze1->at("success").get<bool>());
  const double watch_id = freeze1->at("body").get<picojson::object>().at("watchId").get<double>();

  // Re-freeze with a different value — should not leak the old memcheck.
  client.Send(std::string(R"({
    "seq": 4,
    "type": "request",
    "command": "dolphin_freeze",
    "arguments": {"watchId": )") +
              std::to_string(static_cast<int>(watch_id)) +
              R"(, "data": "BBBBBB=="} 
  })");
  const auto freeze2 = client.Receive();
  ASSERT_TRUE(freeze2.has_value());
  ASSERT_TRUE(freeze2->at("success").get<bool>());

  // Unfreeze — should succeed and leave no memcheck behind. If the old
  // freeze leaked, there'd be a stale entry after unfreeze.
  client.Send(std::string(R"({
    "seq": 5,
    "type": "request",
    "command": "dolphin_unfreeze",
    "arguments": {"watchId": )") +
              std::to_string(static_cast<int>(watch_id)) + R"( }
  })");
  const auto unfreeze = client.Receive();
  ASSERT_TRUE(unfreeze.has_value());
  ASSERT_TRUE(unfreeze->at("success").get<bool>());
  EXPECT_FALSE(Core::System::GetInstance().GetPowerPC().GetMemChecks().HasAny());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, SetDataBreakpointsPreservesFreezeAndSampler)
{
  TestClient client(m_client_fd());
  Handshake(client);

  // Install a freeze.
  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "dolphin_freeze",
    "arguments": {"memoryReference": "0x00004000", "count": 4,
                  "data": "AAAAAA=="}
  })");
  const auto freeze = client.Receive();
  ASSERT_TRUE(freeze.has_value());
  ASSERT_TRUE(freeze->at("success").get<bool>());
  EXPECT_TRUE(Core::System::GetInstance().GetPowerPC().GetMemChecks().HasAny());

  // Replacing watchpoints must not mutate this session's independent freezes.
  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "setDataBreakpoints",
    "arguments": {
      "breakpoints": [{"dataId": "0x00005000", "accessType": "write"}]
    }
  })");
  const auto dbp = client.Receive();
  ASSERT_TRUE(dbp.has_value());
  ASSERT_TRUE(dbp->at("success").get<bool>());
  // The data breakpoint should be installed.
  EXPECT_NE(Core::System::GetInstance().GetPowerPC().GetMemChecks().GetMemCheck(0x00005000),
            nullptr);
  EXPECT_EQ(Core::System::GetInstance().GetPowerPC().GetMemChecks().GetMemCheck(0x00004000),
            nullptr);
  EXPECT_TRUE(
      Core::System::GetInstance().GetPowerPC().GetMemChecks().GetSnapshot()->OverlapsPrivateFreeze(
          0x00004000, 4));

  // Unfreeze still owns and removes the preserved freeze.
  const double watch_id = freeze->at("body").get<picojson::object>().at("watchId").get<double>();
  client.Send(std::string(R"({
    "seq": 5,
    "type": "request",
    "command": "dolphin_unfreeze",
    "arguments": {"watchId": )") +
              std::to_string(static_cast<int>(watch_id)) + R"( }
  })");
  const auto unfreeze = client.Receive();
  ASSERT_TRUE(unfreeze.has_value());
  ASSERT_TRUE(unfreeze->at("success").get<bool>());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

// DESNOTE(jbarber, 2026-07-26): Disconnecting a session with an active freeze
// must remove the freeze memcheck via the session's Run() teardown path
// (ClearFreezes for last session, per-session RemoveFreeze for non-last).
// We can't check this in the test body (races with the session thread
// teardown), but we verify the freeze is present while alive and that
// unfreeze removes it (covered by FreezeAndUnfreezeRemovesMemcheck). The
// Run() teardown is straightforward: last session calls ClearBreakpoints
// (wipes all memchecks) + ClearFreezes; non-last iterates m_watch_to_freeze
// and calls RemoveFreeze per entry. Bugbot #75.

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

TEST_F(DapSessionTest, FrameZeroScopesExposeExpandableDwarfVariablesAndExpireHandles)
{
  auto& system = Core::System::GetInstance();
  system.GetPPCSymbolDB().SetDwarfDebugInfo(DwarfTestFixture::MakeTypedParseResult());
  system.GetPPCState().pc = DwarfTestFixture::kFunctionAddress;
  const std::array<u8, 8> point{{0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x40, 0x00}};
  system.GetMemory().CopyToEmu(DwarfTestFixture::kTypedDataAddress, point.data(), point.size());

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 10, "type": "request", "command": "scopes", "arguments": {"frameId": 0}
  })");
  const auto scopes_response = client.Receive();
  ASSERT_TRUE(scopes_response);
  const auto& scopes =
      scopes_response->at("body").get<picojson::object>().at("scopes").get<picojson::array>();
  ASSERT_EQ(scopes.size(), 4U);
  EXPECT_EQ(scopes[2].get<picojson::object>().at("name").to_str(), "Locals");
  EXPECT_EQ(scopes[3].get<picojson::object>().at("name").to_str(), "Globals");
  EXPECT_FALSE(scopes[2].get<picojson::object>().at("expensive").get<bool>());
  EXPECT_FALSE(scopes[3].get<picojson::object>().at("expensive").get<bool>());

  client.Send(R"({
    "seq": 11, "type": "request", "command": "variables",
    "arguments": {"variablesReference": 1003}
  })");
  const auto globals_response = client.Receive();
  ASSERT_TRUE(globals_response);
  const auto& globals =
      globals_response->at("body").get<picojson::object>().at("variables").get<picojson::array>();
  ASSERT_EQ(globals.size(), 2U);
  const auto& global = globals[0].get<picojson::object>();
  EXPECT_EQ(global.at("name").to_str(), "global_point");
  const int children_reference = static_cast<int>(global.at("variablesReference").get<double>());
  EXPECT_GE(children_reference, 0x10000);

  client.Send(fmt::format(
      R"({{"seq":12,"type":"request","command":"variables","arguments":{{"variablesReference":{}}}}})",
      children_reference));
  const auto children_response = client.Receive();
  ASSERT_TRUE(children_response);
  const auto& children =
      children_response->at("body").get<picojson::object>().at("variables").get<picojson::array>();
  ASSERT_EQ(children.size(), 2U);
  EXPECT_EQ(children[0].get<picojson::object>().at("value").to_str(), "0x11223344");

  client.Send(R"({
    "seq": 13, "type": "request", "command": "scopes", "arguments": {"frameId": 1}
  })");
  const auto non_top_scopes_response = client.Receive();
  ASSERT_TRUE(non_top_scopes_response);
  EXPECT_EQ(non_top_scopes_response->at("body")
                .get<picojson::object>()
                .at("scopes")
                .get<picojson::array>()
                .size(),
            2U);

  client.Send(fmt::format(
      R"({{"seq":14,"type":"request","command":"variables","arguments":{{"variablesReference":{}}}}})",
      children_reference));
  const auto stale_response = client.Receive();
  ASSERT_TRUE(stale_response);
  EXPECT_FALSE(stale_response->at("success").get<bool>());

  client.Send(R"({
    "seq": 15, "type": "request", "command": "scopes", "arguments": {"frameId": 0}
  })");
  ASSERT_TRUE(client.Receive());
  client.Send(R"({
    "seq": 16, "type": "request", "command": "variables",
    "arguments": {"variablesReference": 1003}
  })");
  const auto refreshed_globals_response = client.Receive();
  ASSERT_TRUE(refreshed_globals_response);
  const auto& refreshed_globals = refreshed_globals_response->at("body")
                                      .get<picojson::object>()
                                      .at("variables")
                                      .get<picojson::array>();
  ASSERT_FALSE(refreshed_globals.empty());
  const int refreshed_reference = static_cast<int>(
      refreshed_globals[0].get<picojson::object>().at("variablesReference").get<double>());
  EXPECT_NE(refreshed_reference, children_reference);

  client.Send(fmt::format(
      R"({{"seq":17,"type":"request","command":"variables","arguments":{{"variablesReference":{}}}}})",
      children_reference));
  const auto aliased_stale_response = client.Receive();
  ASSERT_TRUE(aliased_stale_response);
  EXPECT_FALSE(aliased_stale_response->at("success").get<bool>());

  client.Send(R"({"seq": 18, "type": "request", "command": "disconnect"})");
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
  {
    Core::CPUThreadGuard guard(Core::System::GetInstance());
    Core::System::GetInstance().GetPowerPC().GetSymbolDB().AddKnownSymbol(
        guard, CODE_ADDRESS, 0x100, "main", "game.elf", Common::Symbol::Type::Function);
  }
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
  const auto& stack_frame = stack_frames[0].get<picojson::object>();
  EXPECT_EQ(stack_frame.at("instructionPointerReference").to_str(), "0x00003100");
  EXPECT_EQ(stack_frame.at("source").get<picojson::object>().at("sourceReference").get<double>(),
            static_cast<double>(DAP::MakeDisassemblySourceReference(CODE_ADDRESS)));
  EXPECT_EQ(stack_frame.at("line").get<double>(), 1.0);
  EXPECT_EQ(stack_frame.at("column").get<double>(), 1.0);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, UnmappedHighAddressStackFrameOmitsSource)
{
  constexpr u32 address = 0x800052cc;
  auto& ppc_state = Core::System::GetInstance().GetPPCState();
  ppc_state.pc = address;
  ppc_state.gpr[1] = 0;
  LR(ppc_state) = 0;

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "stackTrace",
    "arguments": {"threadId": 1}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  const auto& frames =
      response->at("body").get<picojson::object>().at("stackFrames").get<picojson::array>();
  ASSERT_EQ(frames.size(), 1u);
  const auto& frame = frames[0].get<picojson::object>();
  EXPECT_EQ(frame.count("source"), 0u);
  EXPECT_EQ(frame.at("line").get<double>(), 1.0);
  EXPECT_EQ(frame.at("column").get<double>(), 1.0);

  client.Send(R"({"seq":9,"type":"request","command":"disconnect"})");
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

TEST_F(DapSessionTest, SetDataBreakpointsRangedLengthInstallsRangedWatchpoint)
{
  TestClient client(m_client_fd());
  Handshake(client);

  // `length` is a Dolphin extension to the standard DAP data-breakpoint args.
  // When present and > 1 the controller installs a ranged PPC memcheck over
  // [address, address+length-1].
  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "setDataBreakpoints",
    "arguments": {
      "breakpoints": [{"dataId": "0x00003100", "accessType": "write", "length": 256}]
    }
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->at("success").get<bool>());

  const auto check =
      Core::System::GetInstance().GetPowerPC().GetMemChecks().GetMemCheck(CODE_ADDRESS);
  ASSERT_NE(check, nullptr);
  EXPECT_TRUE(check->is_ranged);
  EXPECT_EQ(check->start_address, CODE_ADDRESS);
  EXPECT_EQ(check->end_address, CODE_ADDRESS + 256u - 1u);

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

TEST_F(DapSessionTest, SetInstructionBreakpointsInstallsCodeBreakpoint)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "setInstructionBreakpoints",
    "arguments": {
      "breakpoints": [{"instructionReference": "0x80003100"}]
    }
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->at("command").to_str(), "setInstructionBreakpoints");
  const auto& breakpoints =
      response->at("body").get<picojson::object>().at("breakpoints").get<picojson::array>();
  ASSERT_EQ(breakpoints.size(), 1u);
  EXPECT_TRUE(breakpoints[0].get<picojson::object>().at("verified").get<bool>());

  EXPECT_TRUE(
      Core::System::GetInstance().GetPowerPC().GetBreakPoints().IsAddressBreakPoint(0x80003100));

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, SetInstructionBreakpointsReplacesPreviousBreakpoints)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "setInstructionBreakpoints",
    "arguments": {"breakpoints": [{"instructionReference": "0x80003100"}]}
  })");
  (void)client.Receive();

  // An authoritative second set with a different address must drop the first.
  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "setInstructionBreakpoints",
    "arguments": {"breakpoints": [{"instructionReference": "0x80003200"}]}
  })");
  (void)client.Receive();

  auto& bps = Core::System::GetInstance().GetPowerPC().GetBreakPoints();
  EXPECT_FALSE(bps.IsAddressBreakPoint(0x80003100));
  EXPECT_TRUE(bps.IsAddressBreakPoint(0x80003200));

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, FailedBreakpointRequestDoesNotConsumeIds)
{
  TestClient client(m_client_fd());
  Handshake(client);

  auto& breakpoints = Core::System::GetInstance().GetPowerPC().GetBreakPoints();
  ASSERT_TRUE(breakpoints.Add(0x80003100, false, true, Expression::TryParse("r3 == 1")));
  const auto gui_event = client.Receive();
  ASSERT_TRUE(gui_event.has_value());
  EXPECT_EQ(gui_event->at("event").to_str(), "breakpoint");

  client.Send(R"({
    "seq":3,"type":"request","command":"setBreakpoints",
    "arguments":{
      "source":{"name":"0x80003100"},
      "breakpoints":[{"line":1,"condition":"r3 == 1"}]
    }
  })");
  const auto source_response = client.Receive();
  ASSERT_TRUE(source_response.has_value());
  const auto& source_breakpoints =
      source_response->at("body").get<picojson::object>().at("breakpoints").get<picojson::array>();
  ASSERT_EQ(source_breakpoints.size(), 1u);
  EXPECT_EQ(source_breakpoints[0].get<picojson::object>().at("id").get<double>(), 1.0);

  client.Send(R"({
    "seq":4,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[
      {"instructionReference":"0x80003300"},
      {"instructionReference":"0x80003100","condition":"r3 == 2"}
    ]}
  })");
  const auto failed = client.Receive();
  ASSERT_TRUE(failed.has_value());
  EXPECT_FALSE(failed->at("success").get<bool>());

  client.Send(R"({
    "seq":5,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[{"instructionReference":"0x80003400"}]}
  })");
  const auto instruction_response = client.Receive();
  ASSERT_TRUE(instruction_response.has_value());
  const auto& instruction_breakpoints = instruction_response->at("body")
                                            .get<picojson::object>()
                                            .at("breakpoints")
                                            .get<picojson::array>();
  ASSERT_EQ(instruction_breakpoints.size(), 1u);
  EXPECT_EQ(instruction_breakpoints[0].get<picojson::object>().at("id").get<double>(), 2.0);

  client.Send(R"({"seq":6,"type":"request","command":"disconnect"})");
  const auto disconnect = client.Receive();
  ASSERT_TRUE(disconnect.has_value());
  EXPECT_EQ(disconnect->at("command").to_str(), "disconnect");
}

TEST_F(DapSessionTest, FirstSessionPreservesPersistedBreakpoints)
{
  auto& breakpoints = Core::System::GetInstance().GetPowerPC().GetBreakPoints();
  (void)breakpoints.Add(0x80003100);
  ASSERT_TRUE(breakpoints.IsAddressBreakPoint(0x80003100));

  TestClient client(m_client_fd());
  Handshake(client);

  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(0x80003100));

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, GotoTargetsWithoutSourceReturnsNoTargets)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "gotoTargets",
    "arguments": {"line": 1}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE(response->at("success").get<bool>());
  const auto& targets =
      response->at("body").get<picojson::object>().at("targets").get<picojson::array>();
  EXPECT_TRUE(targets.empty());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, GotoTargetsThenGotoMovesPc)
{
  auto& ppc_state = Core::System::GetInstance().GetPPCState();
  ppc_state.pc = CODE_ADDRESS;

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "gotoTargets",
    "arguments": {"source": {"name": "0x00003100"}, "line": 2}
  })");
  const auto targets_response = client.Receive();
  ASSERT_TRUE(targets_response.has_value());
  const auto& targets =
      targets_response->at("body").get<picojson::object>().at("targets").get<picojson::array>();
  ASSERT_EQ(targets.size(), 1u);
  const double target_id = targets[0].get<picojson::object>().at("id").get<double>();
  EXPECT_EQ(target_id, static_cast<double>(CODE_ADDRESS + 4));

  client.Send(std::string(R"({"type":"request","seq":4,"command":"goto","arguments":)")
                  .append(R"({"threadId":1,"targetId":)")
                  .append(std::to_string(CODE_ADDRESS + 4))
                  .append("}}"));
  const auto goto_response = client.Receive();
  ASSERT_TRUE(goto_response.has_value());
  EXPECT_EQ(goto_response->at("command").to_str(), "goto");
  EXPECT_TRUE(goto_response->at("success").get<bool>());

  const auto stopped = client.Receive();
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->at("event").to_str(), "stopped");
  EXPECT_EQ(stopped->at("body").get<picojson::object>().at("reason").to_str(), "goto");
  EXPECT_EQ(Core::System::GetInstance().GetPPCState().pc, CODE_ADDRESS + 4u);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, ExceptionInfoReportsPendingException)
{
  Core::System::GetInstance().GetPPCState().Exceptions = EXCEPTION_PROGRAM;

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "exceptionInfo",
    "arguments": {"threadId": 1}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE(response->at("success").get<bool>());
  const auto& body = response->at("body").get<picojson::object>();
  EXPECT_NE(body.at("description").to_str().find("Program"), std::string::npos);

  Core::System::GetInstance().GetPPCState().Exceptions = 0;

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, ExceptionInfoWithoutExceptionFails)
{
  Core::System::GetInstance().GetPPCState().Exceptions = 0;

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "exceptionInfo",
    "arguments": {"threadId": 1}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->at("command").to_str(), "exceptionInfo");
  EXPECT_FALSE(response->at("success").get<bool>());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, LoadedSourcesReturnsFunctionSymbol)
{
  {
    Core::CPUThreadGuard guard(Core::System::GetInstance());
    Core::System::GetInstance().GetPowerPC().GetSymbolDB().AddKnownSymbol(
        guard, CODE_ADDRESS, 0x100, "main", "game.elf", Common::Symbol::Type::Function);
  }

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "loadedSources",
    "arguments": {}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  const auto& sources =
      response->at("body").get<picojson::object>().at("sources").get<picojson::array>();
  ASSERT_EQ(sources.size(), 1u);
  const auto& source = sources[0].get<picojson::object>();
  EXPECT_EQ(source.at("name").to_str(), "game.elf");
  EXPECT_EQ(source.at("sourceReference").get<double>(),
            static_cast<double>(DAP::MakeDisassemblySourceReference(CODE_ADDRESS)));
  EXPECT_EQ(source.count("path"), 0u);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, SourceReturnsDisassembly)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "source",
    "arguments": {
      "source": {"name": "0x00003100", "sourceReference": 4294979840},
      "startLine": 0,
      "endLine": 0
    }
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE(response->at("success").get<bool>());
  const auto& body = response->at("body").get<picojson::object>();
  EXPECT_EQ(body.at("mimeType").to_str(), "text/x-disassembly");
  EXPECT_NE(body.at("content").to_str().find("nop"), std::string::npos);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, BreakpointLocationsReturnsInstructionLines)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "breakpointLocations",
    "arguments": {
      "source": {"name": "0x00003100"},
      "line": 1,
      "endLine": 2
    }
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  const auto& breakpoints =
      response->at("body").get<picojson::object>().at("breakpoints").get<picojson::array>();
  ASSERT_EQ(breakpoints.size(), 2u);
  EXPECT_EQ(breakpoints[0].get<picojson::object>().at("line").get<double>(), 1.0);
  EXPECT_EQ(breakpoints[1].get<picojson::object>().at("line").get<double>(), 2.0);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, LoadedSourcesReturnsDwarfFileAfterImport)
{
  {
    Core::CPUThreadGuard guard(Core::System::GetInstance());
    ASSERT_TRUE(
        Core::Debug::ImportDwarf(guard, Core::System::GetInstance().GetPowerPC().GetSymbolDB(),
                                 DwarfTestFixture::kDebugSection, DwarfTestFixture::kLineSection));
  }

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "loadedSources",
    "arguments": {}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  const auto& sources =
      response->at("body").get<picojson::object>().at("sources").get<picojson::array>();
  ASSERT_EQ(sources.size(), 1u);
  const auto& source = sources[0].get<picojson::object>();
  EXPECT_EQ(source.at("path").to_str(), DwarfTestFixture::kCompileUnitName);
  EXPECT_EQ(source.count("sourceReference"), 0u);
  EXPECT_EQ(source.at("adapterData").get<picojson::object>().at("dolphinSourceId").get<double>(),
            1.0);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, BreakpointLocationsWithDwarfSourceReference)
{
  {
    Core::CPUThreadGuard guard(Core::System::GetInstance());
    ASSERT_TRUE(
        Core::Debug::ImportDwarf(guard, Core::System::GetInstance().GetPowerPC().GetSymbolDB(),
                                 DwarfTestFixture::kDebugSection, DwarfTestFixture::kLineSection));
  }

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "breakpointLocations",
    "arguments": {
      "source": {"sourceReference": 1},
      "line": 1,
      "endLine": 2
    }
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->at("success").get<bool>());
  const auto& breakpoints =
      response->at("body").get<picojson::object>().at("breakpoints").get<picojson::array>();
  ASSERT_EQ(breakpoints.size(), 2u);
  EXPECT_EQ(breakpoints[0].get<picojson::object>().at("line").get<double>(), 1.0);
  EXPECT_EQ(breakpoints[1].get<picojson::object>().at("line").get<double>(), 2.0);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, SetBreakpointsWithDwarfAdapterData)
{
  {
    Core::CPUThreadGuard guard(Core::System::GetInstance());
    ASSERT_TRUE(
        Core::Debug::ImportDwarf(guard, Core::System::GetInstance().GetPowerPC().GetSymbolDB(),
                                 DwarfTestFixture::kDebugSection, DwarfTestFixture::kLineSection));
  }

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "setBreakpoints",
    "arguments": {
      "source": {"path": "/workspace/test.c", "adapterData": {"dolphinSourceId": 1}},
      "breakpoints": [{"line": 2}]
    }
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->at("success").get<bool>());
  const auto& breakpoints =
      response->at("body").get<picojson::object>().at("breakpoints").get<picojson::array>();
  ASSERT_EQ(breakpoints.size(), 1u);
  EXPECT_TRUE(breakpoints[0].get<picojson::object>().at("verified").get<bool>());

  auto& bps = Core::System::GetInstance().GetPowerPC().GetBreakPoints();
  EXPECT_TRUE(bps.IsAddressBreakPoint(DwarfTestFixture::kLineTwoAddress));

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, StackTraceWithDwarfSourceLine)
{
  {
    Core::CPUThreadGuard guard(Core::System::GetInstance());
    ASSERT_TRUE(
        Core::Debug::ImportDwarf(guard, Core::System::GetInstance().GetPowerPC().GetSymbolDB(),
                                 DwarfTestFixture::kDebugSection, DwarfTestFixture::kLineSection));
    Core::System::GetInstance().GetPPCState().pc = DwarfTestFixture::kLineTwoAddress;
    LR(Core::System::GetInstance().GetPPCState()) = 0;
  }

  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "stackTrace",
    "arguments": {"threadId": 1}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  const auto& frames =
      response->at("body").get<picojson::object>().at("stackFrames").get<picojson::array>();
  ASSERT_EQ(frames.size(), 1u);
  const auto& frame = frames[0].get<picojson::object>();
  EXPECT_EQ(frame.at("line").get<double>(), 2.0);
  EXPECT_EQ(frame.at("source").get<picojson::object>().at("path").to_str(),
            DwarfTestFixture::kCompileUnitName);
  EXPECT_EQ(frame.at("source")
                .get<picojson::object>()
                .at("adapterData")
                .get<picojson::object>()
                .at("dolphinSourceId")
                .get<double>(),
            1.0);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, RestartEmitsStopped)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "restart",
    "arguments": {}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->at("command").to_str(), "restart");

  const auto stopped = client.Receive();
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->at("event").to_str(), "stopped");
  EXPECT_EQ(stopped->at("body").get<picojson::object>().at("reason").to_str(), "restart");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, TerminateEmitsTerminated)
{
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "terminate",
    "arguments": {}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->at("command").to_str(), "terminate");

  const auto terminated = client.Receive();
  ASSERT_TRUE(terminated.has_value());
  EXPECT_EQ(terminated->at("event").to_str(), "terminated");

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

TEST_F(DapSessionTest, LaunchStopOnEntryFalseContinuesInsteadOfStopping)
{
  TestClient client(m_client_fd());

  // initialize handshake (no configurationDone yet)
  client.Send(R"({
    "seq": 1,
    "type": "request",
    "command": "initialize",
    "arguments": {}
  })");
  ASSERT_TRUE(client.Receive().has_value());  // initialize response
  ASSERT_TRUE(client.Receive().has_value());  // initialized event

  // launch with stopOnEntry: false -> the core should be told to continue
  // rather than emit a "stopped"/"entry" event. In the un-booted test harness
  // Core::SetState is a no-op (s_state != Running), so the state hook doesn't
  // fire a "continued" -- what we can assert is that NO "stopped"/"entry"
  // arrives. We prove that by sending a follow-up request and confirming its
  // response is the first thing we receive (no stale stopped event queued
  // ahead of it, unlike the stopOnEntry:true case).
  client.Send(R"({
    "seq": 2,
    "type": "request",
    "command": "launch",
    "arguments": {"stopOnEntry": false}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->at("command").to_str(), "launch");
  EXPECT_TRUE(response->at("success").get<bool>());

  // DESNOTE(jbarber, 2026-07-21): entry-stop is gated on configurationDone;
  // send it here so MaybeFireEntryStop runs. With stopOnEntry:false it should
  // NOT emit a stopped/entry event (the controller is told to Continue).
  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "configurationDone"
  })");
  ASSERT_TRUE(client.Receive().has_value());  // configurationDone response

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "threads"
  })");
  const auto next = client.Receive();
  ASSERT_TRUE(next.has_value());
  // If a "stopped"/"entry" had been queued, it would arrive here instead.
  EXPECT_EQ(next->at("type").to_str(), "response");
  EXPECT_EQ(next->at("command").to_str(), "threads");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, LaunchStopOnEntryTrueEmitsStoppedEntry)
{
  TestClient client(m_client_fd());

  client.Send(R"({
    "seq": 1,
    "type": "request",
    "command": "initialize",
    "arguments": {}
  })");
  ASSERT_TRUE(client.Receive().has_value());
  ASSERT_TRUE(client.Receive().has_value());

  client.Send(R"({
    "seq": 2,
    "type": "request",
    "command": "launch",
    "arguments": {"stopOnEntry": true}
  })");
  const auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->at("success").get<bool>());

  // Entry-stop gate waits for configurationDone before firing.
  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "configurationDone"
  })");
  ASSERT_TRUE(client.Receive().has_value());  // configurationDone response

  const auto stopped = client.Receive();
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->at("event").to_str(), "stopped");
  EXPECT_EQ(stopped->at("body").get<picojson::object>().at("reason").to_str(), "entry");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, LaunchDefaultsToStoppedEntry)
{
  // Omitting stopOnEntry must preserve the historical always-paused behavior.
  TestClient client(m_client_fd());

  client.Send(R"({
    "seq": 1,
    "type": "request",
    "command": "initialize",
    "arguments": {}
  })");
  ASSERT_TRUE(client.Receive().has_value());
  ASSERT_TRUE(client.Receive().has_value());

  client.Send(R"({
    "seq": 2,
    "type": "request",
    "command": "launch",
    "arguments": {}
  })");
  ASSERT_TRUE(client.Receive().has_value());

  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "configurationDone"
  })");
  ASSERT_TRUE(client.Receive().has_value());  // configurationDone response

  const auto stopped = client.Receive();
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->at("event").to_str(), "stopped");
  EXPECT_EQ(stopped->at("body").get<picojson::object>().at("reason").to_str(), "entry");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, ConfigurationDoneBeforeLaunchDoesNotDuplicateEntryStop)
{
  // Regression: a client that sends configurationDone before launch/attach used
  // to emit a `stopped`/entry from configurationDone AND a second one from
  // launch. The entry-stop is now deferred until BOTH configurationDone and
  // launch/attach are seen, so it fires exactly once on the second of the two
  // (here: launch), regardless of the ordering.
  TestClient client(m_client_fd());

  client.Send(R"({
    "seq": 1,
    "type": "request",
    "command": "initialize",
    "arguments": {}
  })");
  ASSERT_TRUE(client.Receive().has_value());  // initialize response
  ASSERT_TRUE(client.Receive().has_value());  // initialized event

  // configurationDone before launch — under the deferred-entry-stop policy this
  // does NOT yet fire the entry stop; we wait for launch so any `stopOnEntry`
  // override on launch is honored.
  client.Send(R"({
    "seq": 2,
    "type": "request",
    "command": "configurationDone"
  })");
  ASSERT_TRUE(client.Receive().has_value());  // configurationDone response

  // launch is the second gate; with no stopOnEntry override, default-true fires
  // the entry stop exactly once.
  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "launch",
    "arguments": {}
  })");
  ASSERT_TRUE(client.Receive().has_value());  // launch response
  const auto first_stop = client.Receive();
  ASSERT_TRUE(first_stop.has_value());
  EXPECT_EQ(first_stop->at("event").to_str(), "stopped");
  EXPECT_EQ(first_stop->at("body").get<picojson::object>().at("reason").to_str(), "entry");

  // A probe request (threads) must not see a stale second stop queued ahead of
  // its response.
  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "threads"
  })");
  const auto next = client.Receive();
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next->at("type").to_str(), "response");
  EXPECT_EQ(next->at("command").to_str(), "threads");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, ConfigurationDoneBeforeLaunchHonorsLaunchStopOnEntryFalse)
{
  // Regression: Bugbot flagged that configurationDone before launch used to
  // commit the entry-stop policy immediately, silently dropping a later
  // `launch` `stopOnEntry:false` override. With the deferred gate, launch's
  // override is applied before MaybeFireEntryStop fires, so the override
  // takes effect: no `stopped` event is emitted and the core is told to
  // continue.
  TestClient client(m_client_fd());

  client.Send(R"({
    "seq": 1,
    "type": "request",
    "command": "initialize",
    "arguments": {}
  })");
  ASSERT_TRUE(client.Receive().has_value());  // initialize response
  ASSERT_TRUE(client.Receive().has_value());  // initialized event

  client.Send(R"({
    "seq": 2,
    "type": "request",
    "command": "configurationDone"
  })");
  ASSERT_TRUE(client.Receive().has_value());  // configurationDone response

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "launch",
    "arguments": {"stopOnEntry": false}
  })");
  ASSERT_TRUE(client.Receive().has_value());  // launch response

  // No `stopped` event should follow; the only thing queued is the core
  // state hook's `continued` (the controller was told to Continue). Verify by
  // probing with threads and asserting the next received message is its
  // response (or the continued event), but NOT a stopped/entry event.
  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "threads"
  })");
  // Drain any continued event from the controller.Continue() path.
  while (true)
  {
    const auto msg = client.Receive();
    ASSERT_TRUE(msg.has_value());
    if (msg->at("type").to_str() == "response")
    {
      EXPECT_EQ(msg->at("command").to_str(), "threads");
      break;
    }
    ASSERT_EQ(msg->at("type").to_str(), "event");
    EXPECT_NE(msg->at("event").to_str(), "stopped");
  }

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

// ---------------------------------------------------------------------------
// Code injection + detour (Dolphin extensions).
// ---------------------------------------------------------------------------

TEST_F(DapSessionTest, InitializeAdvertisesInjectionAndDetourCapabilities)
{
  // DESNOTE(jbarber, 2026-07-21): Mirror the bare-`initialize` pattern of
  // InitializeAdvertisesCapabilities above (no Handshake). The session
  // responds with its capability set; we only assert the Dolphin-specific
  // injection/detour/findFreeMemory flags are present and true.
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
  const auto& caps = response->at("body").get<picojson::object>();
  EXPECT_TRUE(caps.at("supportsDolphinFindFreeMemory").get<bool>());
  EXPECT_TRUE(caps.at("supportsDolphinInjectCode").get<bool>());
  EXPECT_TRUE(caps.at("supportsDolphinDetour").get<bool>());
}

TEST_F(DapSessionTest, InjectCodeAtExplicitAddressWritesBytes)
{
  // DESNOTE(jbarber, 2026-07-21): InjectCode at a bare physical address works
  // in DR=0 test mode. "AAAAAQ==" base64-decodes to {0x00,0x00,0x00,0x01},
  // a 4-byte PPC instruction word aligned to the instruction boundary.
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "dolphin_injectCode",
    "arguments": {"memoryReference": "0x00009000", "code": "AAAAAQ=="}
  })");
  const auto resp = client.Receive();
  ASSERT_TRUE(resp.has_value());
  EXPECT_EQ(resp->at("command").to_str(), "dolphin_injectCode");
  ASSERT_TRUE(resp->at("success").get<bool>());
  const auto& body = resp->at("body").get<picojson::object>();
  EXPECT_EQ(body.at("address").to_str(), "0x00009000");
  EXPECT_EQ(body.at("count").get<double>(), 4.0);

  // Verify the bytes landed in RAM by reading them back via DAP readMemory.
  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "readMemory",
    "arguments": {"memoryReference": "0x00009000", "count": 4}
  })");
  const auto read_resp = client.Receive();
  ASSERT_TRUE(read_resp.has_value());
  ASSERT_TRUE(read_resp->at("success").get<bool>());
  const std::string data_str = read_resp->at("body").get<picojson::object>().at("data").to_str();
  EXPECT_EQ(data_str, "AAAAAQ==");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, InjectCodeRejectsNonMultipleOfFourBytes)
{
  TestClient client(m_client_fd());
  Handshake(client);

  // "AAE=" decodes to a single byte -- not a 4-byte instruction word.
  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "dolphin_injectCode",
    "arguments": {"memoryReference": "0x00009000", "code": "AAE="}
  })");
  const auto resp = client.Receive();
  ASSERT_TRUE(resp.has_value());
  EXPECT_FALSE(resp->at("success").get<bool>());

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, DetourPatchesTargetAndReturnsAddresses)
{
  // DESNOTE(jbarber, 2026-07-21): End-to-end detour at the protocol surface.
  // Byte-level correctness (target patched with `b detour`, trampoline holding
  // original + `b target+4`, detour body + appended tail branch) is exercised
  // in DapControllerTest.DetourPatchesTargetAndInstallsTrampoline. Here we
  // only verify the protocol surface: the response reports the resolved
  // target/detour/trampoline addresses and originalInstruction is non-empty
  // base64 of the 4 patched-out bytes. DR=0 mode, all addresses are bare
  // physical offsets.
  TestClient client(m_client_fd());
  Handshake(client);

  // Pre-load a no-op (ori 0,0,0 = 0x60000000) at the target via writeMemory.
  // "YAAAAA==" is base64 of {0x60, 0x00, 0x00, 0x00}.
  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "writeMemory",
    "arguments": {"memoryReference": "0x00008000", "data": "YAAAAA==",
                  "count": 4}
  })");
  ASSERT_TRUE(client.Receive().has_value());

  // Issue the detour. detourBody `mr r3,r3` (0x7C631B78) -> base64 "fGMbeA==".
  client.Send(R"({
    "seq": 4,
    "type": "request",
    "command": "dolphin_detour",
    "arguments": {"memoryReference": "0x00008000",
                  "detourAddress": "0x0000c000",
                  "detourBody": "fGMbeA=="}
  })");
  const auto resp = client.Receive();
  ASSERT_TRUE(resp.has_value());
  ASSERT_TRUE(resp->at("success").get<bool>()) << resp->at("message").to_str();
  const auto& body = resp->at("body").get<picojson::object>();
  EXPECT_EQ(body.at("targetAddress").to_str(), "0x00008000");
  EXPECT_EQ(body.at("detourAddress").to_str(), "0x0000c000");
  // Trampoline sits right after detour body (4 bytes) + appended tail branch
  // (4 bytes), so at detour_address + 8.
  EXPECT_EQ(body.at("trampolineAddress").to_str(), "0x0000c008");
  // originalInstruction echoes the 4 bytes we pre-loaded (the no-op).
  EXPECT_EQ(body.at("originalInstruction").to_str(), "YAAAAA==");

  // Verify the target was patched: readMemory at 0x8000 must NOT be the
  // original no-op anymore.
  client.Send(R"({
    "seq": 5,
    "type": "request",
    "command": "readMemory",
    "arguments": {"memoryReference": "0x00008000", "count": 4}
  })");
  const auto target_read = client.Receive();
  ASSERT_TRUE(target_read.has_value());
  ASSERT_TRUE(target_read->at("success").get<bool>());
  const std::string target_str =
      target_read->at("body").get<picojson::object>().at("data").to_str();
  EXPECT_NE(target_str, "YAAAAA==");

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

TEST_F(DapSessionTest, FindFreeMemoryReturnsCanonicalAlignedAddress)
{
  // DESNOTE(jbarber, 2026-07-21): FindFreeMemory scans MEM1 RAM directly
  // (not via the address-space accessors) so it succeeds under DR=0 even
  // though the returned canonical address can't be written there. We just
  // assert the response shape: success, an "address" field that looks like
  // a canonical MEM1 address (high bit set), 4-byte aligned, and "count"
  // echoing the request.
  TestClient client(m_client_fd());
  Handshake(client);

  client.Send(R"({
    "seq": 3,
    "type": "request",
    "command": "dolphin_findFreeMemory",
    "arguments": {"count": 16}
  })");
  const auto resp = client.Receive();
  ASSERT_TRUE(resp.has_value());
  ASSERT_TRUE(resp->at("success").get<bool>()) << resp->at("message").to_str();
  const auto& body = resp->at("body").get<picojson::object>();
  // Parse the returned hex string and verify it's canonical + aligned.
  const std::string addr_str = body.at("address").to_str();
  const auto parsed_addr = DAP::Json::ParseHexAddress(addr_str);
  ASSERT_TRUE(parsed_addr.has_value());
  EXPECT_GE(*parsed_addr, 0x80000000u);
  EXPECT_EQ(*parsed_addr % 4u, 0u);
  EXPECT_EQ(body.at("count").get<double>(), 16.0);

  client.Send(R"({
    "seq": 9,
    "type": "request",
    "command": "disconnect"
  })");
  (void)client.Receive();
}

class DapMultiSessionTest : public DapSessionTest
{
protected:
  void SetUp() override
  {
    DapSessionTest::SetUp();
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, m_second_fds), 0);
    m_second_server = std::thread([this] {
      Core::DeclareAsCPUThread();
      DAP::DapTransport transport(m_second_fds[1]);
      DAP::RunSession(transport, Core::System::GetInstance());
      Core::UndeclareAsCPUThread();
    });
  }

  void TearDown() override
  {
    shutdown(m_second_fds[0], SHUT_RDWR);
    if (m_second_server.joinable())
      m_second_server.join();
    close(m_second_fds[0]);
    DapSessionTest::TearDown();
  }

  void HandshakeAdditional(TestClient& client)
  {
    client.Send(R"({"seq":1,"type":"request","command":"initialize","arguments":{}})");
    ASSERT_TRUE(client.Receive().has_value());
    const auto initialized = client.Receive();
    ASSERT_TRUE(initialized.has_value());
    EXPECT_EQ(initialized->at("event").to_str(), "initialized");

    client.Send(R"({"seq":2,"type":"request","command":"launch","arguments":{}})");
    ASSERT_TRUE(client.Receive().has_value());
    client.Send(R"({"seq":3,"type":"request","command":"configurationDone"})");
    ASSERT_TRUE(client.Receive().has_value());
    const auto stopped = client.Receive();
    ASSERT_TRUE(stopped.has_value());
    EXPECT_EQ(stopped->at("event").to_str(), "stopped");
  }

  int m_second_fds[2] = {-1, -1};
  std::thread m_second_server;
};

TEST_F(DapMultiSessionTest, BreakpointsSynchronizeWithoutCrossClientRemoval)
{
  constexpr u32 gui_address = CODE_ADDRESS + 0x20;
  constexpr u32 first_address = CODE_ADDRESS;
  constexpr u32 second_address = CODE_ADDRESS + 4;
  auto& breakpoints = Core::System::GetInstance().GetPowerPC().GetBreakPoints();

  TestClient first(m_client_fd());
  TestClient second(m_second_fds[0]);
  Handshake(first);
  HandshakeAdditional(second);

  (void)breakpoints.Add(gui_address);
  const auto first_gui_event = first.Receive();
  ASSERT_TRUE(first_gui_event.has_value());
  EXPECT_EQ(first_gui_event->at("event").to_str(), "breakpoint");
  const auto second_gui_event = second.Receive();
  ASSERT_TRUE(second_gui_event.has_value());
  EXPECT_EQ(second_gui_event->at("event").to_str(), "breakpoint");
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(gui_address));

  first.Send(R"({
    "seq":10,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[{"instructionReference":"0x00003100"}]}
  })");
  const auto first_response = first.Receive();
  ASSERT_TRUE(first_response.has_value());
  ASSERT_TRUE(first_response->at("success").get<bool>()) << first_response->at("message").to_str();
  const auto first_external = second.Receive();
  ASSERT_TRUE(first_external.has_value());
  EXPECT_EQ(first_external->at("event").to_str(), "breakpoint");
  EXPECT_EQ(first_external->at("body").get<picojson::object>().at("reason").to_str(), "new");

  second.Send(R"({
    "seq":11,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[{"instructionReference":"0x00003104"}]}
  })");
  const auto second_response = second.Receive();
  ASSERT_TRUE(second_response.has_value());
  EXPECT_TRUE(second_response->at("success").get<bool>());
  const auto second_external = first.Receive();
  ASSERT_TRUE(second_external.has_value());
  EXPECT_EQ(second_external->at("event").to_str(), "breakpoint");
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(first_address));
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(second_address));
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(gui_address));

  first.Send(R"({
    "seq":13,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[]}
  })");
  ASSERT_TRUE(first.Receive().has_value());
  const auto removed = second.Receive();
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(removed->at("body").get<picojson::object>().at("reason").to_str(), "removed");
  EXPECT_FALSE(breakpoints.IsAddressBreakPoint(first_address));
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(second_address));
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(gui_address));

  breakpoints.Remove(gui_address);
  const auto gui_removed = second.Receive();
  ASSERT_TRUE(gui_removed.has_value());
  EXPECT_EQ(gui_removed->at("event").to_str(), "breakpoint");
  EXPECT_EQ(gui_removed->at("body").get<picojson::object>().at("reason").to_str(), "removed");
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(second_address));

  first.Send(R"({
    "seq":14,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[{"instructionReference":"0x00003100"}]}
  })");
  ASSERT_TRUE(first.Receive().has_value());
  ASSERT_TRUE(second.Receive().has_value());

  first.Send(R"({"seq":15,"type":"request","command":"disconnect"})");
  ASSERT_TRUE(first.Receive().has_value());
  const auto disconnected_removed = second.Receive();
  ASSERT_TRUE(disconnected_removed.has_value());
  EXPECT_EQ(disconnected_removed->at("body").get<picojson::object>().at("reason").to_str(),
            "removed");
  EXPECT_FALSE(breakpoints.IsAddressBreakPoint(first_address));
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(second_address));
  second.Send(R"({"seq":16,"type":"request","command":"disconnect"})");
  ASSERT_TRUE(second.Receive().has_value());
}

TEST_F(DapMultiSessionTest, GuiSiteMutationsFanOutAndAllowAuthoritativeDapReAdd)
{
  constexpr u32 first_address = CODE_ADDRESS;
  constexpr u32 second_address = CODE_ADDRESS + 4;
  auto& breakpoints = Core::System::GetInstance().GetPowerPC().GetBreakPoints();
  TestClient first(m_client_fd());
  TestClient second(m_second_fds[0]);
  Handshake(first);
  HandshakeAdditional(second);

  first.Send(R"({
    "seq":20,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[{
      "instructionReference":"0x00003100","condition":"r3 == 1"
    }]}
  })");
  ASSERT_TRUE(first.Receive().has_value());
  ASSERT_TRUE(second.Receive().has_value());

  second.Send(R"({
    "seq":19,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[{
      "instructionReference":"0x00003100","condition":"r4 == 2"
    }]}
  })");
  const auto compatible_response = second.Receive();
  ASSERT_TRUE(compatible_response.has_value());
  EXPECT_TRUE(compatible_response->at("success").get<bool>());
  const auto compatible_event = first.Receive();
  ASSERT_TRUE(compatible_event.has_value());
  EXPECT_EQ(compatible_event->at("body").get<picojson::object>().at("reason").to_str(), "changed");
  const auto compatible = breakpoints.GetRegularBreakpoint(first_address);
  ASSERT_NE(compatible, nullptr);
  ASSERT_TRUE(compatible->condition.has_value());
  EXPECT_EQ(compatible->condition->GetText(), "(r3==1) || (r4==2)");

  EXPECT_TRUE(breakpoints.ToggleEnable(first_address));
  EXPECT_FALSE(breakpoints.IsBreakPointEnable(first_address));
  for (TestClient* client : {&first, &second})
  {
    const auto event = client->Receive();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->at("body").get<picojson::object>().at("reason").to_str(), "changed");
  }
  EXPECT_TRUE(breakpoints.ToggleEnable(first_address));
  EXPECT_TRUE(breakpoints.IsBreakPointEnable(first_address));
  for (TestClient* client : {&first, &second})
  {
    const auto event = client->Receive();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->at("body").get<picojson::object>().at("reason").to_str(), "changed");
  }

  ASSERT_TRUE(breakpoints.Add(first_address));
  ASSERT_EQ(breakpoints.GetStrings().size(), 1u);
  for (TestClient* client : {&first, &second})
  {
    const auto event = client->Receive();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->at("body").get<picojson::object>().at("reason").to_str(), "changed");
  }
  EXPECT_TRUE(breakpoints.ToggleEnable(first_address));
  EXPECT_FALSE(breakpoints.IsBreakPointEnable(first_address));
  for (TestClient* client : {&first, &second})
  {
    const auto event = client->Receive();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->at("body").get<picojson::object>().at("reason").to_str(), "changed");
  }
  EXPECT_TRUE(breakpoints.ToggleEnable(first_address));
  for (TestClient* client : {&first, &second})
  {
    const auto event = client->Receive();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->at("body").get<picojson::object>().at("reason").to_str(), "changed");
  }

  ASSERT_TRUE(breakpoints.Add(first_address, true, false, Expression::TryParse("r3 == 2")));
  const auto edited = breakpoints.GetRegularBreakpoint(first_address);
  ASSERT_NE(edited, nullptr);
  ASSERT_TRUE(edited->condition.has_value());
  EXPECT_EQ(edited->condition->GetText(), "(r3==1) || (r3==2) || (r4==2)");
  for (TestClient* client : {&first, &second})
  {
    const auto event = client->Receive();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->at("body").get<picojson::object>().at("reason").to_str(), "changed");
  }

  const u64 edit_revision = breakpoints.GetRevision();
  first.Send(R"({
    "seq":21,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[{
      "instructionReference":"0x00003100","condition":"r3 == 1"
    }]}
  })");
  const auto stale_resend = first.Receive();
  ASSERT_TRUE(stale_resend.has_value());
  EXPECT_TRUE(stale_resend->at("success").get<bool>());
  EXPECT_EQ(breakpoints.GetRevision(), edit_revision);

  EXPECT_TRUE(breakpoints.Remove(first_address));
  EXPECT_FALSE(breakpoints.IsAddressBreakPoint(first_address));
  for (TestClient* client : {&first, &second})
  {
    const auto event = client->Receive();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->at("body").get<picojson::object>().at("reason").to_str(), "removed");
  }

  first.Send(R"({
    "seq":22,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[{"instructionReference":"0x00003100"}]}
  })");
  const auto first_readd = first.Receive();
  ASSERT_TRUE(first_readd.has_value());
  EXPECT_TRUE(first_readd->at("success").get<bool>());
  const auto first_readd_event = second.Receive();
  ASSERT_TRUE(first_readd_event.has_value());
  EXPECT_EQ(first_readd_event->at("body").get<picojson::object>().at("reason").to_str(), "new");

  second.Send(R"({
    "seq":23,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[{"instructionReference":"0x00003104"}]}
  })");
  ASSERT_TRUE(second.Receive().has_value());
  ASSERT_TRUE(first.Receive().has_value());
  breakpoints.Clear();
  EXPECT_TRUE(breakpoints.GetSnapshot()->breakpoints.empty());
  for (TestClient* client : {&first, &second})
  {
    for (int i = 0; i < 2; ++i)
    {
      const auto event = client->Receive();
      ASSERT_TRUE(event.has_value());
      EXPECT_EQ(event->at("body").get<picojson::object>().at("reason").to_str(), "removed");
    }
  }

  second.Send(R"({
    "seq":24,"type":"request","command":"setInstructionBreakpoints",
    "arguments":{"breakpoints":[{"instructionReference":"0x00003104"}]}
  })");
  const auto second_readd = second.Receive();
  ASSERT_TRUE(second_readd.has_value());
  EXPECT_TRUE(second_readd->at("success").get<bool>());
  const auto second_readd_event = first.Receive();
  ASSERT_TRUE(second_readd_event.has_value());
  EXPECT_EQ(second_readd_event->at("body").get<picojson::object>().at("reason").to_str(), "new");
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(second_address));

  first.Send(R"({"seq":25,"type":"request","command":"disconnect"})");
  ASSERT_TRUE(first.Receive().has_value());
  second.Send(R"({"seq":26,"type":"request","command":"disconnect"})");
  ASSERT_TRUE(second.Receive().has_value());
}

TEST_F(DapMultiSessionTest, DataBreakpointsAndFreezesAreOriginAwareAndIndependent)
{
  constexpr u32 first_address = 0x00004100;
  constexpr u32 second_address = 0x00004200;
  constexpr u32 freeze_address = 0x00004300;
  constexpr u32 gui_address = 0x00004400;
  constexpr u32 second_freeze_address = 0x00004500;
  auto& memchecks = Core::System::GetInstance().GetPowerPC().GetMemChecks();
  TestClient first(m_client_fd());
  TestClient second(m_second_fds[0]);
  Handshake(first);
  HandshakeAdditional(second);

  first.Send(R"({"seq":30,"type":"request","command":"setDataBreakpoints",
    "arguments":{"breakpoints":[{"dataId":"0x00004100","accessType":"write"}]}})");
  const auto first_response = first.Receive();
  ASSERT_TRUE(first_response.has_value());
  ASSERT_TRUE(first_response->at("success").get<bool>());
  const auto& first_breakpoints =
      first_response->at("body").get<picojson::object>().at("breakpoints").get<picojson::array>();
  ASSERT_EQ(first_breakpoints.size(), 1u);
  const double first_breakpoint_id =
      first_breakpoints.front().get<picojson::object>().at("id").get<double>();
  const auto first_external = second.Receive();
  ASSERT_TRUE(first_external.has_value());
  EXPECT_EQ(first_external->at("event").to_str(), "breakpoint");
  EXPECT_EQ(first_external->at("body")
                .get<picojson::object>()
                .at("breakpoint")
                .get<picojson::object>()
                .at("id")
                .get<double>(),
            first_breakpoint_id);

  second.Send(R"({"seq":31,"type":"request","command":"setDataBreakpoints",
    "arguments":{"breakpoints":[{"dataId":"0x00004200","accessType":"read"}]}})");
  ASSERT_TRUE(second.Receive().has_value());
  ASSERT_TRUE(first.Receive().has_value());
  EXPECT_NE(memchecks.GetMemCheck(first_address), nullptr);
  EXPECT_NE(memchecks.GetMemCheck(second_address), nullptr);

  first.Send(R"({"seq":32,"type":"request","command":"dolphin_freeze",
    "arguments":{"memoryReference":"0x00004300","count":4,"data":"AAAAAA=="}})");
  ASSERT_TRUE(first.Receive().has_value());
  second.Send(R"({"seq":33,"type":"request","command":"setDataBreakpoints",
    "arguments":{"breakpoints":[{"dataId":"0x00004200","accessType":"read"}]}})");
  ASSERT_TRUE(second.Receive().has_value());
  second.Send(R"({"seq":34,"type":"request","command":"dolphin_freeze",
    "arguments":{"memoryReference":"0x00004500","count":4,"data":"AAAAAA=="}})");
  ASSERT_TRUE(second.Receive().has_value());
  EXPECT_EQ(memchecks.GetMemCheck(freeze_address), nullptr);
  EXPECT_EQ(memchecks.GetMemCheck(second_freeze_address), nullptr);
  EXPECT_TRUE(memchecks.GetSnapshot()->OverlapsPrivateFreeze(freeze_address, 4));
  EXPECT_TRUE(memchecks.GetSnapshot()->OverlapsPrivateFreeze(second_freeze_address, 4));

  TMemCheck gui;
  gui.start_address = gui_address;
  gui.end_address = gui_address;
  gui.is_break_on_write = true;
  gui.break_on_hit = true;
  memchecks.Add(std::move(gui));
  for (TestClient* client : {&first, &second})
  {
    const auto event = client->Receive();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->at("body").get<picojson::object>().at("reason").to_str(), "new");
  }
  first.Send(R"({"seq":35,"type":"request","command":"disconnect"})");
  ASSERT_TRUE(first.Receive().has_value());
  const auto removed = second.Receive();
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(removed->at("body").get<picojson::object>().at("reason").to_str(), "removed");
  EXPECT_EQ(removed->at("body")
                .get<picojson::object>()
                .at("breakpoint")
                .get<picojson::object>()
                .at("id")
                .get<double>(),
            first_breakpoint_id);
  EXPECT_EQ(memchecks.GetMemCheck(first_address), nullptr);
  EXPECT_NE(memchecks.GetMemCheck(second_address), nullptr);
  EXPECT_NE(memchecks.GetMemCheck(gui_address), nullptr);
  EXPECT_FALSE(memchecks.GetSnapshot()->OverlapsPrivateFreeze(freeze_address, 4));
  EXPECT_TRUE(memchecks.GetSnapshot()->OverlapsPrivateFreeze(second_freeze_address, 4));

  second.Send(R"({"seq":36,"type":"request","command":"disconnect"})");
  ASSERT_TRUE(second.Receive().has_value());
  EXPECT_NE(memchecks.GetMemCheck(gui_address), nullptr);
  memchecks.Remove(gui_address);
}
}  // namespace
#endif  // _WIN32

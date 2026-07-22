// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "Core/Debugger/DAP/DapFraming.h"

namespace
{
// Feeds a fixed byte buffer through the Framing::ReadExactFn interface.
class BufferReader
{
public:
  explicit BufferReader(std::string data) : m_data(std::move(data)) {}

  DAP::Framing::ReadExactFn Reader()
  {
    return [this](void* out, std::size_t size) { return Read(out, size); };
  }

  std::size_t remaining() const { return m_data.size() - m_pos; }

private:
  bool Read(void* out, std::size_t size)
  {
    if (m_pos + size > m_data.size())
      return false;

    std::memcpy(out, m_data.data() + m_pos, size);
    m_pos += size;
    return true;
  }

  std::string m_data;
  std::size_t m_pos = 0;
};
}  // namespace

TEST(DapFraming, EncodeAddsContentLengthHeader)
{
  EXPECT_EQ(DAP::Framing::EncodeMessage("{}"), "Content-Length: 2\r\n\r\n{}");
}

TEST(DapFraming, EncodeEmptyBody)
{
  EXPECT_EQ(DAP::Framing::EncodeMessage(""), "Content-Length: 0\r\n\r\n");
}

TEST(DapFraming, DecodeReadsBody)
{
  BufferReader reader("Content-Length: 13\r\n\r\n{\"seq\":1234}!");
  const std::optional<std::string> body = DAP::Framing::DecodeMessage(reader.Reader());

  ASSERT_TRUE(body.has_value());
  EXPECT_EQ(*body, "{\"seq\":1234}!");
  EXPECT_EQ(reader.remaining(), 0u);
}

TEST(DapFraming, DecodeZeroLengthBody)
{
  BufferReader reader("Content-Length: 0\r\n\r\n");
  const std::optional<std::string> body = DAP::Framing::DecodeMessage(reader.Reader());

  ASSERT_TRUE(body.has_value());
  EXPECT_TRUE(body->empty());
}

TEST(DapFraming, DecodeStopsAtBodyBoundary)
{
  // Two concatenated messages: the decoder must consume exactly the first.
  BufferReader reader("Content-Length: 2\r\n\r\nABContent-Length: 2\r\n\r\nCD");

  const std::optional<std::string> first = DAP::Framing::DecodeMessage(reader.Reader());
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, "AB");

  const std::optional<std::string> second = DAP::Framing::DecodeMessage(reader.Reader());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, "CD");
}

TEST(DapFraming, RoundTripThroughEncodeThenDecode)
{
  constexpr std::string_view payload = R"({"command":"initialize","seq":7})";
  BufferReader reader(DAP::Framing::EncodeMessage(payload));

  const std::optional<std::string> body = DAP::Framing::DecodeMessage(reader.Reader());
  ASSERT_TRUE(body.has_value());
  EXPECT_EQ(*body, payload);
}

TEST(DapFraming, DecodeBlankHeaderLineYieldsEmpty)
{
  BufferReader reader("\r\n");
  const std::optional<std::string> body = DAP::Framing::DecodeMessage(reader.Reader());

  ASSERT_TRUE(body.has_value());
  EXPECT_TRUE(body->empty());
}

TEST(DapFraming, DecodeRejectsUnknownHeader)
{
  BufferReader reader("Content-Type: text/plain\r\n\r\n");
  EXPECT_FALSE(DAP::Framing::DecodeMessage(reader.Reader()).has_value());
}

TEST(DapFraming, DecodeRejectsNonNumericContentLength)
{
  BufferReader reader("Content-Length: abc\r\n\r\n");
  EXPECT_FALSE(DAP::Framing::DecodeMessage(reader.Reader()).has_value());
}

TEST(DapFraming, DecodeFailsOnTruncatedBody)
{
  // Header promises 10 bytes but only 3 are available.
  BufferReader reader("Content-Length: 10\r\n\r\nabc");
  EXPECT_FALSE(DAP::Framing::DecodeMessage(reader.Reader()).has_value());
}

TEST(DapFraming, DecodeFailsOnEmptyStream)
{
  BufferReader reader("");
  EXPECT_FALSE(DAP::Framing::DecodeMessage(reader.Reader()).has_value());
}

TEST(DapFraming, DecodeRejectsOversizedContentLength)
{
  // DESNOTE(jbarber, 2026-07-22): DecodeMessage caps Content-Length at 16 MiB
  // so a malicious client can't ship a header claiming a multi-GB body that
  // forces a multi-GB body.resize() on the session I/O thread. Bugbot #62.
  // We don't need to send a real body -- the cap fires before any body read.
  BufferReader reader("Content-Length: 1073741824\r\n\r\n");
  EXPECT_FALSE(DAP::Framing::DecodeMessage(reader.Reader()).has_value());
}

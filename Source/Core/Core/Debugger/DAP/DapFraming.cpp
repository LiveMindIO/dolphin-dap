// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapFraming.h"

#include <charconv>

#include <fmt/format.h>

#include "Common/Logging/Log.h"

namespace DAP::Framing
{
std::string EncodeMessage(std::string_view body)
{
  return fmt::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
}

std::optional<std::string> DecodeMessage(const ReadExactFn& read_exact)
{
  std::string header_line;
  header_line.reserve(128);

  while (true)
  {
    char c = 0;
    if (!read_exact(&c, 1))
      return std::nullopt;

    if (c == '\r')
    {
      char lf = 0;
      if (!read_exact(&lf, 1) || lf != '\n')
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
  const auto [ptr, ec] =
      std::from_chars(length_str.data(), length_str.data() + length_str.size(), content_length);
  if (ec != std::errc{} || ptr != length_str.data() + length_str.size())
  {
    ERROR_LOG_FMT(CONSOLE, "DAP: invalid Content-Length: {}", header_line);
    return std::nullopt;
  }

  char separator = 0;
  if (!read_exact(&separator, 1) || separator != '\r')
    return std::nullopt;
  if (!read_exact(&separator, 1) || separator != '\n')
    return std::nullopt;

  std::string body;
  body.resize(content_length);
  if (content_length > 0 && !read_exact(body.data(), content_length))
    return std::nullopt;

  return body;
}
}  // namespace DAP::Framing

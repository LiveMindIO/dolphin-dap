// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace DAP::Framing
{
// Fills exactly `size` bytes into `data`, returning false on stream end/error.
using ReadExactFn = std::function<bool(void* data, std::size_t size)>;

// Wraps a message body in the DAP wire format: "Content-Length: N\r\n\r\n<body>".
std::string EncodeMessage(std::string_view body);

// Reads a single DAP message body using `read_exact`. Returns:
//   - the body (possibly empty) on success,
//   - an empty string when a bare blank header line is encountered,
//   - std::nullopt on stream end or a malformed header.
std::optional<std::string> DecodeMessage(const ReadExactFn& read_exact);
}  // namespace DAP::Framing

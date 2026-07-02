// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace DAP
{
class DapTransport;

bool RunHandshake(DapTransport& transport);
}  // namespace DAP

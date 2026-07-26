// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core
{
class System;
}

namespace DAP
{
class DapTransport;

void RunSession(DapTransport& transport, Core::System& system);
}  // namespace DAP

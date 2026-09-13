// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>

namespace Core
{
class System;
}

namespace DAP
{
class DapTransport;

struct SessionTestHooks
{
  std::function<void()> async_step_worker_joined;
};

void RunSession(DapTransport& transport, Core::System& system,
                const SessionTestHooks* test_hooks = nullptr);
}  // namespace DAP

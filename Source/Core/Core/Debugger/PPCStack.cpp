// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/PPCStack.h"

#include <algorithm>
#include <utility>

#include <fmt/format.h>

#include "Common/SymbolDB.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Core::Debug
{
StackTrace GetPPCStackTrace(Core::System& system, const int start_frame, const int levels)
{
  Core::CPUThreadGuard guard(system);
  auto& power_pc = system.GetPowerPC();
  const auto& ppc_state = power_pc.GetPPCState();
  auto& debug_interface = power_pc.GetDebugInterface();
  auto& symbol_db = power_pc.GetSymbolDB();

  std::vector<StackFrame> frames;
  const auto push_frame = [&](const u32 address) {
    StackFrame frame;
    frame.index = frames.size();
    frame.address = address;
    frame.name = debug_interface.GetDescription(address);
    if (frame.name.empty() || frame.name == "Invalid")
      frame.name = fmt::format("0x{:08x}", address);

    if (const std::optional<PPCSymbolDB::SourceLine> source_line = symbol_db.GetSourceLine(address))
    {
      frame.source =
          StackFrameSource{source_line->file, source_line->file_index, source_line->line};
    }
    else if (const Common::Symbol* symbol = symbol_db.GetSymbolFromAddr(address);
             symbol != nullptr && symbol->type == Common::Symbol::Type::Function)
    {
      frame.disassembly_base = symbol->address;
      frame.disassembly_line = (address - symbol->address) / 4 + 1;
    }

    frames.push_back(std::move(frame));
  };

  const auto is_stack_bottom = [&](const u32 address) {
    return address == 0 || !PowerPC::MMU::HostIsRAMAddress(guard, address);
  };

  // The stopped PC is always frame zero. LR and saved backchain LRs are return addresses, so
  // callers are represented by the preceding instruction.
  push_frame(ppc_state.pc);

  if (LR(ppc_state) != 0)
    push_frame(LR(ppc_state) - 4);

  if (!is_stack_bottom(ppc_state.gpr[1]))
  {
    u32 address = PowerPC::MMU::HostRead<u32>(guard, ppc_state.gpr[1]);
    for (int count = 0; !is_stack_bottom(address) && !is_stack_bottom(address + 4) && count < 20;
         ++count)
    {
      const u32 saved_lr = PowerPC::MMU::HostRead<u32>(guard, address + 4);
      push_frame(saved_lr - 4);
      address = PowerPC::MMU::HostRead<u32>(guard, address);
    }
  }

  StackTrace result;
  result.total_frames = frames.size();

  const std::size_t begin = static_cast<std::size_t>(std::max(0, start_frame));
  if (begin >= frames.size())
    return result;

  // A non-positive level count requests all remaining frames.
  const std::size_t end = levels <= 0 ?
                              frames.size() :
                              std::min(frames.size(), begin + static_cast<std::size_t>(levels));
  frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(end), frames.end());
  frames.erase(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(begin));
  for (std::size_t i = 0; i < frames.size(); ++i)
    frames[i].index = begin + i;

  result.frames = std::move(frames);
  return result;
}
}  // namespace Core::Debug

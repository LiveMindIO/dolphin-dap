# Dolphin DAP

This folder documents the **Debug Adapter Protocol server** that exposes
Dolphin's PowerPC debugger to DAP-aware clients (VS Code, Cursor, Neovim, etc.).

For the per-operation request/response reference, see
[`capabilities.md`](capabilities.md). The full operations table is below in
[Features](#features).

## Features

Each operation below links to its detailed reference section in
[`capabilities.md`](capabilities.md).

### Standard requests

| Request | Summary |
|---------|---------|
| [`initialize`](capabilities.md#initialize) | Capability handshake — advertises standard + Dolphin-specific extensions. |
| [`launch`](capabilities.md#launch-attach) | Boot the configured ISO/DOL. Emulation starts paused. |
| [`attach`](capabilities.md#launch-attach) | Attach to an already-running core. |
| [`configurationDone`](capabilities.md#configurationdone) | Concludes the launch handshake. |
| [`continue`](capabilities.md#continue-pause-step) | Resume execution. `allThreadsContinued: true` reported. |
| [`pause`](capabilities.md#continue-pause-step) | Halt the core; emits `stopped`/`"pause"`. |
| [`next`](capabilities.md#continue-pause-step) | Step over (interpreter mode). |
| [`stepIn`](capabilities.md#continue-pause-step) | Step into (interpreter mode). |
| [`stepOut`](capabilities.md#continue-pause-step) | Step out (async worker, classified stop on completion). |
| [`setBreakpoints`](capabilities.md#setbreakpoints) | Source/line code breakpoints. Conditional via `condition`. |
| [`setInstructionBreakpoints`](capabilities.md#setinstructionbreakpoints) | Address-keyed code breakpoints; replaces the whole list. |
| [`setDataBreakpoints`](capabilities.md#setdatabreakpoints) | Watchpoints. **Ranged** watchpoints via `length` extension. |
| [`readMemory`](capabilities.md#readmemory-writememory) | Read bytes; base64 payload. `unreadableBytes` reported. |
| [`writeMemory`](capabilities.md#readmemory-writememory) | Write bytes; base64 payload. `allowPartial` for soft failures. |
| [`disassemble`](capabilities.md#disassemble) | Per-instruction disassembly. `instructionCount` capped at 65536. |
| [`stackTrace`](capabilities.md#stacktrace-threads-scopes-variables-setvariable) | PPC call stack. |
| [`threads`](capabilities.md#stacktrace-threads-scopes-variables-setvariable) | OS thread enumeration. |
| [`scopes`](capabilities.md#stacktrace-threads-scopes-variables-setvariable) | Variable scopes for a frame (`Registers`, `PC`). |
| [`variables`](capabilities.md#stacktrace-threads-scopes-variables-setvariable) | Enumerate variables in a scope. |
| [`setVariable`](capabilities.md#stacktrace-threads-scopes-variables-setvariable) | Mutate a variable (registers, etc.). |
| [`evaluate`](capabilities.md#evaluate) | Evaluate a PPC debugger expression. |
| [`goto`](capabilities.md#goto-gototargets) | Set PC; re-emits `stopped`/`"goto"`. |
| [`gotoTargets`](capabilities.md#goto-gototargets) | Enumerate goto targets (address doubles as target id). |
| [`exceptionInfo`](capabilities.md#exceptioninfo) | Reports pending PPC exceptions. |
| [`loadedSources`](capabilities.md#loadedsources-source-breakpointlocations) | List known source files (DWARF/entrypoints-driven). |
| [`source`](capabilities.md#loadedsources-source-breakpointlocations) | Fetch source contents. Emits ≤256 lines. |
| [`breakpointLocations`](capabilities.md#loadedsources-source-breakpointlocations) | List valid breakpoint lines. Capped at 65536. |
| [`terminate`](capabilities.md#terminate-restart-disconnect) | Halt the core; emits `terminated`. |
| [`restart`](capabilities.md#terminate-restart-disconnect) | PPC reset + forced pause; emits `stopped`/`"restart"`. |
| [`disconnect`](capabilities.md#terminate-restart-disconnect) | End session; sockets torn down. |

### Dolphin-specific custom requests

| Request | Summary |
|---------|---------|
| [`dolphin_realtimeWatch`](capabilities.md#dolphin_realtimewatch) | Subscribe to a memory region; stream `dolphin_memoryChanged` events on change at field rate. |
| [`dolphin_realtimeWatchCancel`](capabilities.md#dolphin_realtimewatchcancel) | Cancel a realtime watch subscription by `watchId`. |
| [`dolphin_freeze`](capabilities.md#dolphin_freeze) | Hold a memory region at a fixed value. Standalone form or freeze-an-existing-watch form. |
| [`dolphin_unfreeze`](capabilities.md#dolphin_unfreeze) | Clear the freeze layer on a watch; the watch keeps running. Idempotent. |
| [`dolphin_findFreeMemory`](capabilities.md#dolphin_findfreememory) | Locate the smallest 4-byte-aligned zero-run ≥ `count` bytes in MEM1. |
| [`dolphin_injectCode`](capabilities.md#dolphin_injectcode) | Write raw PPC machine code (base64) at an explicit or server-allocated address. iCache + JIT invalidated. |
| [`dolphin_detour`](capabilities.md#dolphin_detour) | Install a transparent detour at a 4-byte instruction target: detour body + trampoline that replays the original instruction. |

## Running the server

Build with the NoGUI target (DAP sources compile into the `core` static lib;
there is no separate binary or compile flag):

```bash
cmake -B build -DENABLE_NOGUI=ON -DENABLE_QT=OFF
cmake --build build --target dolphin-nogui
```

The server is **inert** unless a DAP port or socket is configured at runtime
(mirrors `GDBPort`). DAP and GDB are mutually exclusive. `stdout` stays free
for normal logging. The DAP client (VS Code / Cursor / Neovim) then attaches
over the socket.

**TCP** (`Dolphin.General.DAPPort`):

```bash
dolphin-emu-nogui -C Dolphin.General.DAPPort=5678 \
  --exec ~/projects/ai/yolo/crowd-control/melee-iso/game.iso --platform headless
```

Or persist in `Dolphin.ini`:

```ini
[General]
DAPPort = 5678
```

**Unix socket** (`Dolphin.General.DAPSocket`, Linux/macOS only) takes priority
over the port (mirrors `GDBSocket`):

```bash
dolphin-emu-nogui -C Dolphin.General.DAPSocket=./dap.sock \
  --exec ~/projects/ai/yolo/crowd-control/melee-iso/game.iso --platform headless
```

```ini
[General]
DAPSocket = /tmp/dolphin-dap.sock
```

**Don't pause at entry** — let the game run immediately, only breaking when a
breakpoint is hit or the client explicitly pauses. Set
`Dolphin.General.DAPStopOnEntry=false` at dolphin launch; an explicit
`stopOnEntry` field on a `launch`/`attach` request overrides it per session:

```bash
dolphin-emu-nogui -C Dolphin.General.DAPSocket=./dap.sock \
  -C Dolphin.General.DAPStopOnEntry=false \
  --exec ~/projects/ai/yolo/crowd-control/melee-iso/game.iso --platform headless
```

**With a sidecar debug ELF** for DWARF 1.1 line info (imported after boot; code
in memory must match the ELF link layout for line mappings to be correct):

```bash
dolphin-emu-nogui -C Dolphin.General.DAPPort=5678 \
  --debug-elf /path/to/build/GALE01/main.elf \
  --exec /path/to/GALE01.iso --platform headless
```

Equivalent config form for the ELF: `-C Dolphin.Debug.DwarfElf=/path/to/main.elf`.
For NonMatching decomp units, Melee `configure.py --debug` also generates
`entrypoints.json` beside `main.elf`; pass `--debug-entrypoints` or rely on
auto-discovery when the sibling file exists.

GDB and DAP are mutually exclusive — do not set `GDBPort`/`GDBSocket` at the
same time.

## Handshake test (no game required for transport check)

With Dolphin running and waiting for a client, send an `initialize` request:

```bash
python3 - <<'PY'
import json, socket
s = socket.create_connection(("127.0.0.1", 5678))
msg = json.dumps({"seq": 1, "type": "request", "command": "initialize",
                  "arguments": {"clientID": "test", "adapterID": "dolphin-dap"}})
data = f"Content-Length: {len(msg)}\r\n\r\n{msg}".encode()
s.sendall(data)
print(s.recv(4096).decode())
PY
```

Expect a JSON `response` with `"command":"initialize"` and `"success":true`.

## Neovim (lazy.nvim)

See [`nvim/README.md`](nvim/README.md) for `nvim-dap` + `nvim-dap-ui` setup,
`.dolphin-dap.lua` per-project config, and keymaps. A sample `launch.json` is
in [`launch.json.example`](launch.json.example).

## Tests

The DAP server is validated by GoogleTest suites under
`Source/UnitTests/Core/Debugger/DAP/` and `Source/UnitTests/Core/Debugger/DWARF/`.
None of them require an ISO, a booted game, or the JIT — they follow the same
pattern as `PageFaultTest` / `PageTableHostMappingTest`: initialize just the
memory subsystem, declare the test thread as the CPU thread, and drive PPC
state directly (with address translation off, so effective addresses map
straight to physical RAM).

| Suite | Layer | What it covers |
|-------|-------|----------------|
| `DapFramingTest` | transport | `Content-Length` framing encode/decode |
| `DapJsonTest` | JSON | picojson parsing, hex addresses, base64 |
| `DapProtocolTest` | protocol | request parsing + response/event building |
| `DapControllerTest` | core integration | `DapDebugController` against a real `Core::System`: register read/write, memory read/write (incl. partial/invalid), disassembly, breakpoints, detour rollback, DWARF source mapping |
| `DapSessionTest` | end-to-end | full `RunSession` command loop over a `socketpair`: handshake, `setBreakpoints`, `readMemory`, `writeMemory`, `disassemble`, `variables`, `setVariable`, step commands, unknown-command error |
| `DwarfReaderTest` | DWARF parser | DWARF 1.1 `.debug`/`.line` parsing, malformed input |
| `PPCSymbolDBLineTest` | symbol DB | line table queries, `ImportDwarf`, `Clear` |
| `RealtimeWatchTest` | sampler | `RealtimeWatchSampler` subscriptions, change detection, freeze canon write-back |

`DapSessionTest` connects a `socketpair` to `RunSession` running on a background
thread (which declares itself the CPU thread, so the controller's
`CPUThreadGuard`s are no-ops) and speaks real DAP over the socket — no TCP or
network. This is the layer that exercises framing + JSON + dispatch + event
serialization together.

Build and run:

```bash
cmake --build build --target tests
./build/Binaries/Tests/tests --gtest_filter='Dap*:Dwarf*:PPCSymbolDBLine*:RealtimeWatch*'
```

> The memory arena uses shared memory; restrictive sandboxes raise `SIGBUS` in
> `Memory::Init` (also breaks `PageFaultHostMappingTest`) — run tests
> unsandboxed.

## Known limitations

- **Single global breakpoint store.** Dolphin has one PPC core and one shared
  breakpoint/watchpoint store tied to it. Each DAP client's `setBreakpoints` /
  `setDataBreakpoints` / `setInstructionBreakpoints` replaces the global set.
  The intended topology is one DAP client per running core (DAP and GDB are
  mutually exclusive). Concurrent DAP clients on the same core will clobber
  each other's breakpoints/watchpoints — this is an architectural constraint,
  not a per-session isolation bug.
- **Realtime sample delivery cadence.** `dolphin_realtimeWatch` *samples* at
  field rate (~60 Hz NTSC) but *delivers* `dolphin_memoryChanged` events on
  the session loop's 50 ms poll, so a change is flushed to the socket within
  ~50 ms of being observed. Burst changes within a single frame are coalesced
  to one event per region.
- **Freeze is field-rate, not atomic.** `dolphin_freeze` writes the frozen
  value back at the next `vi_end_field_event` after the game drifts. For up to
  one field (~16 ms NTSC) the game's value is visible in memory before the
  restore. The canonical use cases (health/ammo/coins/timer) tolerate this
  window; for atomicity you'd need an MMU write-hook, which isn't implemented.
  Freeze is a layer on top of a watch subscription — clearing the freeze via
  `dolphin_unfreeze` leaves the watch running and dispatching events normally.

## Source awareness (DWARF 1.1 + entrypoints)

When DWARF 1.1 line info (MWCC/CodeWarrior `.debug`+`.line` sections) is loaded,
`stackTrace`, `loadedSources`, `source`, and `breakpointLocations` return real
file:line mappings. Loading happens automatically when booting a debug ELF
(`ElfReader::LoadSymbols`), programmatically via `Core::Debug::ImportDwarf` /
`ImportDwarfFromElf`, or as a sidecar via `Dolphin.Debug.DwarfElf` (or
`--debug-elf`). In the Qt UI: **Symbols → Load DWARF/Debug Info…**.

Retail-linked units that omit MWCC DWARF can still expose function entrypoints
+ definition lines via an **`entrypoints.json`** sidecar (normalized). This
supplies entrypoint-level line info without faking body-level line tables.
See [`../../.ai-doc-reference/entrypoints-format.md`](../../.ai-doc-reference/entrypoints-format.md).

# Dolphin DAP

This folder documents the **Debug Adapter Protocol server** that exposes
Dolphin's PowerPC debugger to DAP-aware clients (VS Code, Cursor, Neovim, etc.).

For the per-operation request/response reference, see
[`capabilities.md`](capabilities.md). The full operations table is below in
[Features](#features).

## Table of contents

- [Features](#features)
  - [Standard requests](#standard-requests)
  - [Dolphin-specific custom requests](#dolphin-specific-custom-requests)
- [Running the server](#running-the-server)
- [Configuring a DAP client](#configuring-a-dap-client)
- [Handshake test (no game required for transport check)](#handshake-test-no-game-required-for-transport-check)
- [Client integrations](#client-integrations)
- [Tests](#tests)
- [Known limitations](#known-limitations)
- [Source debugging with DWARF](#source-debugging-with-dwarf)

## Features

Each operation below links to its detailed reference section in
[`capabilities.md`](capabilities.md).

### Standard requests

| Request | Summary |
|---------|---------|
| [`initialize`](capabilities.md#initialize) | Capability handshake — advertises standard + Dolphin-specific extensions. |
| [`launch`](capabilities.md#launch-attach) | Start the configured game. Emulation starts paused. |
| [`attach`](capabilities.md#launch-attach) | Attach to an already-running core. |
| [`configurationDone`](capabilities.md#configurationdone) | Concludes the launch handshake. |
| [`continue`](capabilities.md#continue-pause-step) | Resume execution. `allThreadsContinued: true` reported. |
| [`pause`](capabilities.md#continue-pause-step) | Halt the core; emits `stopped`/`"pause"`. |
| [`next`](capabilities.md#continue-pause-step) | Step to the next source row without entering calls; instruction granularity remains available. |
| [`stepIn`](capabilities.md#continue-pause-step) | Step to the next source row, entering calls; instruction granularity remains available. |
| [`stepOut`](capabilities.md#continue-pause-step) | Step out (async worker, classified stop on completion). |
| [`setBreakpoints`](capabilities.md#setbreakpoints) | Source/line code breakpoints. Conditional via `condition`. |
| [`setInstructionBreakpoints`](capabilities.md#setinstructionbreakpoints) | Address-keyed code breakpoints; replaces the whole list. |
| [`setDataBreakpoints`](capabilities.md#setdatabreakpoints) | Watchpoints. **Ranged** watchpoints via `length` extension. |
| [`readMemory`](capabilities.md#readmemory-writememory) | Read bytes; base64 payload. `unreadableBytes` reported. |
| [`writeMemory`](capabilities.md#readmemory-writememory) | Write bytes; base64 payload. `allowPartial` for soft failures. |
| [`disassemble`](capabilities.md#disassemble) | Per-instruction disassembly. `instructionCount` capped at 65536. |
| [`stackTrace`](capabilities.md#stacktrace-threads-scopes-variables-setvariable) | PPC call stack. |
| [`threads`](capabilities.md#stacktrace-threads-scopes-variables-setvariable) | OS thread enumeration. |
| [`scopes`](capabilities.md#stacktrace-threads-scopes-variables-setvariable) | Registers/PC, plus frame-0 DWARF locals and globals. |
| [`variables`](capabilities.md#stacktrace-threads-scopes-variables-setvariable) | Enumerate scopes and expand supported DWARF structs, pointers, and arrays. |
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
| [`dolphin_memoryRegions`](capabilities.md#dolphin_memoryregions) | Enumerate scannable MEM1/MEM2/ARAM regions and target metadata. |
| [`dolphin_memoryScanStart`](capabilities.md#dolphin_memoryscanstart) | Start an asynchronous numeric, raw-byte, string, or PPC-instruction memory scan from a consistent snapshot. |
| [`dolphin_memoryScanRefine`](capabilities.md#dolphin_memoryscanrefine) | Refine a completed scan against a new value or its previous snapshot. |
| [`dolphin_memoryScanStatus`](capabilities.md#dolphin_memoryscanstatus) | Query scan/job state for diagnostics or recovery; normal clients rely on terminal events. |
| [`dolphin_memoryScanResults`](capabilities.md#dolphin_memoryscanresults) | Read a paginated committed result generation. |
| [`dolphin_memoryScanCancel`](capabilities.md#dolphin_memoryscancancel) | Request cancellation of a running scan job. |
| [`dolphin_memoryScanDispose`](capabilities.md#dolphin_memoryscandispose) | Release a scan and its snapshots/results. |
| [`dolphin_memoryScanUndo`](capabilities.md#dolphin_memoryscanundo) | Restore the previous retained result generation. |
| [`dolphin_memoryScanRemoveResults`](capabilities.md#dolphin_memoryscanremoveresults) | Remove explicit result addresses into a new immutable generation. |
| [`dolphin_resolvePointerChain`](capabilities.md#dolphin_resolvepointerchain) | Resolve a big-endian 32-bit pointer chain under one emulation pause. |

The listener admits at most two concurrent clients, including clients waiting
to send `initialize`. Additional connections are closed immediately; reconnect
after an existing session exits.

## Running the server

Build Dolphin's NoGUI target:

```bash
cmake -B build -DENABLE_NOGUI=ON -DENABLE_QT=OFF
cmake --build build --target dolphin-nogui
```

Choose one of these modes when starting Dolphin.

### Mode 1: Run the ISO

Use this mode to debug the game contained in the ISO:

```bash
dolphin-emu-nogui \
  -C Dolphin.General.DAPPort=5678 \
  --exec /path/to/game.iso \
  --platform headless
```

Dolphin executes the DOL stored in the ISO. Address breakpoints, instruction stepping,
registers, and memory tools work normally. Source stepping and locals require debug
information that matches this exact DOL, which retail ISOs usually do not contain.

An ELF supplied with `--debug-elf` is metadata only in this mode. It does not replace
the ISO's DOL. Use it only when its addresses exactly match the DOL in the ISO.

### Mode 2: Run a debug ELF with an ISO

Use this mode for source-level debugging of a decomp build:

```bash
dolphin-emu-nogui \
  -C Dolphin.General.DAPPort=5678 \
  -C 'Dolphin.Debug.SourcePaths=/path/to/project/src;/path/to/project/extern/dolphin/src' \
  -C Dolphin.Core.DefaultISO=/path/to/game.iso \
  -C Dolphin.Debug.ReplaceDiscExecutable=true \
  --exec /path/to/main.elf \
  --platform headless
```

Both files have a separate purpose:

- The ISO provides the disc bootstrap, game files, and filesystem environment.
- The DOL stored in the ISO is not executed.
- The ELF provides the executable code, symbols, and DWARF debug information.

Because Dolphin executes the ELF, its addresses match its debug information. For a
disc-based game, always provide both the ISO and ELF as shown above.

`Dolphin.Debug.SourcePaths` contains semicolon-separated source roots used to resolve
relative or basename-only paths in the ELF's DWARF data. Roots are checked in order;
the first root with a unique best suffix match wins. Set it before booting the ELF.
To persist it, set `SourcePaths` in the `[Debug]` section of `Dolphin.ini`.

Source stepping and locals do not work reliably inside optimized source files
(translation units).
Optimization can combine or remove source lines and variables, so stepping may skip
lines and locals may be missing or incorrect. Build the translation units you need to
debug without optimization and leave unrelated code optimized.

For code without DWARF, an `entrypoints.json` file beside the ELF can still provide
function names and definition lines.

### Connection options

The examples use TCP port `5678`. To use a Unix socket on Linux or macOS, replace the
port setting with:

```bash
-C Dolphin.General.DAPSocket=/tmp/dolphin-dap.sock
```

You can also persist either setting in `Dolphin.ini`:

```ini
[General]
DAPPort = 5678
# DAPSocket = /tmp/dolphin-dap.sock
```

To let the game run immediately instead of pausing when the debugger connects, add:

```bash
-C Dolphin.General.DAPStopOnEntry=false
```

The DAP client can override this setting with `stopOnEntry`. DAP and GDB are mutually
exclusive, so do not enable both at the same time.

## Configuring a DAP client

Configure your editor or debugger as a DAP client that connects to a server. The client
needs:

- The host and TCP port, such as `127.0.0.1:5678`, or the Unix socket path.
- An `attach` configuration when Dolphin was started separately.
- A `launch` configuration when the client starts Dolphin with one of the commands from
  [Running the server](#running-the-server).

Client configuration formats differ, but the connection is equivalent to:

```text
adapter: server
host: 127.0.0.1
port: 5678
request: attach
```

The client should send standard DAP requests. Dolphin-specific memory watches, freezes,
scans, and code injection require client support for the custom requests documented in
[`capabilities.md`](capabilities.md).

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

## Client integrations

Client installation and editor-specific configuration are maintained with each integration:

- [Neovim integration](https://github.com/LiveMindIO/dolphin-dap-nvim)
- [Visual Studio Code integration](https://github.com/LiveMindIO/dolphin-dap-vscode)

## Tests

Build and run the automated tests without starting a game:

```bash
cmake --build build --target unittests
```

## Known limitations

- Use one DAP client per running Dolphin instance. Multiple clients share the
  same breakpoints and watchpoints and can overwrite each other's settings.
- Realtime memory changes normally reach the client within about 50 ms. Multiple
  changes during one frame may be combined into one update.
- Frozen values block normal game writes immediately. Some hardware-driven writes
  may appear briefly before Dolphin restores the frozen value on the next frame.
  Clearing a freeze leaves its realtime watch active.

## Source debugging with DWARF

Dolphin supports MWCC/CodeWarrior DWARF 1.1 in two different ways.

### Executed ELF

This is the recommended mode for a fully linked decomp build. Start Dolphin with the
ELF as `--exec` and mount the game ISO with `Dolphin.Core.DefaultISO`, as shown in
[Mode 2](#mode-2-run-a-debug-elf-with-an-iso).

The ELF supplies both the running code and its debug information, so source lines,
breakpoints, globals, and variable addresses use the same memory layout. Dolphin loads
the ELF's debug information automatically.

### Sidecar ELF

Sidecar mode is useful for projects that are only partially decompiled. In this mode,
Dolphin executes the DOL from the ISO and loads debug information from a separate ELF:

```bash
dolphin-emu-nogui \
  -C Dolphin.General.DAPPort=5678 \
  --exec /path/to/game.iso \
  --debug-elf /path/to/main.elf \
  --platform headless
```

The sidecar ELF does not replace the DOL in the ISO. It can provide known types,
expandable structures, globals, and source information for decompiled code. This is
safe only when the sidecar ELF preserves the exact addresses used by the running DOL.
If linking the ELF moves code or data, breakpoints and variable values can refer to the
wrong memory.

Sidecar debug information can also be loaded with `Dolphin.Debug.ELFFile` or
**Symbols → Load DWARF/Debug Info…** in the Qt interface.

### Debug information limits

The top stack frame exposes `Locals` and `Globals`. You can inspect pointers, fixed-size
arrays, structures, and unions, and expand nested values. Values are read-only and
usually displayed in hexadecimal. Very deeply nested or extremely large values are
limited, and variables from older stack frames are not currently available.

Source stepping and locals are not reliable for optimized source files. The compiler
may remove variables, reuse their storage, or combine source lines. Compile the files
you need to debug without optimization; unrelated files can remain optimized.

An **`entrypoints.json`** file beside the ELF can add function names and definition
lines for code without DWARF. It does not provide locals, structures, globals, or
line-by-line stepping inside those functions. See
[`../../.ai-doc-reference/entrypoints-format.md`](../../.ai-doc-reference/entrypoints-format.md).

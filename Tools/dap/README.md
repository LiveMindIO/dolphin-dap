# Dolphin DAP

Start the headless emulator with the DAP server listening on a TCP port:

```bash
dolphin-emu-nogui -C Main.General.DAPPort=5678 --exec /path/to/game.iso --platform headless
```

Or persist in `Dolphin.ini`:

```ini
[General]
DAPPort = 5678
```

On Linux, a Unix socket takes priority over the port (mirrors `GDBSocket`):

```ini
DAPSocket = /tmp/dolphin-dap.sock
```

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

## Phase 1 commands (MVP)

After `configurationDone`, the session emits a `stopped` event (`reason: entry`).
Supported requests include `continue`, `pause`, `stepIn`, `next`, `stepOut`,
`setBreakpoints`, `scopes`, `variables`, `readMemory`, `writeMemory`, and
`disassemble`. Breakpoint addresses are hex strings in `source.name` or
`source.path` (optional `line` × 4 offset).

## Phase 2

Step over/out mirror `CodeWidget::StepOver` / `CodeWidget::StepOut` (temporary
breakpoint for branch step-over; interpreter loop for step-out). `setVariable`
writes GPRs and PC-scope registers (`pc`, `lr`, `ctr`, `cr`, `xer`) exposed by
`variables`. `threads` and `stackTrace` expose the emulated PPC thread and call
stack. Conditional breakpoints (`setBreakpoints` `condition`), memory watchpoints
(`setDataBreakpoints` with hex `dataId`), and `evaluate` for PPC expressions are
supported.

## Phase 3

Execution-control completeness and richer stop semantics:
`setInstructionBreakpoints` sets code breakpoints from hex `instructionReference`
values; `gotoTargets`/`goto` move the PC to an address; `stopped` events classify
the reason (`breakpoint`, `data breakpoint`, `step`) and carry `hitBreakpointIds`;
`exceptionInfo` reports pending PPC exceptions.

## Phase 4 (in progress)

Source awareness and session lifecycle: `loadedSources` lists real source files
when DWARF line info is loaded (otherwise hex-address disassembly pseudo-sources);
`source` returns file contents or disassembly; `breakpointLocations` maps source
lines to addresses via the line table; stack frames carry `source`/`line` from
DWARF when available; `terminate` pauses emulation and `restart` resets PPC state.

Import DWARF 1.1 debug info (MWCC `.debug`/`.line`) via **Symbols → Load DWARF/Debug
Info…** in the Qt UI, booting a debug ELF, or the sidecar path:

```bash
dolphin-emu-nogui \
  -C Main.General.DAPPort=5678 \
  --debug-elf /path/to/main.elf \
  --exec /path/to/game.iso \
  --platform headless
```

(`-C Main.Debug.DwarfElf=...` is equivalent.) See `.ai-doc-reference/dwarf-1.1-format.md`.

Requests are parsed with picojson (the project's JSON library) into typed models
in `DapProtocol`. `memoryReference` and addresses are hex strings; `readMemory`/
`writeMemory` carry data as base64. `readMemory` reports `unreadableBytes` when a
range runs past valid memory, and `writeMemory` fails unless `allowPartial` is set
when only part of the range is writable.

GDB and DAP are mutually exclusive — do not set `GDBPort`/`GDBSocket` at the same time.

## Automated tests

The DAP server is validated by GoogleTest suites under
`Source/UnitTests/Core/Debugger/DAP/` and `Source/UnitTests/Core/Debugger/DWARF/`.
None of them require an ISO, a booted game, or the JIT — they follow the same
pattern as `PageFaultTest` / `PageTableHostMappingTest`: initialize just the
memory subsystem, declare the test thread as the CPU thread, and drive PPC state
directly (with address translation off, so effective addresses map straight to
physical RAM).

| Suite | Layer | What it covers |
|-------|-------|----------------|
| `DapFramingTest` | transport | `Content-Length` framing encode/decode |
| `DapJsonTest` | JSON | picojson parsing, hex addresses, base64 |
| `DapProtocolTest` | protocol | request parsing + response/event building |
| `DapControllerTest` | core integration | `DapDebugController` against a real `Core::System`: register read/write, memory read/write (incl. partial/invalid), disassembly, breakpoints, DWARF source mapping |
| `DapSessionTest` | end-to-end | full `RunSession` command loop over a `socketpair`: handshake, `setBreakpoints`, `readMemory`, `writeMemory`, `disassemble`, `variables`, `setVariable`, unknown-command error |
| `DwarfReaderTest` | DWARF parser | DWARF 1.1 `.debug`/`.line` parsing, malformed input |
| `PPCSymbolDBLineTest` | symbol DB | line table queries, `ImportDwarf`, `Clear` |

`DapSessionTest` connects a `socketpair` to `RunSession` running on a background
thread (which declares itself the CPU thread, so the controller's
`CPUThreadGuard`s are no-ops) and speaks real DAP over the socket — no TCP or
network. This is the layer that exercises framing + JSON + dispatch + event
serialization together.

Build and run:

```bash
cmake --build build --target tests
./build/Binaries/Tests/tests --gtest_filter='Dap*:Dwarf*:PPCSymbolDBLine*'
```

Note: the tests allocate Dolphin's memory arena via shared memory, so they must
run in an environment that permits it (some restrictive sandboxes raise
`SIGBUS` inside `Memory::Init`, which also affects the existing
`PageTableHostMappingTest`).

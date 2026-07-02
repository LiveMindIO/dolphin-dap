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
Supported requests include `continue`, `pause`, `stepIn`, `setBreakpoints`,
`scopes`, `variables`, `readMemory`, `writeMemory`, and `disassemble`. Breakpoint
addresses are hex strings in `source.name` or `source.path` (optional `line` × 4
offset).

Requests are parsed with picojson (the project's JSON library) into typed models
in `DapProtocol`. `memoryReference` and addresses are hex strings; `readMemory`/
`writeMemory` carry data as base64. `readMemory` reports `unreadableBytes` when a
range runs past valid memory, and `writeMemory` fails unless `allowPartial` is set
when only part of the range is writable.

GDB and DAP are mutually exclusive — do not set `GDBPort`/`GDBSocket` at the same time.

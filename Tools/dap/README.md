# Dolphin DAP — Phase 0

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

GDB and DAP are mutually exclusive — do not set `GDBPort`/`GDBSocket` at the same time.

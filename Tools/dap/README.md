# Dolphin DAP

This folder documents the **Debug Adapter Protocol server** that exposes
Dolphin's PowerPC debugger to DAP-aware clients (VS Code, Cursor, Neovim, etc.).

Start below to build Dolphin, choose what to run, and connect a debugger.
For supported requests and their payloads, see [`capabilities.md`](capabilities.md).
To debug in Dolphin's own Qt interface without an editor or DAP client, use the
[Qt source-debugging guide](qt-source-debugging.md).

## Table of contents

- [Running the server](#running-the-server)
- [Configuring a DAP client](#configuring-a-dap-client)
- [Handshake test after starting a game](#handshake-test-after-starting-a-game)
- [Client integrations](#client-integrations)
- [Tests](#tests)
- [Known limitations](#known-limitations)
- [Source debugging with DWARF](#source-debugging-with-dwarf)

## Running the server

Install the [platform build prerequisites](../../Readme.md#building)
first; Windows builds have [separate instructions](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Windows).
For a local Linux NoGUI build, run:

```bash
git clone --recurse-submodules https://github.com/LiveMindIO/dolphin-dap.git
cd dolphin-dap
cmake -B build -DENABLE_NOGUI=ON -DENABLE_QT=OFF -DLINUX_LOCAL_DEV=ON
cmake --build build --target dolphin-nogui
ln -s ../../Data/Sys build/Binaries/Sys
```

If you already have a checkout, run `git submodule update --init --recursive`
from its root before building. Skip the `ln` command if `build/Binaries/Sys` already
exists; it makes Dolphin's bundled resources available to this local build.
The commands below also run from the repository
root and use `./build/Binaries/dolphin-emu-nogui`; no installation or `PATH` change
is needed. For Windows, use your built `DolphinNoGUI.exe` and `--platform win32`.
Qt Dolphin uses its own executable and omits `--platform`.

The examples use `headless` (no game window). On Linux, use `--platform x11`
instead if your build supports X11 and you want to see the game.
Replace `/path/to/...` with absolute paths to your files.

Every debugging launch should include `-C Dolphin.Interface.DebugModeEnabled=True`.
This enables core breakpoint checks and debugger-aware stepping, including in the
NoGUI build. Configuring a DAP port or socket alone does not enable core debugging.
The override applies only to this launch; it does not open GUI panes in NoGUI.

### Mode 1: Run the ISO

Use this mode to debug the game contained in the ISO:

```bash
./build/Binaries/dolphin-emu-nogui \
  -C Dolphin.Interface.DebugModeEnabled=True \
  -C Dolphin.General.DAPSocket= \
  -C Dolphin.General.DAPPort=5678 \
  -C Dolphin.Debug.ELFFile= \
  -C Dolphin.Debug.ReplaceDiscExecutable=false \
  --exec /path/to/game.iso \
  --platform headless
```

Dolphin executes the DOL stored in the ISO. Address breakpoints, instruction stepping,
registers, and memory tools work normally. Source stepping and locals require debug
information that matches this exact DOL, which retail ISOs usually do not contain.

To add matching debug information without replacing the DOL, use
[sidecar mode](#sidecar-elf). The empty `ELFFile` override above clears any saved ELF.

### Mode 2: Run a debug ELF with an ISO

Use this mode for source-level debugging of a decomp build:

```bash
./build/Binaries/dolphin-emu-nogui \
  -C Dolphin.Interface.DebugModeEnabled=True \
  -C Dolphin.General.DAPSocket= \
  -C Dolphin.General.DAPPort=5678 \
  -C 'Dolphin.Debug.SourcePaths=/path/to/project/src;/path/to/project/libs/dolphin/src' \
  -C Dolphin.Debug.ELFFile=/path/to/main.elf \
  -C Dolphin.Debug.ReplaceDiscExecutable=true \
  --exec /path/to/game.iso \
  --platform headless
```

Both files have a separate purpose:

- The ISO provides the disc bootstrap, game files, and filesystem environment.
- The DOL stored in the ISO is not executed.
- The ELF provides the executable code, symbols, and DWARF debug information.

This matches the editor plugins' recommended launch: boot the ISO, then replace
its executable with the ELF. You can alternatively use `--exec /path/to/main.elf`
with `-C Dolphin.Core.DefaultISO=/path/to/game.iso` and replacement enabled.

`Dolphin.Debug.SourcePaths` contains semicolon-separated source roots used to resolve
relative or basename-only paths in the ELF's DWARF data. Roots are checked in order;
the first root with a unique best suffix match wins. Set it before booting the ELF.
To persist it, set `SourcePaths` in the `[Debug]` section of `Dolphin.ini`.
The example roots are for current Melee; use your project's directories (older
Melee checkouts may use `extern/dolphin/src`). Build a debug ELF from the same
sources you open in the editor. See [debug information limits](#debug-information-limits).

### Connection options

The examples use TCP port `5678`. To use a Unix socket on Linux or macOS, replace the
empty socket override and port setting with:

```bash
-C Dolphin.General.DAPSocket=/tmp/dolphin-dap.sock
```

Keep the core-debugging argument when switching from TCP to a Unix socket.
On Linux and macOS, a nonempty socket setting takes priority over the TCP port;
the TCP examples clear it explicitly so a saved socket cannot redirect the listener.
You can also persist core debugging and either connection setting in `Dolphin.ini`:

Close Dolphin before editing this file so shutdown does not overwrite your changes.
Its location depends on the active configuration:

- Linux's default XDG setup: `~/.config/dolphin-emu/Dolphin.ini`, or
  `$XDG_CONFIG_HOME/dolphin-emu/Dolphin.ini` if that environment variable is set.
- Older Linux setups: `~/.dolphin-emu/Config/Dolphin.ini` if that user folder exists.
- Windows and macOS: `Config/Dolphin.ini` inside the active user folder. Qt's
  **File → Open User Folder** locates that folder. On XDG Linux, this menu opens
  the data folder, not the separate configuration folder listed above.
- For a predictable NoGUI location, add `--user /absolute/path/to/dolphin-user`
  to every Dolphin launch. After the first run, edit
  `/absolute/path/to/dolphin-user/Config/Dolphin.ini`. This selects a separate user
  directory, including its saves and other settings; keep using it when debugging.

```ini
[Interface]
DebugModeEnabled = True

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

## Handshake test after starting a game

Start a game using one of the TCP commands above, without connecting an editor.
Keep Dolphin running while you execute this in another terminal. The listener is
created during game startup; opening Dolphin without booting a game is not enough.
This checks transport only, not source debugging, and closes its connection afterward.

```bash
python3 - <<'PY'
import json, socket
msg = json.dumps({"seq": 1, "type": "request", "command": "initialize",
                  "arguments": {"clientID": "test", "adapterID": "dolphin-dap"}}).encode()
with socket.create_connection(("127.0.0.1", 5678), timeout=5) as s:
    s.sendall(f"Content-Length: {len(msg)}\r\n\r\n".encode() + msg)
    with s.makefile("rb") as stream:
        headers = {}
        while (line := stream.readline()) not in (b"\r\n", b""):
            key, value = line.decode().split(":", 1)
            headers[key.lower()] = value.strip()
        response = json.loads(stream.read(int(headers["content-length"])))
        print(json.dumps(response, indent=2))
        assert response["command"] == "initialize" and response["success"]
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
cmake -B build -DENABLE_TESTS=ON
cmake --build build --target tests
./build/Binaries/Tests/tests
```

These commands run from the repository root. On Windows, run the generated
`tests.exe` from your build's test output directory instead.

## Known limitations

- Use one DAP client per running Dolphin instance. Multiple clients share the
  guest state and watchpoints; memory edits or execution commands affect every client.
  The listener admits at most two clients; additional connections are closed.
- Realtime memory changes normally reach the client within about 50 ms. Multiple
  changes during one frame may be combined into one update.
- Frozen values block normal game writes immediately. Some hardware-driven writes
  may appear briefly before Dolphin restores the frozen value on the next frame.
  Clearing a freeze leaves its realtime watch active.

## Source debugging with DWARF

Dolphin supports MWCC/CodeWarrior DWARF 1.1 in two different ways.

### Executed ELF

For a fully linked decomp build, use [Mode 2](#mode-2-run-a-debug-elf-with-an-iso).
The ELF supplies both the executed code and its debug information, keeping addresses
aligned. No separate debug-information loading step is needed.

### Sidecar ELF

Sidecar mode is useful for projects that are only partially decompiled. In this mode,
Dolphin executes the DOL from the ISO and loads debug information from a separate ELF:

```bash
./build/Binaries/dolphin-emu-nogui \
  -C Dolphin.Interface.DebugModeEnabled=True \
  -C Dolphin.General.DAPSocket= \
  -C Dolphin.General.DAPPort=5678 \
  -C Dolphin.Debug.ReplaceDiscExecutable=false \
  -C 'Dolphin.Debug.SourcePaths=/path/to/project/src;/path/to/project/libs/dolphin/src' \
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
arrays, structures, and unions, and expand nested values. Supported scalar variables
can be edited; const values and unsupported locations remain read-only. Values are
usually displayed in hexadecimal. Very deeply nested or extremely large values are
limited, and variables from older stack frames are not currently available.

Source stepping and locals are not reliable for optimized source files. The compiler
may remove variables, reuse their storage, or combine source lines. Compile the files
you need to debug without optimization; unrelated files can remain optimized.
After changing sources, rebuild the ELF before debugging. If the Qt debugger warns
that a DWARF line does not match the source and falls back to disassembly, check that
the source roots point at the right checkout and rebuild its ELF.

An **`entrypoints.json`** file beside the ELF can add function names and definition
lines for code without DWARF. It does not provide locals, structures, globals, or
line-by-line stepping inside those functions. See
[`../../.ai-doc-reference/entrypoints-format.md`](../../.ai-doc-reference/entrypoints-format.md).

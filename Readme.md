# Dolphin - A GameCube and Wii Emulator

[Homepage](https://dolphin-emu.org/) | [Project Site](https://github.com/dolphin-emu/dolphin) | [Buildbot](https://dolphin.ci/) | [Forums](https://forums.dolphin-emu.org/) | [Wiki](https://wiki.dolphin-emu.org/) | [GitHub Wiki](https://github.com/dolphin-emu/dolphin/wiki) | [Issue Tracker](https://bugs.dolphin-emu.org/projects/emulator/issues) | [Coding Style](https://github.com/dolphin-emu/dolphin/blob/master/Contributing.md) | [Transifex Page](https://app.transifex.com/dolphinemu/dolphin-emu/dashboard/) | [Analytics](https://mon.dolphin-emu.org/)

Dolphin is an emulator for running GameCube and Wii games on Windows,
Linux, macOS, and recent Android devices. It's licensed under the terms
of the GNU General Public License, version 2 or later (GPLv2+).

Please read the [FAQ](https://dolphin-emu.org/docs/faq/) before using Dolphin.

---

## Debug Adapter Protocol (DAP) Server

This fork of Dolphin adds a **Debug Adapter Protocol server** that exposes
Dolphin's PowerPC debugger to DAP-aware clients (VS Code, Cursor, Neovim, etc.).
It mirrors the behavior of Dolphin's built-in Qt debugger — breakpoints,
watchpoints, stepping, register/memory inspection — but drives them over a
socket from an external editor instead of from the in-app UI. The result is a
fully scriptable, headless reverse-engineering and TAS-debugging workflow with
the emulator as the backend and your editor as the front-end.

Activation mirrors the GDB stub: there is no `--dap` flag and no separate
binary. The server is **inert** unless a DAP port/socket is configured at
runtime. DAP and GDB are mutually exclusive (only one may be active at a time).

### Capabilities

The server speaks the **Debug Adapter Protocol** (the same protocol VS Code's
debuggers use). Standard requests implemented:

| Capability | Request(s) | Notes |
|------------|------------|-------|
| Launch / attach | `launch`, `attach` | Emulation boots on `launch`; `attach` reuses a running core. |
| Continue / pause | `continue`, `pause` | Single emulated thread; `allThreadsContinued` reported. |
| Stepping | `next`, `stepIn`, `stepOut` | Forces interpreter mode while stepping. |
| Code breakpoints | `setBreakpoints`, `setInstructionBreakpoints` | Conditional breakpoints supported via `condition`. |
| Watchpoints | `setDataBreakpoints` | Reads/writes pause the core. **Ranged** watchpoints supported via a `length` extension (see below). |
| Evaluation | `evaluate`, `setVariable`, `scopes`, `variables` | PPC debugger expression syntax (same as breakpoint conditions). |
| Stack / threads | `stackTrace`, `threads` | OS thread enumeration + PPC call stack. |
| Memory I/O | `readMemory`, `writeMemory` | Base64-encoded payloads. |
| Disassembly | `disassemble` | Per-instruction; `instructionReference` carries the address. |
| Goto | `goto`, `gotoTargets` | Set PC and re-emit a `stopped` event. |
| Exception info | `exceptionInfo` | Reports pending PPC exceptions. |
| Sources | `loadedSources`, `source`, `breakpointLocations` | DWARF 1.1 line tables + entrypoints sidecar. |
| Lifecycle | `terminate`, `restart`, `configurationDone`, `disconnect` | |

Dolphin-specific custom requests (vendor-prefixed `dolphin_`, not part of the
DAP spec — clients must opt in):

| Request | Purpose |
|---------|---------|
| `dolphin_realtimeWatch` | Subscribe to realtime memory-region changes. The adapter pushes a `dolphin_memoryChanged` event each frame whenever any byte in the watched region changes — **without pausing the debugger**. ~60 Hz (NTSC) / ~50 Hz (PAL). |
| `dolphin_realtimeWatchCancel` | Cancels a realtime watch subscription by `watchId`. |

### Running the DAP server

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

**Unix socket** (`Dolphin.General.DAPSocket`, Linux/macOS only):

```bash
dolphin-emu-nogui -C Dolphin.General.DAPSocket=./dap.sock \
  --exec ~/projects/ai/yolo/crowd-control/melee-iso/game.iso --platform headless
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

### Tests

```bash
cmake --build build --target tests
./build/Binaries/Tests/tests --gtest_filter='Dap*:RealtimeWatch*:Dwarf*:PPCSymbolDBLine*'
```

The integration tests (`DapControllerTest`, `DapSessionTest`,
`RealtimeWatchTest`) follow `PageFaultTest`/`PageTableHostMappingTest`: they use
the `Core::System` singleton, `Memory::Init()` + `AddressSpace::Init()`,
`DeclareAsCPUThread()`, and disable address translation (`MSR.DR=0`) so
effective == physical RAM. `DapSessionTest` runs `RunSession` on a thread that
declares itself the CPU thread and drives it over a `socketpair`.

> The memory arena uses shared memory; restrictive sandboxes raise `SIGBUS` in
> `Memory::Init` (also breaks `PageTableHostMappingTest`) — run tests
> unsandboxed.

### Request / response payload reference

All messages use DAP framing: `Content-Length: N\r\n\r\n` followed by a JSON
body. Sequence numbers (`seq`) are assigned by the client for requests and by
the server for responses/events.

#### `initialize` → capabilities

```jsonc
// → client request
{"seq": 1, "type": "request", "command": "initialize", "arguments": {}}

// ← server response (excerpt)
{"seq": 2, "type": "response", "command": "initialize", "request_seq": 1,
 "success": true,
 "body": {"capabilities": {
   "supportsConfigurationDoneRequest": true,
   "supportsDisassembleRequest": true,
   "supportsReadMemoryRequest": true,
   "supportsWriteMemoryRequest": true,
   "supportsSetVariable": true,
   "supportsStackTraceRequest": true,
   "supportsDataBreakpoints": true,
   "supportsInstructionBreakpoints": true,
   "supportsGotoTargetsRequest": true,
   "supportsEvaluateForHovers": true,
   "supportsExceptionInfoRequest": true,
   "supportsLoadedSourcesRequest": true,
   "supportsRestartRequest": true
 }}}

// ← then an "initialized" event (means "ready for setBreakpoints / launch")
{"seq": 3, "type": "event", "event": "initialized", "body": {}}
```

#### `launch` / `attach`

```jsonc
{"command": "launch", "arguments": {}}      // boot the configured ISO/DOL, then stop at entry
{"command": "attach", "arguments": {}}      // attach to an already-running core
```

Response is empty `{}`. Server then emits a `stopped` event with
`reason: "entry"` (launch) or `reason: "attach"`.

**`stopOnEntry`** (standard DAP field): controls whether the core breaks at
entry. While a DAP debugger is attached the core starts paused
(see `CPUSetInitialExecutionState` in `Core.cpp`); with `stopOnEntry: true`
the server emits a `stopped`/`entry` (or `stopped`/`attach`) event and the
client proceeds to set breakpoints. With `stopOnEntry: false` the server skips
the entry stop and resumes the core — the client receives a `continued` event
(via the CPU state hook) instead, and the game runs immediately.

When the field is **omitted** from the request, the session falls back to the
`Dolphin.General.DAPStopOnEntry` config (default `true`, set at dolphin launch
via `-C Dolphin.General.DAPStopOnEntry=false`). This lets you configure the
default once at startup without the client having to pass it on every
`launch`. An explicit `stopOnEntry: true`/`false` on the request overrides the
config for that session.

```jsonc
{"command": "launch", "arguments": {"stopOnEntry": false}}  // start running, don't pause at entry
```

#### `configurationDone`

Concludes the launch handshake. Server emits a `stopped`/`"entry"` event if it
hasn't already.

#### Continue / pause / step

```jsonc
{"command": "continue",  "arguments": {"threadId": 1}}
//  → {"command": "continue", "body": {"allThreadsContinued": true}}
//  → {"event": "continued", "body": {"threadId": 1, "allThreadsContinued": true}}

{"command": "pause",     "arguments": {"threadId": 1}}
//  → {"command": "pause"}
//  → {"event": "stopped", "body": {"reason": "pause", "threadId": 1}}

{"command": "next",      "arguments": {"threadId": 1}}   // step over
{"command": "stepIn",    "arguments": {"threadId": 1}}
{"command": "stepOut",   "arguments": {"threadId": 1}}
//  → {"event": "stopped", "body": {"reason": "step", "threadId": 1}}
```

On a spontaneous stop (breakpoint hit / watchpoint hit / step completion) the
reason is classified: `"breakpoint"`, `"data breakpoint"`, or `"step"`.
`hitBreakpointIds` is populated for code breakpoints.

#### `setBreakpoints` (source / line)

A Dolphin "source" is anchored at an address encoded as a hex string in
`source.name` or `source.path`; each breakpoint's address is `base + line*4`.

```jsonc
{"command": "setBreakpoints", "arguments": {
  "source": {"name": "0x80003100"},
  "breakpoints": [
    {"line": 0,  "condition": "r3 == 0"},
    {"line": 4}
  ]
}}
//  → {"breakpoints": [
//       {"verified": true, "instructionReference": "0x80003100"},
//       {"verified": true, "instructionReference": "0x80003104"}
//     ]}
```

#### `setInstructionBreakpoints`

Directly sets code breakpoints by address. Replaces the whole code-breakpoint
list (mirrors the GDB stub).

```jsonc
{"command": "setInstructionBreakpoints", "arguments": {
  "breakpoints": [
    {"instructionReference": "0x80003100", "offset": 0, "condition": "r4 == 0x10"}
  ]
}}
```

#### `setDataBreakpoints` — watchpoints (including ranged)

A DAP **data breakpoint** is a memory watchpoint: it pauses the core when the
specified address is read or written. `accessType` is one of
`"read"` / `"write"` / `"readWrite"` (default `"readWrite"`).

```jsonc
// single-byte write watchpoint at 0x80004000
{"command": "setDataBreakpoints", "arguments": {
  "breakpoints": [{"dataId": "0x80004000", "accessType": "write"}]
}}
```

**Ranged watchpoints (Dolphin extension).** The DAP spec has no notion of a
watchpoint region — `dataId` is a single address. Dolphin's `TMemCheck` natively
supports ranges, so this server accepts an optional **`length`** field on each
data breakpoint. When `length > 1`, the controller installs a ranged watchpoint
over `[address, address+length-1]`. Omitting `length` (or `length: 1`) preserves
standard single-byte behavior — spec-compliant clients are unaffected.

```jsonc
// watch the 0x100-byte region starting at 0xdeadb33f for writes
{"command": "setDataBreakpoints", "arguments": {
  "breakpoints": [{"dataId": "0xdeadb33f", "accessType": "write", "length": 256}]
}}
```

Like the standard request, `setDataBreakpoints` is authoritative: it clears all
existing watchpoints and installs the new set. Conditional watchpoints are
supported via `condition`:

```jsonc
{"breakpoints": [{"dataId": "0x80004000", "accessType": "readWrite",
                  "length": 16, "condition": "r5 == 0"}]}
```

On a watchpoint hit, the server emits:

```jsonc
{"event": "stopped", "body": {"reason": "data breakpoint", "threadId": 1}}
```

#### `readMemory` / `writeMemory`

```jsonc
// read 4 bytes at 0x80004000
{"command": "readMemory", "arguments": {"memoryReference": "0x80004000", "count": 4}}
//  → {"address": "0x80004000", "data": "AAAA"}   // base64

// write 3 bytes ("Man") at 0x80004000
{"command": "writeMemory", "arguments":
  {"memoryReference": "0x80004000", "data": "TWFu"}}
//  → {"bytesWritten": 3, "offset": 0}

// partial write (don't fail at the first invalid address)
{"command": "writeMemory", "arguments":
  {"memoryReference": "0x80004000", "data": "TWFu", "allowPartial": true}}
```

`readMemory` reports `unreadableBytes` when the region extends past valid RAM.
The read stops at the first invalid address.

#### `disassemble`

```jsonc
{"command": "disassemble", "arguments":
  {"memoryReference": "0x80003100", "instructionCount": 4}}
//  → {"instructions": [
//       {"address": "0x80003100", "instruction": "nop"},
//       {"address": "0x80003104", "instruction": "blr"},
//       ...
//     ]}
```

#### `stackTrace` / `threads` / `scopes` / `variables` / `setVariable`

```jsonc
{"command": "stackTrace", "arguments": {"threadId": 1, "startFrame": 0, "levels": 20}}
{"command": "threads"}
{"command": "scopes", "arguments": {"frameId": 0}}     // → ["Registers", "PC"]
{"command": "variables", "arguments": {"variablesReference": 1000}}  // 1000 = Registers
{"command": "setVariable", "arguments":
  {"variablesReference": 1000, "name": "r3", "value": "0x12345678"}}
```

Frame `source.path`/`source.name` carry either a real source file path (when
DWARF/entrypoints line info is loaded) or a hex anchor address for a
disassembly pseudo-source.

#### `evaluate`

```jsonc
{"command": "evaluate", "arguments": {"expression": "r3 + r4"}}
//  → {"result": "0x000004d2", "type": "string"}
```

Uses the PPC debugger expression syntax — the same evaluator that handles
breakpoint conditions.

#### `goto` / `gotoTargets`

```jsonc
{"command": "gotoTargets", "arguments": {"source": {"name": "0x80003100"}, "line": 0}}
//  → {"targets": [{"id": 2147501824, "label": "0x80003100",
//                  "instructionPointerReference": "0x80003100"}]}

{"command": "goto", "arguments": {"threadId": 1, "targetId": 2147501824}}
//  → {}  then a stopped/event with reason "goto"
```

The address doubles as the target id so `goto` is stateless.

#### `loadedSources` / `source` / `breakpointLocations`

```jsonc
{"command": "loadedSources"}
//  → {"sources": [{"sourceReference": 1, "name": "...", "path": "..."}, ...]}

{"command": "source", "arguments": {"sourceReference": 1, "startLine": 0, "endLine": -1}}
{"command": "breakpointLocations",
 "arguments": {"source": {"name": "0x80003100"}, "line": 0, "endLine": 20}}
```

#### `exceptionInfo`

```jsonc
{"command": "exceptionInfo", "arguments": {"threadId": 1}}
//  → {"exceptionId": "0x00000000", "description": "...", "breakMode": "always"}
```

#### `terminate` / `restart` / `disconnect`

```jsonc
{"command": "terminate"}          // → emits {"event": "terminated", "body": {"restart": false}}
{"command": "restart"}           // → core reset, then a stopped/"restart" event
{"command": "disconnect"}         // → session ends, sockets torn down
```

### Custom (Dolphin-specific) requests

These requests are not part of the DAP spec and are prefixed `dolphin_`. A
DAP-aware editor that wants to use them needs a small client-side extension
(e.g. a custom VS Code command or a debug-adapter extension).

#### `dolphin_realtimeWatch` — realtime memory change streaming

Subscribes to changes in a memory region. Unlike `setDataBreakpoints` (which
**pauses** on access), a realtime watch **streams** the new value to the client
on every change without halting emulation. Sampling happens at field rate
(~60 Hz NTSC / ~50 Hz PAL) via the `vi_end_field_event` CPU-thread hook — the
same signal the Qt `MemoryViewWidget` and `CheatsManager` use — so the watch
never stalls the core. Changes are diffed against the last-seen snapshot; only
genuinely changed regions emit events, and the initial subscribe is seeded with
the current contents so the first frame doesn't echo back as a spurious change.

```jsonc
// subscribe
{"command": "dolphin_realtimeWatch",
 "arguments": {"memoryReference": "0xdeadb33f", "count": 256}}
//  → {"watchId": 1, "address": "0xdeadb33f", "count": 256}
```

Each frame in which any byte in `[address, address+count)` differs from the
previous value, the server pushes:

```jsonc
{"event": "dolphin_memoryChanged",
 "body": {"watchId": 1, "address": "0xdeadb33f",
          "count": 256, "data": "<base64 of the new region contents>"}}
```

If part of the region is unreadable (extends past valid RAM), the `data`
payload is truncated to the readable prefix; `count` still reports the
originally requested length so the client can tell where the unreadable tail
begins.

Multiple watches are independent — each gets its own `watchId`. A region that
never changes never emits.

#### `dolphin_realtimeWatchCancel` — stop a realtime watch

```jsonc
{"command": "dolphin_realtimeWatchCancel", "arguments": {"watchId": 1}}
//  → {}   on success
//  → error response with message "no such watch" if watchId is unknown
```

After cancellation the region is no longer sampled; no further
`dolphin_memoryChanged` events are emitted for that `watchId`.

### Source awareness (DWARF 1.1 + entrypoints)

When DWARF 1.1 line info (MWCC/CodeWarrior `.debug`+`.line` sections) is loaded,
`stackTrace`, `loadedSources`, `source`, and `breakpointLocations` return real
file:line mappings. Loading happens automatically when booting a debug ELF
(`ElfReader::LoadSymbols`), programmatically via `Core::Debug::ImportDwarf` /
`ImportDwarfFromElf`, or as a sidecar via `Dolphin.Debug.DwarfElf` (or
`--debug-elf`).

Retail-linked units that omit MWCC DWARF can still expose function entrypoints
+ definition lines via an **`entrypoints.json`** sidecar (normalized; planned:
Melee `tools/entrypoints.py` producer, Dolphin `ImportEntrypointsFromJson`
importer). This supplies entrypoint-level line info without faking body-level
line tables. See [`.ai-doc-reference/entrypoints-format.md`](.ai-doc-reference/entrypoints-format.md).

---

## System Requirements

### Desktop

* OS
    * Windows (10 1903 or higher).
    * Linux.
    * macOS (11.0 Big Sur or higher).
    * Unix-like systems other than Linux are not officially supported but might work.
* Processor
    * A CPU with SSE2 support.
    * A modern CPU (3 GHz and Dual Core, not older than 2008) is highly recommended.
* Graphics
    * A reasonably modern graphics card (Direct3D 11.1 / OpenGL 3.3).
    * A graphics card that supports Direct3D 11.1 / OpenGL 4.4 is recommended.

### Android

* OS
    * Android (7.0 Nougat or higher).
* Processor
    * A processor with support for 64-bit applications (either ARMv8 or x86-64).
* Graphics
    * A graphics processor that supports OpenGL ES 3.0 or higher. Performance varies heavily with [driver quality](https://dolphin-emu.org/blog/2013/09/26/dolphin-emulator-and-opengl-drivers-hall-fameshame/).
    * A graphics processor that supports standard desktop OpenGL features is recommended for best performance.

Dolphin can only be installed on devices that satisfy the above requirements. Attempting to install on an unsupported device will fail and display an error message.

## Building for Windows

Use the solution file `Source/dolphin-emu.sln` to build Dolphin on Windows.
Dolphin targets the latest MSVC shipped with Visual Studio or Build Tools.
Other compilers might be able to build Dolphin on Windows but have not been
tested and are not recommended to be used. Git and latest Windows SDK must be
installed when building.

Make sure to pull submodules before building:
```sh
git submodule update --init --recursive
```

The "Release" solution configuration includes performance optimizations for the best user experience but complicates debugging Dolphin.
The "Debug" solution configuration is significantly slower, more verbose and less permissive but makes debugging Dolphin easier.

## Building for Linux and macOS

Dolphin requires [CMake](https://cmake.org/) for systems other than Windows. 
You need a recent version of GCC or Clang with decent c++20 support. CMake will
inform you if your compiler is too old.
Many libraries are bundled with Dolphin and used if they're not installed on 
your system. CMake will inform you if a bundled library is used or if you need
to install any missing packages yourself. You may refer to the [wiki](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Linux) for more information.

Make sure to pull submodules before building:
```sh
git submodule update --init --recursive
```

### macOS Build Steps:

A binary supporting a single architecture can be built using the following steps: 

1. `mkdir build`
2. `cd build`
3. `cmake ..`
4. `make -j $(sysctl -n hw.logicalcpu)`

An application bundle will be created in `./Binaries`.

A script is also provided to build universal binaries supporting both x64 and ARM in the same
application bundle using the following steps:

1. `mkdir build`
2. `cd build`
3. `python ../BuildMacOSUniversalBinary.py`
4. Universal binaries will be available in the `universal` folder

Doing this is more complex as it requires installation of library dependencies for both x64 and ARM (or universal library
equivalents) and may require specifying additional arguments to point to relevant library locations. 
Execute BuildMacOSUniversalBinary.py --help for more details.  

### Linux Global Build Steps:

To install to your system.

1. `mkdir build`
2. `cd build`
3. `cmake ..`
4. `make -j $(nproc)`
5. `sudo make install`

### Linux Local Build Steps:

Useful for development as root access is not required.

1. `mkdir Build`
2. `cd Build`
3. `cmake .. -DLINUX_LOCAL_DEV=true`
4. `make -j $(nproc)`
5. `ln -s ../../Data/Sys Binaries/`

### Linux Portable Build Steps:

Can be stored on external storage and used on different Linux systems.
Or useful for having multiple distinct Dolphin setups for testing/development/TAS.

1. `mkdir Build`
2. `cd Build`
3. `cmake .. -DLINUX_LOCAL_DEV=true`
4. `make -j $(nproc)`
5. `cp -r ../Data/Sys/ Binaries/`
6. `touch Binaries/portable.txt`

## Building for Android

These instructions assume familiarity with Android development. If you do not have an
Android dev environment set up, see [AndroidSetup.md](AndroidSetup.md).

Make sure to pull submodules before building:
```sh
git submodule update --init --recursive
```

If using Android Studio, import the Gradle project located in `./Source/Android`.

Android apps are compiled using a build system called Gradle. Dolphin's native component,
however, is compiled using CMake. The Gradle script will attempt to run a CMake build
automatically while building the Java code.

## Uninstalling

On Windows, simply remove the extracted directory, unless it was installed with the NSIS installer,
in which case you can uninstall Dolphin like any other Windows application.

Linux users can run `cat install_manifest.txt | xargs -d '\n' rm` as root from the build directory
to uninstall Dolphin from their system.

macOS users can simply delete Dolphin.app to uninstall it.

Additionally, you'll want to remove the global user directory if you don't plan on reinstalling Dolphin.

## Command Line Usage

```
Usage: Dolphin.exe [options]... [FILE]...

Options:
  --version             show program's version number and exit
  -h, --help            show this help message and exit
  -u USER, --user=USER  User folder path
  -m MOVIE, --movie=MOVIE
                        Play a movie file
  -e <file>, --exec=<file>
                        Load the specified file
  -n <16-character ASCII title ID>, --nand_title=<16-character ASCII title ID>
                        Launch a NAND title
  -C <System>.<Section>.<Key>=<Value>, --config=<System>.<Section>.<Key>=<Value>
                        Set a configuration option
  -s <file>, --save_state=<file>
                        Load the initial save state
  -d, --debugger        Show the debugger pane and additional View menu options
  -l, --logger          Open the logger
  -b, --batch           Run Dolphin without the user interface (Requires
                        --exec or --nand-title)
  -c, --confirm         Set Confirm on Stop
  -v VIDEO_BACKEND, --video_backend=VIDEO_BACKEND
                        Specify a video backend
  -a AUDIO_EMULATION, --audio_emulation=AUDIO_EMULATION
                        Choose audio emulation from [HLE|LLE]
```

Available DSP emulation engines are HLE (High Level Emulation) and
LLE (Low Level Emulation). HLE is faster but less accurate whereas
LLE is slower but close to perfect. Note that LLE has two submodes (Interpreter and Recompiler)
but they cannot be selected from the command line.

Available video backends are "D3D" and "D3D12" (they are only available on Windows), "OGL", and "Vulkan".
There's also "Null", which will not render anything, and
"Software Renderer", which uses the CPU for rendering and
is intended for debugging purposes only.

## DolphinTool Usage
```
usage: dolphin-tool COMMAND -h

commands supported: [convert, verify, header, extract]
```

```
Usage: convert [options]... [FILE]...

Options:
  -h, --help            show this help message and exit
  -u USER, --user=USER  User folder path, required for temporary processing
                        files.Will be automatically created if this option is
                        not set.
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -o FILE, --output=FILE
                        Path to the destination FILE.
  -f FORMAT, --format=FORMAT
                        Container format to use. Default is RVZ. [iso|gcz|wia|rvz]
  -s, --scrub           Scrub junk data as part of conversion.
  -b BLOCK_SIZE, --block_size=BLOCK_SIZE
                        Block size for GCZ/WIA/RVZ formats, as an integer.
                        Suggested value for RVZ: 131072 (128 KiB)
  -c COMPRESSION, --compression=COMPRESSION
                        Compression method to use when converting to WIA/RVZ.
                        Suggested value for RVZ: zstd [none|zstd|bzip|lzma|lzma2]
  -l COMPRESSION_LEVEL, --compression_level=COMPRESSION_LEVEL
                        Level of compression for the selected method. Ignored
                        if 'none'. Suggested value for zstd: 5
```

```
Usage: verify [options]...

Options:
  -h, --help            show this help message and exit
  -u USER, --user=USER  User folder path, required for temporary processing
                        files.Will be automatically created if this option is
                        not set.
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -a ALGORITHM, --algorithm=ALGORITHM
                        Optional. Compute and print the digest using the
                        selected algorithm, then exit. [crc32|md5|sha1|rchash]
```

```
Usage: header [options]...

Options:
  -h, --help            show this help message and exit
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -b, --block_size      Optional. Print the block size of GCZ/WIA/RVZ formats,
then exit.
  -c, --compression     Optional. Print the compression method of GCZ/WIA/RVZ
                        formats, then exit.
  -l, --compression_level
                        Optional. Print the level of compression for WIA/RVZ
                        formats, then exit.
```

```
Usage: extract [options]...

Options:
  -h, --help            show this help message and exit
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -o FOLDER, --output=FOLDER
                        Path to the destination FOLDER.
  -p PARTITION, --partition=PARTITION
                        Which specific partition you want to extract.
  -s SINGLE, --single=SINGLE
                        Which specific file/directory you want to extract.
  -l, --list            List all files in volume/partition. Will print the
                        directory/file specified with --single if defined.
  -q, --quiet           Mute all messages except for errors.
  -g, --gameonly        Only extracts the DATA partition.
```

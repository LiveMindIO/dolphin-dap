# DAP Server Capabilities

Operations supported by the Dolphin DAP server. Use the table of contents below
to find a request and its payload.

For build/run instructions, tests, known limitations, and source awareness, see
[`README.md`](README.md).

All messages use DAP framing: `Content-Length: N\r\n\r\n` followed by a JSON
body. Sequence numbers (`seq`) are assigned by the client for requests and by
the server for responses/events.

## Table of contents

- [Standard requests](#standard-requests)
  - [`initialize`](#initialize)
  - [`launch` / `attach`](#launch--attach)
  - [`configurationDone`](#configurationdone)
  - [`continue` / `pause` / `step`](#continue--pause--step)
  - [`setBreakpoints`](#setbreakpoints)
  - [`setInstructionBreakpoints`](#setinstructionbreakpoints)
  - [`setDataBreakpoints`](#setdatabreakpoints)
  - [`readMemory` / `writeMemory`](#readmemory--writememory)
  - [`disassemble`](#disassemble)
  - [`stackTrace` / `threads` / `scopes` / `variables` / `setVariable`](#stacktrace--threads--scopes--variables--setvariable)
  - [`evaluate`](#evaluate)
  - [`goto` / `gotoTargets`](#goto--gototargets)
  - [`loadedSources` / `source` / `breakpointLocations`](#loadedsources--source--breakpointlocations)
  - [`exceptionInfo`](#exceptioninfo)
  - [`terminate` / `restart` / `disconnect`](#terminate--restart--disconnect)
- [Dolphin-specific custom requests](#dolphin-specific-custom-requests)
  - [`dolphin_realtimeWatch`](#dolphin_realtimewatch)
  - [`dolphin_realtimeWatchCancel`](#dolphin_realtimewatchcancel)
  - [`dolphin_freeze`](#dolphin_freeze)
  - [`dolphin_unfreeze`](#dolphin_unfreeze)
  - [`dolphin_findFreeMemory`](#dolphin_findfreememory)
  - [`dolphin_injectCode`](#dolphin_injectcode)
  - [`dolphin_detour`](#dolphin_detour)
  - [`dolphin_memoryRegions`](#dolphin_memoryregions)
  - [`dolphin_memoryScanStart`](#dolphin_memoryscanstart)
  - [`dolphin_memoryScanRefine`](#dolphin_memoryscanrefine)
  - [`dolphin_memoryScanStatus`](#dolphin_memoryscanstatus)
  - [`dolphin_memoryScanResults`](#dolphin_memoryscanresults)
  - [`dolphin_memoryScanCancel`](#dolphin_memoryscancancel)
  - [`dolphin_memoryScanDispose`](#dolphin_memoryscandispose)
  - [`dolphin_memoryScanUndo`](#dolphin_memoryscanundo)
  - [`dolphin_memoryScanRemoveResults`](#dolphin_memoryscanremoveresults)
  - [`dolphin_resolvePointerChain`](#dolphin_resolvepointerchain)

# Standard requests

## `initialize`

Capability handshake. The server advertises which standard and
Dolphin-specific extensions it supports.

```jsonc
// → client request
{"seq": 1, "type": "request", "command": "initialize", "arguments": {}}

// ← server response (excerpt)
{"seq": 2, "type": "response", "command": "initialize", "request_seq": 1,
 "success": true,
  "body": {
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
   "supportsRestartRequest": true,
   "supportsDolphinRealtimeWatch": true,
   "supportsDolphinFreeze": true,
   "supportsDolphinFindFreeMemory": true,
   "supportsDolphinInjectCode": true,
   "supportsDolphinDetour": true,
   "supportsDolphinMemoryRegions": true,
   "supportsDolphinMemoryScan": true,
   "supportsDolphinPointerChain": true
  }}

// ← then an "initialized" event (means "ready for setBreakpoints / launch")
{"seq": 3, "type": "event", "event": "initialized", "body": {}}
```

## `launch` / `attach`

```jsonc
{"command": "launch", "arguments": {}}      // begin debugging the game already booted by Dolphin
{"command": "attach", "arguments": {}}      // attach to an already-running core
```

Response is empty `{}`. Neither request selects a game or starts a Dolphin process;
configure and boot the game through Dolphin's CLI or GUI first. With stop-on-entry
enabled, after configuration completes the server emits a `stopped` event with
`reason: "entry"` (launch) or `reason: "attach"`.

**`stopOnEntry`** (standard DAP field): controls whether the game pauses before
it begins. With `stopOnEntry: true`, the client can set breakpoints before
continuing. With `stopOnEntry: false`, the game runs immediately and the client
receives a `continued` event.

When the field is **omitted** from the request, the session falls back to the
`Dolphin.General.DAPStopOnEntry` config (default `true`, set at dolphin launch
via `-C Dolphin.General.DAPStopOnEntry=false`). This lets you configure the
default once at startup without the client having to pass it on every
`launch`. An explicit `stopOnEntry: true`/`false` on the request overrides the
config for that session.

```jsonc
{"command": "launch", "arguments": {"stopOnEntry": false}}  // start running, don't pause at entry
```

## `configurationDone`

Concludes the launch handshake. Server emits a `stopped`/`"entry"` event if it
hasn't already.

## `continue` / `pause` / `step`

```jsonc
{"command": "continue",  "arguments": {"threadId": 1}}
//  → {"command": "continue", "body": {"allThreadsContinued": true}}
//  → {"event": "continued", "body": {"threadId": 1, "allThreadsContinued": true}}

{"command": "pause",     "arguments": {"threadId": 1}}
//  → {"command": "pause"}
//  → {"event": "stopped", "body": {"reason": "pause", "threadId": 1}}

{"command": "next",      "arguments": {"threadId": 1, "granularity": "line"}}
{"command": "stepIn",    "arguments": {"threadId": 1, "granularity": "instruction"}}
{"command": "stepOut",   "arguments": {"threadId": 1}}
//  → {"event": "stopped", "body": {"reason": "step", "threadId": 1}}
```

On a spontaneous stop (breakpoint hit / watchpoint hit / step completion) the
reason is classified: `"breakpoint"`, `"data breakpoint"`, or `"step"`.
`hitBreakpointIds` is populated for code breakpoints.

Omitted, `"statement"`, and `"line"` granularity run asynchronously in
interpreter mode until the `(source file, line)` changes. `next` consumes linking
calls through their return address while `stepIn` enters them. `"instruction"`
preserves single-opcode stepping. When the current PC has no source row, source
stepping falls back to one opcode (`stepIn`) or one logical call-over (`next`).
Source stepping checks code breakpoints after every opcode and is bounded by a
5-second deadline and 1000000 instructions; continue, pause, and disconnect cancel
an in-flight step, and overlapping step requests are rejected.

## `setBreakpoints`

For a real source file, provide its path and one-based source lines. Dolphin
resolves them using loaded DWARF or entrypoint information. Lines without a
resolvable address are returned as unverified. Each request replaces this
client's breakpoints for that source, not breakpoints belonging to other sources
or clients.

```jsonc
{"command": "setBreakpoints", "arguments": {
  "source": {"path": "/path/to/project/src/example.c"},
  "breakpoints": [{"line": 42, "condition": "r3 == 0"}]
}}
```

For a disassembly pseudo-source, encode the base address as a hex string in
`source.name` or `source.path`. Its lines are also one-based; the address is
`base + (line - 1) * 4`. Line zero is invalid.

```jsonc
{"command": "setBreakpoints", "arguments": {
  "source": {"name": "0x80003100"},
  "breakpoints": [
    {"line": 1, "condition": "r3 == 0"},
    {"line": 2}
  ]
}}
//  → {"breakpoints": [
//       {"verified": true, "instructionReference": "0x80003100"},
//       {"verified": true, "instructionReference": "0x80003104"}
//     ]}
```

## `setInstructionBreakpoints`

Directly sets code breakpoints by address. Replaces this client's instruction
breakpoint list, leaving source breakpoints and other clients' breakpoints intact.

```jsonc
{"command": "setInstructionBreakpoints", "arguments": {
  "breakpoints": [
    {"instructionReference": "0x80003100", "offset": 0, "condition": "r4 == 0x10"}
  ]
}}
```

## `setDataBreakpoints`

A DAP **data breakpoint** is a memory watchpoint: it pauses the core when the
specified address is read or written. `accessType` is one of
`"read"` / `"write"` / `"readWrite"` (default `"readWrite"`).

```jsonc
// single-byte write watchpoint at 0x80004000
{"command": "setDataBreakpoints", "arguments": {
  "breakpoints": [{"dataId": "0x80004000", "accessType": "write"}]
}}
```

**Ranged watchpoints (Dolphin extension).** Standard DAP watchpoints identify one
address. Dolphin also accepts an optional **`length`** field to watch several
bytes. Omitting `length` or setting it to `1` keeps the standard behavior.

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

## `readMemory` / `writeMemory`

```jsonc
// read 4 bytes at 0x80004000
{"command": "readMemory", "arguments": {"memoryReference": "0x80004000", "count": 4}}
//  → {"address": "0x80004000", "data": "AAAAAA=="}   // base64 for four zero bytes

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

## `disassemble`

```jsonc
{"command": "disassemble", "arguments":
  {"memoryReference": "0x80003100", "instructionCount": 4}}
//  → {"instructions": [
//       {"address": "0x80003100", "instruction": "nop"},
//       {"address": "0x80003104", "instruction": "blr"},
//       ...
//     ]}
```

`instructionCount` is capped at 65536. PC advancement is bounds-checked
against u32 max so a high base address paired with a large count fails safely
rather than wrapping and disassembling unrelated low memory.

## `stackTrace` / `threads` / `scopes` / `variables` / `setVariable`

```jsonc
{"command": "stackTrace", "arguments": {"threadId": 1, "startFrame": 0, "levels": 20}}
{"command": "threads"}
{"command": "scopes", "arguments": {"frameId": 0}}
// → ["Registers", "PC", "Locals", "Globals"] when typed DWARF is loaded
{"command": "variables", "arguments": {"variablesReference": 1000}}  // 1000 = Registers
{"command": "setVariable", "arguments":
  {"variablesReference": 1000, "name": "r3", "value": "0x12345678"}}
```

Frame `source.path`/`source.name` carry either a real source file path (when
DWARF/entrypoints line info is loaded) or a hex anchor address for a
disassembly pseudo-source.

`Locals` and `Globals` are available only for frame 0 and only when MWCC DWARF
1.1 debug information is loaded. `variables` can expand supported typedefs,
pointers, fixed-size arrays, structures, and unions to at most 32 levels and 1000
children. It resolves absolute, supported PPC-register (`r0`-`r31`, `lr`, `ctr`, or `xer`),
base-register-plus-constant, and constant member-offset locations. Scalar displays
generally use raw hexadecimal; supported writable scalars can be edited with
`setVariable`. Location
lists, arbitrary DWARF expressions, bit fields, inheritance, dynamic arrays, and
unwinding locals for older frames are not implemented. Expansion references are
invalidated by resume, stepping, restart, terminate, and value edits through the
GUI or any DAP client, including `writeMemory` (even a partial write reported as
an error) and `evaluate` expressions containing assignments or memory-write
functions. Such expressions conservatively invalidate references even if a
conditional branch skips the mutation; read-only evaluations retain references.
Re-request scope variables after an edit rather than reusing
old expansion references. A new expansion reference returned by `setVariable`
remains valid until a subsequent edit or execution change.

## `evaluate`

```jsonc
{"command": "evaluate", "arguments": {"expression": "r3 + r4"}}
//  → {"result": "0x000004d2", "type": "string"}
```

Uses the PPC debugger expression syntax — the same evaluator that handles
breakpoint conditions.

## `goto` / `gotoTargets`

```jsonc
{"command": "gotoTargets", "arguments": {"source": {"name": "0x80003100"}, "line": 1}}
//  → {"targets": [{"id": 2147496192, "label": "0x80003100",
//                  "instructionPointerReference": "0x80003100"}]}

{"command": "goto", "arguments": {"threadId": 1, "targetId": 2147496192}}
//  → {}  then a stopped/event with reason "goto"
```

Use the `id` returned by `gotoTargets` as `targetId`; do not copy an ID from this
example when jumping elsewhere. The address doubles as the target id so `goto` is
stateless. The core is paused
first so the post-goto stopped event is truthful — if the client called `goto`
while emulation was running, the CPU would otherwise keep executing at the new
PC while the adapter told the client emulation halted.

## `loadedSources` / `source` / `breakpointLocations`

```jsonc
{"command": "loadedSources"}
//  → {"sources": [{"sourceReference": 1, "name": "...", "path": "..."}, ...]}

{"command": "source", "arguments": {"sourceReference": 1, "startLine": 0, "endLine": -1}}
{"command": "breakpointLocations",
 "arguments": {"source": {"name": "0x80003100"}, "line": 1, "endLine": 20}}
```

`source` line emission is capped at 256 lines and `breakpointLocations`
iteration is capped at 65536 entries — bounds guarding against pathological
inputs.

## `exceptionInfo`

```jsonc
{"command": "exceptionInfo", "arguments": {"threadId": 1}}
//  → {"exceptionId": "0x00000000", "description": "...", "breakMode": "always"}
```

## `terminate` / `restart` / `disconnect`

```jsonc
{"command": "terminate"}          // → emits {"event": "terminated", "body": {"restart": false}}
{"command": "restart"}           // → reset, pause, then emit a stopped/"restart" event
{"command": "disconnect"}         // → session ends, sockets torn down
```

`restart` leaves the game paused and emits a `stopped`/`"restart"` event.

# Dolphin-specific custom requests

These requests are not part of the DAP spec and are prefixed `dolphin_`. A
DAP-aware editor that wants to use them needs a small client-side extension
(e.g. a custom VS Code command or a debug-adapter extension).

## `dolphin_realtimeWatch`

Subscribes to changes in a memory region. Unlike `setDataBreakpoints`, which
pauses the game on access, a realtime watch streams changed values without
stopping. Values are checked once per video field, and unchanged regions do not
produce events.

```jsonc
// subscribe
{"command": "dolphin_realtimeWatch",
 "arguments": {"memoryReference": "0xdeadb33f", "count": 256}}
//  → {"watchId": 1, "address": "0xdeadb33f", "count": 256}
```

`count` is capped at 1 MiB; if the original request exceeds the cap, the
response includes `requestedCount` (the original value) alongside the capped
`count` so the client knows it's watching fewer bytes than it asked for.

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

## `dolphin_realtimeWatchCancel`

```jsonc
{"command": "dolphin_realtimeWatchCancel", "arguments": {"watchId": 1}}
//  → {}   on success
//  → error response with message "no such watch" if watchId is unknown
```

After cancellation the region is no longer sampled; no further
`dolphin_memoryChanged` events are emitted for that `watchId`.

## `dolphin_freeze`

Holds a memory region at a fixed value. Every field, if the cell drifted from
the frozen canon, the adapter writes the canon back and suppresses the
`dolphin_memoryChanged` event. Two forms, both base64-encode the value as
`data` (matching the DAP `writeMemory` convention).

**Form 1 — standalone freeze** (creates a new frozen subscription):

```jsonc
{"command": "dolphin_freeze",
 "arguments": {"memoryReference": "0x803ce4e8", "count": 4, "data": "AAAAAQ=="}}
//  → {"watchId": 2, "address": "0x803ce4e8", "count": 4}   on success
//  → error response  if data does not decode to exactly count bytes,
//                     count is 0, or address+count wraps past u32 max
```

**Form 2 — freeze an existing watch in place** (the watch was previously
created via `dolphin_realtimeWatch` or `dolphin_freeze`):

```jsonc
{"command": "dolphin_freeze",
 "arguments": {"watchId": 3, "data": "AAAAAQ=="}}
//  → {"watchId": 3}                                        on success
//  → error response  if watchId is unknown or data length != watch count
```

The frozen value (`data`) must always be exactly `count` bytes long. A freeze
takes effect immediately and blocks normal game writes. Hardware-driven writes
may appear briefly before the value is restored on the next video field. The DAP
client can replace the value with `writeMemory` without unfreezing it first.

`dolphin_memoryChanged` events are suppressed for frozen subscriptions (the
freeze *is* the response).

## `dolphin_unfreeze`

Clears the freeze layer on an existing watch.

```jsonc
{"command": "dolphin_unfreeze", "arguments": {"watchId": 3}}
//  → {}            on success
//  → error response with message "no such watch" if watchId is unknown
```

The watch itself stays subscribed and resumes dispatching `dolphin_memoryChanged`
events normally. Idempotent — calling on a watch that wasn't frozen still
succeeds.

## `dolphin_findFreeMemory`

Scans MEM1 (real RAM size, `GetRamSizeReal`) for the smallest 4-byte-aligned
run of zero words of at least `count` bytes and returns its address. Used by
integrators that want to inject code but don't know the game's memory layout —
the server picks a safe address.

```jsonc
{"command": "dolphin_findFreeMemory", "arguments": {"count": 64}}
//  → {"address": "0x8012d3c0", "count": 64}   on success
//  → error response with message "no free region of that size"
//                    if count is 0 or no run of zeros >= count exists
```

## `dolphin_injectCode`

Writes PPC machine code at an explicit or server-allocated address. The
client supplies raw big-endian bytes (base64-encoded); the server does not
assemble. `memoryReference` is optional — when omitted, the server allocates a
region via `dolphin_findFreeMemory` and writes there; when present, it writes
at that address. `WriteMemory`'s iCache + JIT invalidation ensures the
injected bytes are observed by the next fetch in both interpreter and JIT
modes.

```jsonc
// write at an explicit address
{"command": "dolphin_injectCode", "arguments":
  {"memoryReference": "0x8000c000", "code": "AAAAAAAA"}}
//  → {"address": "0x8000c000", "count": 4}

// let the server pick a code cave
{"command": "dolphin_injectCode", "arguments": {"code": "AAAAAAAA"}}
//  → {"address": "0x8012d3c0", "count": 4}   (allocated address)
//  → error response "no free region of that size" if no cave exists
```

`code` must be a non-empty multiple of 4 bytes (PPC instruction alignment);
otherwise the request is rejected as invalid arguments. The server does not
validate the instructions themselves — PC alignment of trailing data is the
client's responsibility. When no address is supplied, free memory is allocated
ONCE and threaded through to the inject call so the response is truthful (no
second scan that could disagree with the pre-check under a running core).

### Overwriting existing memory

**Yes — explicit-address injections overwrite whatever is at the target
address, including live game code, existing detour bodies, or your own
previously-injected code.** The server performs no read-before-write, no
snapshot, no rollback, and no overlap detection. It calls `WriteMemory`
directly:

- **At a code address:** overwrites the instruction stream. The bytes need
  not form a valid instruction boundary alignment with what was there
  before — the server doesn't decode or check. As long as your `code` is a
  multiple of 4, the write succeeds and iCache+JIT are invalidated so the
  next fetch sees the new bytes.
- **At a data address:** overwrites the data. Useful for patching constants,
  tables, or live game state. No different from `writeMemory` except the
  iCache invalidation hint — harmless for data writes.
- **Overwriting game code that has a detour installed:** the detour's
  patched `b detour_addr` instruction at the target is itself 4 bytes; if
  your injection writes over it, the detour is silently destroyed (no
  rollback). Detour bodies, trampolines, and code injected by prior
  `dolphin_injectCode` calls are all regular memory and can be overwritten
  the same way.
- **Overwriting the same address twice:** the second write wins. There is no
  versioning, no diffing, no record of what was there before. If you need
  rollback semantics (write-through-until-revert), snapshot the bytes
  yourself with `readMemory` before injecting.

When `memoryReference` is **omitted**, the server allocates via
`dolphin_findFreeMemory` which scans for a zero-run — so auto-allocated
regions are guaranteed to be unused (as of the scan). Explicit addresses
carry no such guarantee.

## `dolphin_detour`

Installs a transparent detour at a 4-byte instruction target. The server:

1. Allocates `detourAddress` + trampoline address (if `detourAddress` is
   omitted, finds free memory big enough for both via `dolphin_findFreeMemory`).
2. Writes `detourBody` at `detourAddress`.
3. Appends `b trampolineAddress` to the detour so control resumes at the
   trampoline after the body.
4. Writes the trampoline at `trampolineAddress`: the original instruction
   followed by `b targetAddress + 4`.
5. Patches `targetAddress` with `b detourAddress`.

The patched-out instruction still executes (via the trampoline) so the detour
is transparent. The detour body should end with `b trampolineAddress` (or
fall through to the implicit appended one) to resume after the patch site.
All writes invalidate the iCache + JIT. If the call fails after any write,
previously-written regions are restored in reverse so the caller is returned
to the pre-detour byte layout (no partially-patched target/trampoline left
behind).

```jsonc
{"command": "dolphin_detour", "arguments": {
  "memoryReference": "0x80003100",   // targetAddress — the 4-byte instruction being detoured
  "detourBody": "AAAAAAAAaaaaaaaa",  // base64 PPC machine code (multiple of 4 bytes)
  "detourAddress": "0x8000c000"      // optional; server allocates when omitted
}}
//  → {"targetAddress": "0x80003100",
//      "detourAddress": "0x8000c000",
//      "trampolineAddress": "0x8000c010",
//      "originalInstruction": "<base64 of the 4 bytes at target>"
//     }
//  → error response "detour failed (invalid target or no free memory?)"
//                    if the target address can't be read, no free region of
//                    the required size exists, or the branch displacement
//                    exceeds the PPC `b` range (±32 MiB)
```

`detourBody` must be a non-empty multiple of 4 bytes. The PPC `b` instruction
encodes a 24-bit signed displacement (±32 MiB); a detour layout placing the
patch site farther than that from its target is rejected outright rather than
silently encoding the wrong branch.

### What the detour body can do

The detour body is **raw PPC machine code**. It runs in the same register
context as the patched instruction (r0–r31, cr, xer, lr, ctr, pc) and has full
read/write access to PowerPC address space; the DAP server doesn't inspect
or constrain it. What your body can do depends on what you do at the end:

- **Inspect inputs and continue.** If you patch the first instruction of a
  function, the body runs at function entry: arguments sit in `r3`–`r10`
  per the PPC calling convention, the stack pointer is `r1`, the return
  address is in `lr`. The body can read them, write them, log them to
  memory, or pass them to another function. As long as it ends with
  `b trampolineAddress` (or falls through to the implicit appended branch),
  the trampoline replays the patched-out instruction and branches back to
  `targetAddress + 4` so the original function body runs normally with
  whatever register state the body left behind. Use this to observe inputs
  or to mutate them before the function sees them.
- **Replace the function entirely.** Write your body so its final instruction
  is a branch (e.g. `b your_return_path`) or `blr` to return to the caller,
  instead of `b trampolineAddress`. Control never reaches the appended
  tail branch or the trampoline, so the original instruction is skipped and
  the function's body never executes. Common pattern for "make a function
  do something else altogether": set `r3` (the typical return value register)
  to your desired value as the last thing the body does, then `blr`.
- **Chain to other DAP commands.** A detour body can call any code in
  addressable memory — including previously-injected code from
  [`dolphin_injectCode`](#dolphin_injectcode) or another detour's body — so
  you can compose: inject a helper routine, then install a detour whose
  body calls it. Make sure to follow the PPC ABI if your body calls other
  functions: save `lr`, set up a stack frame on `r1`, preserve non-volatile
  registers (`r13`–`r31`, `cr2`–`cr4`, etc.) if appropriate.

The detour mechanism itself is **call-transparent and minimal**. The server
patches exactly four bytes (`b detour_addr`) at the target; the trampoline
preserves the patched instruction's observable effect; everything else — what
the body reads, writes, or calls — is up to the PPC code you supply. The body
**must not** assume the patched memory layout stays alive across a `restart`
or another `dolphin_detour` call that touches the same regions; the patched
bytes don't survive PPC reset, and the rollback on a failed detour only
restores regions touched by *that* detour.

## `dolphin_memoryRegions`

Returns target metadata and the canonical regions accepted by memory scans.
GameCube exposes MEM1 and ARAM; Wii exposes MEM1 and MEM2 when initialized.

```jsonc
{"command":"dolphin_memoryRegions"}
// -> {"platform":"gamecube", "pointerSize":4,
//     "byteOrder":"big", "regions":[
//       {"id":"mem1", "name":"MEM1", "baseAddress":"0x80000000",
//        "size":25165824, "readable":true, "writable":true, "scannable":true}
//     ]}
```

## `dolphin_memoryScanStart`

Starts an asynchronous typed scan. Supported `dataType` values are `u8`, `u16`,
`u32`, `u64`, `s8`, `s16`, `s32`, `s64`, `f32`, `f64`, `bytes`, `string`, and
`ppcInstruction`.
Initial filters are `exact`, `notEqual`, `between`, `greaterThan`,
`greaterOrEqual`, `lessThan`, `lessOrEqual`, and `unknown`.
Ranges are half-open (`[start,end)`). Omitted or empty `regions` selects MEM1;
explicit ranges must be wholly contained in one of the selected regions.
Adjacent ranges in the same region are treated as one continuous range, and
requests are limited to 1024 ranges and three unique region IDs.

`dataType:"bytes"` performs a fixed-width raw pattern scan. `value` is canonical
base64 containing 1 to 4096 bytes. Initial filters are `exact` and `notEqual`;
refinements also support `changed` and `unchanged`. Exact/not-equal refinements
must provide a base64 value with the original width. With `aligned:true`, the
pattern width is the stride; with `false`, overlapping matches are possible.
Byte results return the current pattern as base64 in both `scannedValue` and
`raw`. Aligned candidates are anchored to absolute addresses where
`address % width == 0`. A changed byte can affect multiple overlapping windows.

`dataType:"string"` uses the same fixed-width engine with a plain JSON string
`value`. `encoding` is `utf8` (default) or `ascii`; ASCII rejects non-ASCII
input and UTF-8 rejects malformed input. Encoded width is 1 to 4096 bytes.
`caseSensitive` defaults to `true`. Case-insensitive matching folds only
ASCII `A`-`Z`; non-ASCII UTF-8 bytes remain exact. Exact/not-equal refinements
inherit encoding and case sensitivity and must keep the original encoded width.
Changed/unchanged compare raw bytes. `scannedValue` is valid UTF-8 with malformed
result bytes replaced by U+FFFD; `raw` always preserves exact bytes as base64.

`dataType:"ppcInstruction"` scans absolute 4-byte-aligned instruction words.
Initial filters are `exact` (numeric instruction word), `mnemonic` (the exact,
case-sensitive first token of canonical disassembly, including aliases such as
`nop` and `blr`), and `validInstruction` (an encoding supported by Dolphin's
Gekko execution tables and accepted by the canonical disassembler). Refinements
also support raw-word `changed` and `unchanged`. Results include the word as
`scannedValue`, exact bytes in `raw`, and canonical address-aware Gekko
`disassembly`. The type always enforces 4-byte alignment.
Mnemonic and validity scans are limited to 4,194,304 instruction candidates.

```jsonc
{"command":"dolphin_memoryScanStart", "arguments":{
  "regions":["mem1"],
  "ranges":[{"start":"0x80000000", "end":"0x81800000"}],
  "dataType":"u32", "filter":"exact", "value":"100",
  "aligned":true, "pauseDuringScan":false
}}
// -> {"scanId":1, "jobId":1, "state":"running", "pauseDuringScan":false}
```

Every scan pauses CPU, DSP, and FIFO while all requested ranges are copied into
one consistent snapshot. With `pauseDuringScan:false` (the default), emulation
resumes before the immutable snapshot is filtered. With `true`, execution stays
paused through filtering and atomic result commit. A core that was already
paused remains paused.
While a `pauseDuringScan:true` job owns the core, requests other than memory
region, scan status/results/cancel/dispose/undo/result-removal, and disconnect
are rejected. Undo and result removal remain safe to receive but reject while
the job is active.

The accepted response is followed by exactly one terminal event. Clients do
not need to poll status:

```jsonc
{"event":"dolphin_memoryScanCompleted", "body":{
  "scanId":1, "jobId":1, "generation":1, "resultCount":42,
  "durationMilliseconds":183, "pauseDuringScan":false
}}
```

Failures emit `dolphin_memoryScanFailed` with `message`; cancellation emits
`dolphin_memoryScanCancelled`. Failed or cancelled refinements leave the last
committed generation available.

## `dolphin_memoryScanRefine`

Filters a completed generation using a fresh consistent snapshot. In addition
to value filters, refinement supports `changed`, `unchanged`, `increased`,
`decreased`, `increasedBy`, and `decreasedBy`.

```jsonc
{"command":"dolphin_memoryScanRefine", "arguments":{
  "scanId":1, "filter":"decreased"
}}
```

Omitting `pauseDuringScan` inherits the value from `dolphin_memoryScanStart`;
an explicit boolean overrides it for that refinement job.

## `dolphin_memoryScanStatus`

Optional diagnostics/recovery request. Normal clients should wait for terminal
events instead.

```jsonc
{"command":"dolphin_memoryScanStatus", "arguments":{"scanId":1}}
// -> {"scanId":1, "jobId":2, "generation":1, "state":"running",
//     "phase":"filtering", "pauseDuringScan":true,
//     "emulationPaused":true, "resultCount":42}
```

Phases are `waiting`, `capturing`, `filtering`, `committing`, `completed`,
`cancelled`, or `failed`.

## `dolphin_memoryScanResults`

Returns up to 4096 committed results. Results remain readable while a refinement
builds the next generation privately.

```jsonc
{"command":"dolphin_memoryScanResults", "arguments":{
  "scanId":1, "start":0, "count":256
}}
// -> {"scanId":1, "generation":1, "totalResults":42, "start":0,
//     "results":[{"address":"0x80401234", "scannedValue":"100",
//                  "raw":"AAAAZA=="}]}
```

## `dolphin_memoryScanCancel`

```jsonc
{"command":"dolphin_memoryScanCancel", "arguments":{"scanId":1}}
```

Cancellation is cooperative. The original job still emits its mandatory
`dolphin_memoryScanCancelled` terminal event once it has released any scan-owned
pause.

## `dolphin_memoryScanDispose`

```jsonc
{"command":"dolphin_memoryScanDispose", "arguments":{"scanId":1}}
```

Releases retained snapshots and results. Disposing an active scan requests
cancellation; its worker still emits a terminal event. Completion can win if
the generation has already entered its atomic commit.

Limits: one active scan job per DAP session, eight retained scans, 256 MiB of
snapshot plus candidate state process-wide, 256 MiB of snapshot input per job,
non-overlapping ranges, at most 1 GiB of estimated byte-pattern comparison
work, 128-byte numeric literals, and 4096 results per page. Wide byte-pattern
result pages are reduced to at most 1 MiB of raw result bytes, so fewer than the
requested count may return.

## `dolphin_memoryScanUndo`

Restores the previous committed result generation. Successful refinements and
result removals retain up to 16 undo generations. Undo is synchronous, emits no
terminal event, and is rejected while any scan job in the session is active.

```jsonc
{"command":"dolphin_memoryScanUndo", "arguments":{"scanId":1}}
// -> {"scanId":1, "generation":1, "resultCount":42,
//     "removedCount":0, "canUndo":false}
```

Generation numbers identify immutable states and are not reused. Undo restores
the original generation number; the next refinement or removal receives a new,
larger number. Refinement generations retain their snapshots and candidate
bitmaps; removal generations share snapshots but retain their own bitmaps.
Budget admission counts the complete live retained set plus the in-flight
generation, before any 16-generation history eviction.

## `dolphin_memoryScanRemoveResults`

Removes up to 4096 explicit result addresses. A successful removal creates a
new immutable generation without copying the retained snapshot. Duplicate,
unknown, unaligned, and already-removed addresses are ignored. If no supplied
address is an active result, no generation is created.

```jsonc
{"command":"dolphin_memoryScanRemoveResults", "arguments":{
  "scanId":1, "addresses":["0x80401234","0x80405678"]
}}
// -> {"scanId":1, "generation":3, "resultCount":40,
//     "removedCount":2, "canUndo":true}
```

## `dolphin_resolvePointerChain`

Resolves up to 64 big-endian 32-bit pointer dereferences from one consistent
snapshot. `offsets` must contain one to 64 JSON integers in
`[-2147483648, 2147483647]`. For each offset, Dolphin reads the pointer at the
current address and adds the offset to find the next address.

```jsonc
{"command":"dolphin_resolvePointerChain", "arguments":{
  "baseAddress":"0x80004000", "offsets":[16,-4]
}}
// -> {"finalAddress":"0x8000601c", "steps":[
//   {"address":"0x80004000", "pointerValue":"0x80005000",
//    "offset":16, "resultAddress":"0x80005010"},
//   {"address":"0x80005010", "pointerValue":"0x80006020",
//    "offset":-4, "resultAddress":"0x8000601c"}
// ]}
```

The request fails if any four-byte pointer is unreadable, an offset leaves the
32-bit address space, the offset list is empty, or it exceeds 64 entries. The
final address is range-checked but not dereferenced or otherwise required to be
readable.

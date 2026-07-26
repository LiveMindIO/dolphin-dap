# DAP Server Capabilities

Operations supported by the Dolphin DAP server. Each command below is a link
target — the [Features](README.md#features) table links into this document.

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
   "supportsRestartRequest": true,
   "supportsDolphinRealtimeWatch": true,
   "supportsDolphinFreeze": true,
   "supportsDolphinFindFreeMemory": true,
   "supportsDolphinInjectCode": true,
   "supportsDolphinDetour": true
  }}}

// ← then an "initialized" event (means "ready for setBreakpoints / launch")
{"seq": 3, "type": "event", "event": "initialized", "body": {}}
```

## `launch` / `attach`

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

{"command": "next",      "arguments": {"threadId": 1}}   // step over
{"command": "stepIn",    "arguments": {"threadId": 1}}
{"command": "stepOut",   "arguments": {"threadId": 1}}
//  → {"event": "stopped", "body": {"reason": "step", "threadId": 1}}
```

On a spontaneous stop (breakpoint hit / watchpoint hit / step completion) the
reason is classified: `"breakpoint"`, `"data breakpoint"`, or `"step"`.
`hitBreakpointIds` is populated for code breakpoints.

## `setBreakpoints`

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

## `setInstructionBreakpoints`

Directly sets code breakpoints by address. Replaces the whole code-breakpoint
list (mirrors the GDB stub).

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

## `readMemory` / `writeMemory`

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
{"command": "scopes", "arguments": {"frameId": 0}}     // → ["Registers", "PC"]
{"command": "variables", "arguments": {"variablesReference": 1000}}  // 1000 = Registers
{"command": "setVariable", "arguments":
  {"variablesReference": 1000, "name": "r3", "value": "0x12345678"}}
```

Frame `source.path`/`source.name` carry either a real source file path (when
DWARF/entrypoints line info is loaded) or a hex anchor address for a
disassembly pseudo-source.

## `evaluate`

```jsonc
{"command": "evaluate", "arguments": {"expression": "r3 + r4"}}
//  → {"result": "0x000004d2", "type": "string"}
```

Uses the PPC debugger expression syntax — the same evaluator that handles
breakpoint conditions.

## `goto` / `gotoTargets`

```jsonc
{"command": "gotoTargets", "arguments": {"source": {"name": "0x80003100"}, "line": 0}}
//  → {"targets": [{"id": 2147501824, "label": "0x80003100",
//                  "instructionPointerReference": "0x80003100"}]}

{"command": "goto", "arguments": {"threadId": 1, "targetId": 2147501824}}
//  → {}  then a stopped/event with reason "goto"
```

The address doubles as the target id so `goto` is stateless. The core is paused
first so the post-goto stopped event is truthful — if the client called `goto`
while emulation was running, the CPU would otherwise keep executing at the new
PC while the adapter told the client emulation halted.

## `loadedSources` / `source` / `breakpointLocations`

```jsonc
{"command": "loadedSources"}
//  → {"sources": [{"sourceReference": 1, "name": "...", "path": "..."}, ...]}

{"command": "source", "arguments": {"sourceReference": 1, "startLine": 0, "endLine": -1}}
{"command": "breakpointLocations",
 "arguments": {"source": {"name": "0x80003100"}, "line": 0, "endLine": 20}}
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
{"command": "restart"}           // → core reset + Break + State::Paused, then a stopped/"restart" event
{"command": "disconnect"}         // → session ends, sockets torn down
```

`restart` forces `CPU::Break()` + `Core::State::Paused` after the PPC reset so
the post-restart `stopped`/`"restart"` event is truthful even when the client
had continued execution before the restart.

# Dolphin-specific custom requests

These requests are not part of the DAP spec and are prefixed `dolphin_`. A
DAP-aware editor that wants to use them needs a small client-side extension
(e.g. a custom VS Code command or a debug-adapter extension).

## `dolphin_realtimeWatch`

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

The frozen canon (`data`) must always be exactly `count` bytes long. The
adapter enforces the freeze with two layers:

1. **MMU write suppression** — an `is_freeze` memcheck is installed on the
   watched range. Emulated CPU stores (`MMU::Write<T>`) that hit the range
   are silently dropped before reaching RAM, so the game's own writes are
   perfectly unobservable (no ~16 ms window).
2. **Field-rate Tick fallback** — at each video field, `Tick()` re-applies
   the canon if the region drifted. With layer 1 active, drift only comes
   from DMA/peripheral writes that bypass `MMU::Write` (PI/DVD transfers,
   `Memory::CopyToEmu`, etc.). The ~16 ms window is DMA-only, not CPU.

HostWrite (debugger/cheat writes, including DAP `WriteMemory`) bypasses the
memcheck by design, so the DAP client can update the frozen value itself
without first unfreezing. The canon is written into guest RAM immediately at
subscribe/Freeze time (under `CPUThreadGuard`) via `HostWrite` so the freeze
takes effect at once even when the core is paused — the watched bytes are
not left at the game value waiting for the next field.

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

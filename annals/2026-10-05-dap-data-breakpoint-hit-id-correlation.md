# DAP data breakpoint hit ID correlation by exact watchpoint range

Date: 2026-10-05
Status: implemented
Scope: DAP `setDataBreakpoints` ID assignment, `ExecutionState` watchpoint event fields, stopped-event `hitBreakpointIds`.

## Git provenance

- Repository: Dolphin emulator debugger branch, `feature/gui-source-debugger`
- Canonical remote: `origin` (`https://github.com/Jacoby6000/dolphin.git`, redirects to `https://github.com/LiveMindIO/dolphin-dap.git`)
- Branch state: clean, tracking `origin/feature/gui-source-debugger`
- Recording HEAD: `129ff7b29092e44a7ba3dba355b889ba4b565d00` (`129ff7b290`), nearest describe `5.0-25542-g129ff7b290`
- Decision commit: `ec07612613de4a7bb31599595b4441b1437f2bfb` (`ec07612613`), subject `DAP: correlate data breakpoint hits by exact watchpoint range`
- Source session: `ses_f07c8f576ffergQyBAxj2eibaJ`, response `msg_10d2f60e6001HvvlgYg0qnH4KP`

## Decision

DAP data breakpoint hit IDs must be derived from the exact watchpoint start/end present on the `ExecutionEvent`, not from historical containment of a single address. `LookupDataBreakpointId(start, end)` matches the event's current watchpoint range.

## Context and problem

`DapSession::LookupDataBreakpointId` previously scanned historical `(range -> id)` entries by containment. Replacing `[0x4000,0x400f]` id1 with `[0x4004,0x4007]` id2, or expanding the same start `[0x4000,0x4003]` id1 to `[0x4000,0x400f]` id2, made a real guest store emit id1 rather than id2.

## Options considered

- Keep address-containment lookup and order newest first. Rejected; overlapping shifted/resized ranges remain ambiguous.
- Re-key IDs by exact range. Chosen because `ExecutionState` already carries exact `watchpoint_start`/`watchpoint_end` and `setDataBreakpoints` assigns IDs by exact range.
- Attach DAP ID to `TMemCheck`. Deferred; more invasive and not required for correlation.

## Rationale

Historical range lookup cannot distinguish replaced overlapping watchpoints. Exact range matching uses the event's authoritative cause and preserves stable IDs for actual active ranges.

## Consequences

- `setDataBreakpoints` still assigns/reuses IDs by `{start,end}`.
- `DapSession` stopped events with watchpoint start/end resolve `hitBreakpointIds` to the exact current range.
- Two socket-level regressions cover shifted and resized ranges red-before-green after fix.
- Full suite passed with no exclusions after both reporting fixes.

## Implementation evidence

- Commit `ec07612613` changed `DapSession.cpp` and `DapSessionTest.cpp`.
- Focused tests passed: shifted/resized data breakpoint ID correlation, `RangedDataStopCorrelatesInteriorAccessWithBreakpointId`, `MemoryBreakpointRegistryTest.*`.

## Follow-up obligations

- Ensure any future watchpoint event includes both `watchpoint_start` and `watchpoint_end`; without both, exact ID correlation is unavailable.
- Do not fall back to historical containment for DAP hit ID reporting.

## Reconsideration triggers

- If multiple active watchpoints can exactly cover the same access, define a deterministic precedence or report multiple IDs.
- If event data no longer carries exact range, restore exact-range provenance before relying on IDs.

## Open questions

- None durable.

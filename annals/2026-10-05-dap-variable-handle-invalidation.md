# DAP variable expansion handle invalidation after value edits

Date: 2026-10-05
Status: implemented
Scope: DAP `setVariable` handling and variable expansion handle lifetime.

## Git provenance

- Repository: Dolphin emulator debugger branch, `feature/gui-source-debugger`
- Canonical remote: `origin` (`https://github.com/Jacoby6000/dolphin.git`, redirects to `https://github.com/LiveMindIO/dolphin-dap.git`)
- Branch state: clean, tracking `origin/feature/gui-source-debugger`
- Recording HEAD: `129ff7b29092e44a7ba3dba355b889ba4b565d00` (`129ff7b290`), nearest describe `5.0-25542-g129ff7b290`
- Decision commit: `b23715c6ec1b9653cc2c920d90baddcc25d582ef` (`b23715c6ec`), subject `DAP: invalidate variable expansion handles after value edits`
- Source session: `ses_f07c8f576ffergQyBAxj2eibaJ`, response `msg_10d2f60e6001HvvlgYg0qnH4KP`

## Decision

DAP value edits must invalidate cached/expansion variable handles before allocating a fresh `setVariable` response handle. `ValuesChanged`/`ApplyDebugValueInvalidation` must run on the session thread before request handling that can materialize handles and before response handle allocation.

## Context and problem

A read-only review reproduced that editing a register-held pointer from `0x4000` to `0x5000` left an old variables reference reading and writing the stale `0x4000` address. The existing `ValuesChanged` invalidation was scheduled too late for the in-flight request path.

## Options considered

- Retire expansion handles synchronously before handling the editing request or allocating its response handle. Chosen.
- Keep old handles but validate reads/writes against current guest memory. Rejected as a weaker data model; stale handles should not remain usable.
- Re-resolve only the edited variable. Rejected; child expansions derived from a parent/register chain can all become stale.

## Rationale

Expansion handles are derived from mutable execution state. A write can change the address, type context, or child visibility of every derived handle. Clearing before response allocation prevents immediate reuse of invalidated handles.

## Consequences

- `DapSession` applies invalidation before normal command handling and again before `setVariable` success responses.
- Pointer edits reject stale pointer reads/writes, fresh response handles survive invalidation, and external value-change invalidation is covered by socket-level tests.
- Existing typed-write test requires reacquiring variables after an edit.
- `Tools/dap/capabilities.md` documents expansion-reference lifetime.

## Implementation evidence

- Commit `b23715c6ec` changed `DapSession.cpp`, `DapSession.h`, `DapSessionTest.cpp`, and `Tools/dap/capabilities.md`.
- Focused tests passed: `RegisterPointerEditsInvalidateOldPointeeHandles`, `PointerEditResponseHandleSurvivesInvalidation`, `SetVariableUpdatesTypedChildAndReturnsType`.
- Later full-suite runs passed with no exclusions.

## Follow-up obligations

- Preserve reacquisition semantics in client tests and documentation.
- Do not allocate expansion handles before pending invalidation is applied.

## Reconsideration triggers

- If variable handles become generation-scoped rather than cleared, retain the same guarantee that stale derived handles cannot write through old addresses.

## Open questions

- None durable.

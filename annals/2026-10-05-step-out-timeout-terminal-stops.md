# Step-out timeout publishes owned terminal stops

Date: 2026-10-05
Status: implemented
Scope: DAP and DolphinQt step-out timeout execution-state reporting.

## Git provenance

- Repository: Dolphin emulator debugger branch, `feature/gui-source-debugger`
- Canonical remote: `origin` (`https://github.com/Jacoby6000/dolphin.git`, redirects to `https://github.com/LiveMindIO/dolphin-dap.git`)
- Branch state: clean, tracking `origin/feature/gui-source-debugger`
- Recording HEAD: `129ff7b29092e44a7ba3dba355b889ba4b565d00` (`129ff7b290`), nearest describe `5.0-25542-g129ff7b290`
- Decision commits: `5f94cb4734a940a0e2e47a23c5543f22ce425f65` (`5f94cb4734`), `7b088a7cd2075fc04a527985a0d9162b44f5aade` (`7b088a7cd2`)
- Source session: `ses_f07c8f576ffergQyBAxj2eibaJ`, response `msg_10d2f60e6001HvvlgYg0qnH4KP`

## Decision

A step-out timeout whose CPU remains stepping must publish an owned terminal stop rather than abandon the operation and leave shared execution state running. Cancellation and active-operation checks remain authoritative; instruction-acknowledgement timeout behavior remains unchanged.

## Context and problem

The DAP path had been fixed first by publishing `CompleteStep` when CPU remained stepping. A later real Qt reproducer showed `CodeWidget` still called `AbandonStep` after a `NotStepped` step-out timeout. The CPU was paused, but `ExecutionState` remained running/stopped=false, so connected DAP clients and variables views became inconsistent.

## Options considered

- Treat step-out timeout like cancellation with `AbandonStep`. Rejected; timeout is terminal for the stepping operation, not user cancellation.
- Publish an unclassified running state again. Rejected; CPU is paused and no further opcode is pending.
- On `NotStepped` + `Out` + CPU still stepping, enter the existing owned terminal-stop path after cancellation and ownership checks. Chosen.

## Rationale

Unlike an instruction acknowledgement timeout, step-out timeout does not leave a pending opcode that could complete after the stop. The shared execution service must reflect that the debugger is stopped and the operation is complete.

## Consequences

- DAP `5f94cb4734` adds deterministic `SessionTestHooks::step_out_timeout` and publishes terminal stops.
- Qt `7b088a7cd2` treats step-out timeout as a terminal owned stop and keeps instruction timeout handling unchanged.
- Real offscreen Qt reproducer went from `CPU stepping=1 execution stopped=0 continued=1 stopped=0` to `execution stopped=1 stopped events=1`.
- Focused `ExecutionStateTest`, `PPCSteppingTest`, and DAP timeout tests passed; later full suite passed 1,879 and then 1,894 tests with no exclusions.

## Implementation evidence

- DAP test hooks and socket-level timeout tests exist in `DapSessionTest.cpp`.
- Qt timeout reproducer linked against real `CodeWidget`/`PPCStepping` engine with zero-duration timeout passed after fix.

## Follow-up obligations

- Keep cancellation and external-stop ownership checks before publishing terminal stops.
- Do not use the same terminal-stop fallback for instruction acknowledgement timeouts unless the engine can prove no pending opcode remains.

## Reconsideration triggers

- If step-out can later resume a pending opcode after timeout, the terminal-stop path must be revisited.

## Open questions

- None durable.

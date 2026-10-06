# DAP bootless test fixture CPU-thread role for async step workers

Date: 2026-10-05
Status: implemented
Scope: DAP session unit-test fixture validity and previously excluded asynchronous step tests.

## Git provenance

- Repository: Dolphin emulator debugger branch, `feature/gui-source-debugger`
- Canonical remote: `origin` (`https://github.com/Jacoby6000/dolphin.git`, redirects to `https://github.com/LiveMindIO/dolphin-dap.git`)
- Branch state: clean, tracking `origin/feature/gui-source-debugger`
- Recording HEAD: `129ff7b29092e44a7ba3dba355b889ba4b565d00` (`129ff7b290`), nearest describe `5.0-25542-g129ff7b290`
- Decision commit: `bf4ac46572e962514c13d569cae0f9372cf618ba` (`bf4ac46572`), subject `DAP tests: establish CPU-thread role for bootless step workers`
- Source session: `ses_f07c8f576ffergQyBAxj2eibaJ`, response `msg_10d2f60e6001HvvlgYg0qnH4KP`

## Decision

Bootless DAP fixtures must establish the CPU-thread role on every asynchronous stepping worker, not only on the session thread. Optional `SessionTestHooks::async_step_worker_started` lets the bootless fixture call `Core::DeclareAsCPUThread()` at worker start while production null-hook behavior stays unchanged.

## Context and problem

Four DAP tests had been excluded because they asserted `Core::IsCPUThread()` at `CPUThreadConfigCallback.cpp:70`; one had passed an explicit recheck but remained excluded. Rerunning all four confirmed all failed with the same assertion. The bootless fixture declared the session thread as CPU thread, but async source/step-out workers executed interpreter/CoreTiming while global Core was uninitialized, so `CPUThreadGuard` did not confer the role.

Affected tests:

- `DapSessionTest.StepCommandsRespondAndEmitStopped`
- `DapSessionTest.DisconnectCancelsAndReleasesActiveStep`
- `DapSessionTest.PauseCancelsAndJoinsActiveStepBeforePublishingStop`
- `DapAsyncStepOrderingTest.CompletedStepStopPrecedesAlreadyReadableRequestResponse`

## Options considered

- Keep excluding the four tests. Rejected; validation gap remains.
- Weaken or remove `Core::IsCPUThread()` assertions. Rejected; production code relies on the role.
- Add worker-start fixture hook and declare role per fresh worker. Chosen; mirrors session thread setup and ends with thread lifetime.

## Rationale

The test fixture boots a real memory/CPU subsystem without the global Core state machine. CPU-thread role is thread-local and must be established wherever interpreter/CoreTiming runs. The production default hook keeps behavior unchanged.

## Consequences

- All four formerly excluded tests passed 20 consecutive runs.
- Full suite passed 1,883 tests with zero exclusions at `bf4ac46572`; final later state reached 1,894 with zero exclusions.
- The old exclusion list is obsolete.

## Implementation evidence

- `SessionTestHooks` gained `async_step_worker_started`.
- Both async worker lambdas invoke the hook before stepping.
- `DapSessionTest` sets the hook to `Core::DeclareAsCPUThread()` and comments why `CPUThreadGuard` is insufficient in bootless mode.
- Builds passed for `tests`, `dolphin-emu`, `dolphin-emu-nogui`.

## Follow-up obligations

- Do not count on `CPUThreadGuard` to establish CPU-thread role in uninitialized/bootless tests.
- Keep assertions intact; fix fixture threading rather than weakening production checks.

## Reconsideration triggers

- If the global Core state machine is booted in DAP fixtures, this hook may become redundant; revalidate before removing.

## Open questions

- None durable.

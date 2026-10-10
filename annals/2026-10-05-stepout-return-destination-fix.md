# Step-out return-destination breakpoint evaluation (implemented)

Date: 2026-10-05
Status: implemented
Scope: Shared `PPCStepping` step-out return transition and destination breakpoint predicates (DAP and Qt).

## Git provenance

- Repository: Dolphin emulator debugger branch, `feature/gui-source-debugger`
- Canonical remote: `origin` (`https://github.com/Jacoby6000/dolphin.git`, redirects to `https://github.com/LiveMindIO/dolphin-dap.git`)
- Branch state: clean, tracking `origin/feature/gui-source-debugger`; untracked `annals/` preserved untouched
- Recording HEAD: `8297b5057d14ba208a74dd152e971e2efdbed613` (`8297b5057d`)
- Prior HEAD: `88a5ddc3b99aa2bce2df97a12452f2486197032d`
- Fix commit: `8297b5057d` ("Debugger: check step-out return destination breakpoints"), pushed to `feature/gui-source-debugger`; PR #5 head verified at `8297b5057d14ba208a74dd152e971e2efdbed613`.
- Source session: `ses_f07c8f576ffergQyBAxj2eibaJ`, fix request response; follow-on implementation completed in same session (final answer `msg_10d6e3384001NgTQLB6pa1t060`).

## Decision summary

Implement the previously proposed fix from `2026-10-05-stepout-return-destination-breakpoint-gap.md`: after the final acknowledged return step in `PPCStepping::StepOut`, evaluate the destination breakpoint once via `CheckAndHandleBreakPoints()`, guarded by cancellation, by an intervening memory-check hit (`DidSteppingMemcheckHit`), and by an unchanged pre-return stop generation. `DapDebugController::CompleteStep` keeps its existing normal-`Step` completion for non-breaking destinations and does not regain existence-only classification.

## Context and problem

The finding recorded at `88a5ddc3b9` still applied: `StepOut` executed the return with `step_one()` and returned without visiting the destination through `can_continue()`'s `CheckBreakPoints()` flow, so a step-out landing on an enabled breakpoint reported `Step` and never evaluated that breakpoint's condition. Two temporary regressions had reproduced this at `88a5ddc3b9` (controller-level and DAP-socket level), while all 1,901 existing tests passed.

## Requirements, constraints, and evidence

- The destination predicate must run exactly once; side-effecting conditions such as `r3 = r3 + 1` must not be double-evaluated (the instruction-step fix at `129ff7b290` already enforces this for instruction-level steps).
- The check must not overwrite an intervening stop published during the return (external pause/data breakpoint) nor run after cancellation; mirror the instruction-step safeguards from `129ff7b290`.
- Non-breaking destinations (disabled, false condition, log-only, globally disabled breaking) must still complete as a normal `Step` stop owned by the step operation.
- Full suite must pass without reintroducing the four historical DAP exclusions.

## Options considered

- Evaluate the final destination predicate once in shared `StepOut` with cancellation/stop-generation/memcheck safeguards. Chosen; adopted from the prior proposal.
- Re-evaluate every inner-loop iteration. Rejected; `can_continue()` already runs `CheckBreakPoints()` per iteration, so only the final return edge needs the extra check.
- Restore existence-only destination classification in `CompleteStep`. Rejected; refuted by the `6ba0dadb23`/`129ff7b290` experience that existence without the real predicate drops conditions and double-publishes.

## Chosen option and rationale

Chosen the single guarded evaluation after final return, implemented in `Source/Core/Core/Debugger/PPCStepping.cpp` (lines ~230–244): capture `stop_generation` before `step_one()`, then after stepping, if not cancelled, no stepping memcheck hit occurred, and the stop generation is unchanged, call `power_pc.CheckAndHandleBreakPoints()` once. This keeps ownership in the shared stepping engine, preserves `CompleteStep` as a pure step-completion classifier, and matches the instruction-step contract established at `129ff7b290`.

## Consequences, tradeoffs, and risks

- Step-out onto an enabled breakpoint now publishes the real breakpoint stop cause with `hitBreakpointIds` instead of a normal step.
- A breakpoint condition with guest-state side effects runs once more for the return destination; evaluation is skipped when cancelled, on memcheck hit, or when an intervening stop superseded the return.
- Source-row and instruction step-over (`StepOver`) temporary-breakpoint return paths remain governed by their existing temporary-breakpoint tag; the open question from the prior record about extending the same check to those paths stays open.
- Follow-up obligations from the prior record are fulfilled: shared fix, permanent `DapControllerTest`/`DapSessionTest`/`PPCSteppingTest` regressions, full suite green.

## Implementation status and evidence

- `Source/Core/Core/Debugger/PPCStepping.cpp`: `StepOut` final-return path now evaluates destination breakpoints once with the guards described above.
- Tests added: `DapControllerTest.StepOutEvaluatesReturnDestinationConditionOnlyOnce` (side-effect condition runs exactly once, breakpoint cause and operation ownership preserved); parameterized `DapStepOutCompletionTest.NonBreakingReturnDestinationRetainsStepCause` for disabled/false-condition/log-only/globally-disabled destinations; `DapSessionTest.StepOutReturnReportsDestinationBreakpointId` (DAP socket reports `reason: "breakpoint"` and the verified breakpoint's `hitBreakpointIds`); parameterized `PPCStepOutInterruptionTest.ReturnDoesNotEvaluateDestinationAfterInterruption` for cancellation and an intervening external stop.
- Eight new tests passed 20 repetitions; full suite: 1,909 tests passed with no exclusions (`/tmp/opencode/pr5-stepout-destination-suite.log`); `tests`, `dolphin-emu`, and `dolphin-emu-nogui` targets built successfully (`/tmp/opencode/pr5-stepout-destination-build.log`).
- Pre-existing untracked `annals/` preserved; no tracked changes beyond the fix.

## Reconsideration triggers

- If step-out is restructured so all PC transitions share one post-step hook, fold the final evaluation into that hook rather than keeping a separate call site.
- If source-row/step-over return paths are found to skip destination predicates similarly, extend the same guarded check there.

## Open questions and missing historical information

- Whether the final-destination check should also cover `StepSourceRow` step-over-to-temporary-breakpoint and `StepInstructionOver` return paths; current evidence covers only `StepOut`.

## Subsequent developments (2026-10-05)

A later review at `8297b5057d` narrowed the remaining open question rather than refuting the step-out fix: temporary tests showed `StepSourceRow::step_one` evaluating destination predicates without interruption guards, and temporary step-over return hits retaining `CodeBreakpoint` classification. Extending the same guard/result distinction to those paths remains open; see `2026-10-05-source-step-interruption-and-temporary-step-over-classification.md`.

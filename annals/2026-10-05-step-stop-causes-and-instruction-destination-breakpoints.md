# Step stop causes and instruction-step destination breakpoint evaluation

Date: 2026-10-05
Status: implemented
Scope: DAP/DolphinQt step completion classification and destination breakpoint handling in shared `PPCStepping`.

## Git provenance

- Repository: Dolphin emulator debugger branch, `feature/gui-source-debugger`
- Canonical remote: `origin` (`https://github.com/Jacoby6000/dolphin.git`, redirects to `https://github.com/LiveMindIO/dolphin-dap.git`)
- Branch state: clean, tracking `origin/feature/gui-source-debugger`
- Recording HEAD: `129ff7b29092e44a7ba3dba355b889ba4b565d00` (`129ff7b290`), nearest describe `5.0-25542-g129ff7b290`
- Intermediate commits: `6ba0dadb23010e3ecf0e21e0bda84e2205662c37` (`6ba0dadb23`), then corrected by `129ff7b29092e44a7ba3dba355b889ba4b565d00` (`129ff7b290`)
- Source session: `ses_f07c8f576ffergQyBAxj2eibaJ`, response `msg_10d2f60e6001HvvlgYg0qnH4KP`

## Decision

Normal step completion must publish `ExecutionStopCause::Step` unless the shared stepping engine has already published a real breakpoint stop and retired the operation. Destination breakpoint existence alone must not classify a step as a breakpoint hit. Conversely, instruction-level steps onto an enabled, breaking destination breakpoint must evaluate that breakpoint's actual condition/break policy after the acknowledged instruction step and publish the real hit.

## Context and problem

`6ba0dadb23` removed existence-only `IsAddressBreakPoint` classification from `DapDebugController::CompleteStep` and the Qt worker. That fixed false breakpoint stops for disabled, condition-false, log-only, and globally disabled destinations. It also refuted a temporary assumption that every destination breakpoint stop is engine-published.

A follow-up Bugbot/test reproduced the inverse defect: instruction-level `StepInto`/`next` uses `Interpreter::SingleStep()`, which does not check the destination breakpoint. The latest `CompleteStep` then reported normal `step` and omitted `hitBreakpointIds` even for an enabled breaking breakpoint.

## Options considered

- Restore existence-only classification in `CompleteStep`/Qt worker. Rejected; it reintroduces false breakpoint hits for disabled/false-condition/log-only destinations.
- Only trust engine stops and never evaluate destination after instruction steps. Rejected; instruction stepping would lose real destination hits.
- Evaluate destination breakpoint predicates once in shared `PPCStepping` after acknowledged instruction steps, on both direct CPU-thread and `StepOpcode` paths, skipping when cancelled or when stop generation changed. Chosen.
- Keep `CompleteStep` as normal Step completion. Chosen for the operation-active fallback; real engine hits have already retired the operation and preserve `CodeBreakpoint`/IDs.

## Rationale

Breakpoint semantics belong in the execution engine, not in existence-only completion classification. The engine must publish real hits with exact cause and ID correlation; completion code should not fabricate hit causes and should not suppress real ones.

## Consequences

- `PPCStepping.cpp` records stop generation before acknowledged instruction stepping and calls `CheckAndHandleBreakPoints()` once after the step when not cancelled and generation unchanged.
- DAP and Qt instruction `next`/`stepIn` report real destination breakpoint hits; disabled, false-condition, log-only, and globally disabled destinations retain normal `Step`.
- `DapControllerTest` has parameterized source/instruction non-breaking destination tests and a single-evaluation condition test.
- `PPCSteppingTest` has real `CPUManager::Run` acknowledgement and timing-injected external-stop preservation tests.
- Full suite passed 1,894 tests with no exclusions; selected regression tests passed 20 repeats; five offscreen Qt `Step()` checks passed.

## Refuted claim

Intermediate commit `6ba0dadb23` claimed preserving ordinary step completion by removing existence-only checks was sufficient. It was superseded by evidence that instruction stepping needs actual destination-breakpoint predicate evaluation; the final design keeps the non-breaking false-hit fix and adds engine-side destination checks.

## Follow-up obligations

- Do not restore `IsAddressBreakPoint` in completion paths.
- Keep condition evaluation single-shot for a destination; a side-effecting condition must not be evaluated both by engine and completion.
- If the interpreter later checks destination breakpoints itself, remove the shared post-step evaluation to avoid duplicate condition evaluation.

## Reconsideration triggers

- If `Interpreter::SingleStep()` starts performing full breakpoint checks, re-evaluate this post-step hook.
- If stop generation semantics change, update the skip guard.

## Open questions

- Source-row stepping and temporary step-over classification remain open at `8297b5057d`; see the following new record. Prior to the `8297b5057d` review, no durable open question was recorded here.

## Subsequent developments (2026-10-05, `8297b5057d`)

A later review verified a source-row gap analogous to the instruction-step gap: `PPCStepping::StepSourceRow::step_one` evaluates destination predicates without the guarded delay used for instruction/final-return stepping, and a temporary step-over return hit is still classified as `CodeBreakpoint`. See `2026-10-05-source-step-interruption-and-temporary-step-over-classification.md`. The instruction-step fix recorded above remains implemented and is not refuted; its cancellation/stop-generation guard contract has not yet been extended to `StepSourceRow`.

## Subsequent developments (2026-10-05, `88a5ddc3b9`)

A later read-only PR #5 review at `88a5ddc3b9` reproduced the analogous remaining gap in `PPCStepping::StepOut`: the final return edge executes without evaluating the destination breakpoint, so a step-out landing on an enabled breakpoint still reports `reason: "step"` with no `hitBreakpointIds`. This does not refute the instruction-step fix recorded here (instruction `next`/`stepIn` retain their destination evaluation); it extends the same engine-owned-predicate rationale to the step-out return edge. See `2026-10-05-stepout-return-destination-breakpoint-gap.md` for evidence and the recommended fix.

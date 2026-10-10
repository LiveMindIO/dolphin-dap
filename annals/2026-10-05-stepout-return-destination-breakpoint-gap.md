# Step-out skips evaluation of the breakpoint at its return destination

Date: 2026-10-05
Status: implemented
Scope: Shared `PPCStepping` step-out completion and destination breakpoint predicates (DAP and Qt).

## Git provenance

- Repository: Dolphin emulator debugger branch, `feature/gui-source-debugger`
- Canonical remote: `origin` (`https://github.com/Jacoby6000/dolphin.git`, redirects to `https://github.com/LiveMindIO/dolphin-dap.git`)
- Branch state: clean, tracking `origin/feature/gui-source-debugger`; untracked `annals/` preserved untouched
- Recording HEAD: `88a5ddc3b99aa2bce2df97a12452f2486197032d` (`88a5ddc3b9`), nearest describe `5.0-25543-g88a5ddc3b9`
- Source session: `ses_f07c8f576ffergQyBAxj2eibaJ`, response `msg_10d647bcf001MS1O6JbYk4tQub`
- Open finding, not yet remediated.

## Subsequent developments (2026-10-05, implemented)

The proposed fix was implemented, committed, and pushed as `8297b5057d14ba208a74dd152e971e2efdbed613` ("Debugger: check step-out return destination breakpoints"); PR #5 head verified at that SHA. `PPCStepping::StepOut` now evaluates `CheckAndHandleBreakPoints()` once after the final return, guarded by cancellation, `DidSteppingMemcheckHit`, and an unchanged pre-return stop generation. See `2026-10-05-stepout-return-destination-fix.md` for the full record. Status updated from `proposed` to `implemented`; the open question about extending the check to source-row/step-over return paths remains unresolved.

## Finding

`PPCStepping.cpp` step-out (`StepOut`, lines 186–255; specifically the final return path at lines 233–236) executes the return instruction and terminates without evaluating the destination breakpoint. Because `DapDebugController::CompleteStep` now correctly avoids existence-only breakpoint classification (see `2026-10-05-step-stop-causes-and-instruction-destination-breakpoints.md`), returning onto an enabled breakpoint produces a normal `Step` stop and drops the real hit:

- Temporary controller test `/tmp/opencode/pr5-return-destination-review.cpp`: `blr` at `0x3100` with `LR=0x3200` and an enabled destination breakpoint conditioned on `r3 = r3 + 1` reached `PC=0x3200`, but the condition never ran (`r3` stayed 0) and the event reported `ExecutionStopCause::Step` with no `code_breakpoint_address`.
- Temporary socket test `/tmp/opencode/pr5-stepout-socket-review.cpp`: verified instruction breakpoint at `0x3200`, then DAP `stepOut` from `blr` at `0x3100`. The terminal `stopped` event reported `reason: "step"` and omitted `hitBreakpointIds` (a normal `continued` event preceded it).
- Both temporary regressions failed as expected; all 1,901 existing tests passed without exclusions at `88a5ddc3b9`. No remote review posted; PR #5 head remained `88a5ddc3b9`.

## Context and problem

`129ff7b290` added destination breakpoint evaluation after acknowledged **instruction-level** steps, because `Interpreter::SingleStep()` does not check destination breakpoints. The analogous gap remains in step-out: the final return edge executes via `step_one()` without an intervening `CheckAndHandleBreakPoints()` pass, so a step-out that lands on a breaking breakpoint reports a step instead of a breakpoint hit.

## Options considered

- Evaluate the final destination predicate once in shared `StepOut`, using the same safeguards as the instruction-step path (skip when cancelled or when the stop generation changed; do not overwrite intervening data breakpoint/other stops). Chosen recommendation.
- Re-evaluate every iteration in step-out's inner loop. Rejected as redundant with the per-iteration `CheckBreakPoints()` flow that already runs inside `can_continue()`.
- Let `CompleteStep` classify destinations by existence again. Rejected; refuted by `6ba0dadb23`/`129ff7b290` experience.

## Rationale

Breakpoint semantics belong in the shared stepping engine. Step-out's return transition is the one acknowledged edge that can reach a new PC without a predicate check, so the real hit must be published there and `CompleteStep` must remain a normal `Step` completion.

## Consequences

- Until remediated, a `stepOut`/Qt step-out that returns onto an enabled breakpoint silently reports a normal step and never evaluates that breakpoint's condition.
- Clients cannot rely on `hitBreakpointIds` for step-out landing hits.
- Follow-up: add permanent controller and socket regressions mirroring the temporary reproducers.

## Follow-up obligations

- In `PPCStepping::StepOut`, after the final acknowledged return step, run the destination evaluation once with cancellation and stop-generation safeguards, preserving any intervening data/other stop.
- Add regression tests at both the `DapControllerTest` and `DapSessionTest` socket levels.
- Re-run the full suite; current baseline is 1,901 tests passing.

## Reconsideration triggers

- If step-out is restructured to route every PC transition through a single post-step hook, the extra final evaluation can be folded into that hook.

## Open questions

- Should the final destination check also apply to the source-row and step-over temporary-breakpoint return paths, or only to `StepOut`? Current evidence at `8297b5057d` covers only `StepOut` as implemented; `2026-10-05-source-step-interruption-and-temporary-step-over-classification.md` reproduces the remaining source-row and `StepOver` classification gaps.

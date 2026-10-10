# Source-step interruption guards and temporary step-over breakpoint classification

Date: 2026-10-05
Status: proposed
Scope: Shared `PPCStepping` destination-predicate evaluation and temporary step-over completion classification in DAP/Qt.

## Git provenance

- Repository: Dolphin emulator debugger branch, `feature/gui-source-debugger`
- Canonical remote: `origin` (`https://github.com/Jacoby6000/dolphin.git`, redirects to `https://github.com/LiveMindIO/dolphin-dap.git`)
- Branch state: clean and tracking `origin/feature/gui-source-debugger`; untracked `annals/` preserved
- Recording HEAD: `8297b5057d14ba208a74dd152e971e2efdbed613` (`8297b5057d`), nearest describe `5.0-25544-g8297b5057d`
- Source session: `ses_f07c8f576ffergQyBAxj2eibaJ`
- Source response: `msg_10d7b717800123HtIufXz84V47`
- PR head verified unchanged at `8297b5057d14ba208a74dd152e971e2efdbed613`; no implementation or remote review was submitted at that point.

## Findings

Two verified P2 findings remain open at `8297b5057d`:

1. Source-row stepping evaluates destination breakpoint predicates after cancellation or an intervening stop.
   - `Source/Core/Core/Debugger/PPCStepping.cpp`, `StepSourceRow::step_one`, lines 142-154, calls `CheckBreakPoints()` without the cancellation and unchanged-stop-generation checks present in the instruction-step path (`lines 52-60`) and the step-out final-return path (`lines 235-244`).
   - Temporary parameterized test `/tmp/opencode/pr5-source-interruption-review.cpp` schedules an interruption while stepping a NOP at `0x3100`, configures a real destination breakpoint at `0x3104` with side-effecting condition `r3 = r3 + 1`, and checks both cancellation and an external `UserPause` stop.
   - Reproduced 2026-10-05 at `8297b5057d`: both cases failed. In both, `r3` became `1` even though the destination condition should have been skipped as interrupted. Cancellation produced a `CodeBreakpoint` stop; pause produced a second `CodeBreakpoint` stop after the original `UserPause`.

2. Instruction step-over's internal temporary return breakpoint can be reported as a real breakpoint stop.
   - `PowerPCManager::CheckAndHandleBreakPoints` (`Source/Core/Core/PowerPC/PowerPC.cpp`, lines 661-673) publishes `ExecutionStopCause::CodeBreakpoint` for any hit, including the temporary breakpoint installed by `PPCStepping::StepInstructionOver` (`PPCStepping.cpp`, lines 105-109).
   - Temporary test `/tmp/opencode/pr5-temporary-step-review.cpp` drives the interpreter run loop for `bl` at `0x3100`, `blr` at `0x3140`, and return to `0x3104`, with only the temporary step-over return breakpoint. The owned stop was `CodeBreakpoint` with a code-breakpoint address rather than `Step`.
   - The unverified test initially compared `DAP::StepOverResult` to the wrong enum type; after fixing that temporary-harness issue, the defect reproduced at `8297b5057d`.

## Context and problem

This follows the instruction-step and step-out destination-predicate work recorded in `2026-10-05-step-stop-causes-and-instruction-destination-breakpoints.md` and `2026-10-05-stepout-return-destination-fix.md`.

Those fixes protect only acknowledged instruction-step and final step-out return edges. Source-row stepping still runs its destination-predicate helper unconditionally after `SingleStep()`, and a temporary internal return breakpoint reaches the same engine publisher as a genuine user breakpoint. The previous records correctly identify the analogous source-row/step-over question as open; this record preserves the reproducible evidence.

## Options considered

- Extend the shared stepping engine's existing interruption contract to source-row stepping: before `CheckBreakPoints()`, require no cancellation, `DidSteppingMemcheckHit() == false`, and unchanged pre-step stop generation. Chosen recommendation.
- Leave source-row predicates unguarded or classify the event later in DAP. Rejected; a side-effecting condition may already have mutated guest state, and the overwriting stop would already have been published to GUI and other clients.
- Preserve the internal temporary step-over hit as `CodeBreakpoint` and filter it in the DAP session translation. Rejected as incomplete: the shared execution event, Qt observers, and other DAP clients receive the wrong cause before filtering, and no client can distinguish a real coincidence from an internal completion edge there.
- Suppress every breakpoint stop that matches a temporary return address. Rejected; a genuine enabled breakpoint at the same address must retain `CodeBreakpoint`. The temporary mechanism needs to carry the step-completion context or a real coincident hit must be distinguished from the temporary service.
- Keep existence-only `Step` completion. Rejected by `6ba0dadb23` / `129ff7b290` experience: the engine must publish real predicates, but internal temporary hits must not borrow the user-breakpoint classification.

## Consequences

- At `8297b5057d`, source-stepping cancellation or a data/external stop can still run destination conditions and publish a second, overwriting stop.
- Instruction `next` over a call can report a breakpoint stop with `hitBreakpointIds` even when no user breakpoint exists at the return site.
- Permanent regression coverage should mirror the temporary tests: parameterized cancellation/external-stop source-step interruption, and an actual interpreter step-over completion test requiring `Step` and no fabricated address.
- Full existing suite at `8297b5057d` still passes: 1,909 tests, no exclusions (`/tmp/opencode/pr5-8297-review-suite.log`).

## Open questions

- How should temporary step-over hits be represented so a real enabled breakpoint at the same address still wins and any real predicate at that address runs exactly once?
- Should the source-row guard use the exact `CheckBreakPoints()` path as instruction/step-out, or a shared predicate-evaluation helper consumed by all three?
- If the call-over completes because of a real coincident breakpoint, how should the shared event preserve both owners/causes without double-publishing?

## Reconsideration triggers

- If source-row stepping is routed through the same post-step engine hook as instruction stepping, add the guard centrally rather than duplicating it.
- If temporary breakpoints gain metadata or ownership tags in `BreakPoints`, use that metadata to distinguish internal completion from user breakpoints.

## Follow-up obligations

- Guard `PPCStepping::StepSourceRow::step_one` destination evaluation by cancellation, stepping memcheck hit, and unchanged stop generation.
- Preserve the original external/data/cancellation stop instead of publishing a later step-cause breakpoint stop.
- Ensure temporary step-over completion maps to `Step`, while a real coincident breakpoint still maps to `CodeBreakpoint`.
- Add permanent `DapControllerTest`, `DapSessionTest`, and `PPCSteppingTest` coverage, then run the full 1,909-test baseline and selected regressions at least 20 repetitions.

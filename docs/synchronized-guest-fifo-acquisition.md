# Synchronized guest FIFO acquisition

**HACK/workaround, not a new input transport or a proven root-cause fix.**
Slippi Mainline's managed pipe path can sample before the writer's complete
controller batch is available. Advancing with that old state can retain input
debt. This workaround waits for one complete `FLUSH` from every bound pipe
before publishing the cohort and packing a new guest SI response.

## Contract

- Requires `OnSIRead`, `BlockingPipes=false`, and synchronized gameplay state.
  Despite that setting, acquisition deliberately waits synchronously.
- A real Slippi frame bookend supplies a writer turn. Repeated HI/LO reads and
  scheduled polls do not consume another turn. Direct SI transfers use the same
  acquisition path.
- Resolve current pad bindings, including explicit pipe-qualified controls and
  bindings on unplugged SI ports. Deduplicate shared pipe devices. Physical and
  unbound devices are not awaited.
- Stage commands privately through one completed `FLUSH` per selected pipe;
  publish only after every selected pipe completes. A bare `FLUSH` remains a
  real held-state turn, not a duplicate to discard.
- One shared one-second watchdog bounds acquisition. Missing devices, EOF,
  cancellation, or watchdog expiry invalidate the cohort, mark SI no-response,
  and request asynchronous emulation stop. They never become stale success.
- A game header alone does not wait for a nonexistent frame response. Menu
  polling retains its unsynchronized behavior. Faulted pipe devices require
  replacement; resetting SI must not publish a previously failed cohort.
- A transient debugger guard or user pause may request CPU stepping while the
  pending instruction completes. That alone is not cancellation. Shutdown and
  gameplay retirement still cancel; pause does not auto-resume the CPU.

## Runner dependency

Inputs still use libmelee FIFOs, **not DAP input**. No protocol, socket, or SDK
receipt is added here. The tested configuration also needs the companion Runner
timer-lock change: a writer must not wait for a pending DAP memory operation
while holding the lock needed to produce the cohort that operation awaits.
Changing the native pause predicate alone cannot resolve that lock inversion.

Keep the native and Runner changes coordinated. Match finalization/retirement
with pending DAP work remains a separate review concern; this document does not
certify that lifecycle or authorize deployment.

## Evidence and limits

The instrumented Slippi Mainline `e88d2ec`-derived candidate plus the companion
Runner completed two short Supercomputer games. Independent recording analysis
matched 15,866 checked whole-state packets, 1,006 rising button bits and 871
isolated pulses at constant source-frame +1, with no detected interior miss.
Eight boundary sources were excluded; the post-target export suffix was not
fully paired. This is an observed two-game result, not universal delivery,
endurance, SDK-acknowledgement, visual, or binary-equivalence certification.

This publication removes all recorder hooks. Source-owned unit tests exercise
real pipes and SI MMIO without a disc. Private booted interpreter/JIT fixtures
are not shipped: their fixed user directory and disc-specific environment are
not a portable test contract. Results from those diagnostic fixtures do not
stand in for tests of this uninstrumented publication artifact.

The workaround retains frame-bookend/FIFO association assumptions, holds input
configuration locks during the bounded wait, and changes the meaning of
nonblocking gameplay. It does not establish why older Slippi Ishiiruka behaved
differently, prove arbitrary writer/device lifetimes, or replace the pending
combined implementation review.

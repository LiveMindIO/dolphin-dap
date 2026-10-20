# Legacy Slippi input polling

`[Core] PollingMethod = OnSIRead` is the default, matching Ishiiruka Slippi.
It does not require `[Slippi] BlockingPipes = true`.

With blocking pipes disabled:

- Scheduled SI updates arm guest reads at the legacy 123-scanline interval.
- During gameplay, the first guest input-HI read consumes at most one complete
  pipe batch per device, without waiting for a writer. It snapshots every GC
  channel together; later channel reads use that generation. Input-LO remains
  paired with the captured response.
- Scheduled, GUI, and hotkey updates cannot consume gameplay pipe batches.
- Menu input still drains without a one-response-per-telemetry-event handshake.
  The existing bounded OS pipe provides backpressure; no unbounded queue is added.
  Buffering can add command age. Measure both age and delivery under live load.

`PollingMethod = Console` retains scheduled response latching. Movie playback and
netplay retain their existing polling path. Blocking pipes retain their existing
request-driven gameplay batch consumption and response-generation handling.
Menu events do not require an exact batch count. Game headers clear stale menu
requests without requiring an impossible response before the first frame.
Pipe waits yield to debugger-requested CPU pauses so DAP memory access can finish.

Regression coverage is in `SerialInterfaceTest` and `PipesTest`, including
nonblocking operation without bookends, HI/LO pairing, and host-update ownership.
Tests reproduce coalesced and separately scheduled press/release loss before
guest sampling. Live memory-only recording found all three residual losses in
the scheduled implementation were released before a guest SI read. An initial
guest-boundary capture delivered 134/134 isolated presses, but added command age
(median 25.8 ms, p95 42.6 ms). A longer capture delivered 160/160 during its live
segment (median 16.9 ms, p95 28.0 ms), then disconnected over ENet in later CSS;
the disconnect cause is unknown. The recorder-free image subsequently completed
two saved-match transitions and started another match. All 1,971 native tests
pass. This is not a loss-free guarantee or a controlled before/after rate
comparison; neither the rendering assertion nor injection ownership is fixed.

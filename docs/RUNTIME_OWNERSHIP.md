# Runtime Ownership Contract

This repo’s main loop is split into a small set of ownership phases so state mutation stays explicit. The goal is to prevent new features from drifting back into duplicate owners, read-side effects, or order-dependent hidden behavior.

## Per-loop ownership

1. `processLoopConnectionEarlyPhase(...)`
Owns early connection-derived decisions such as boot splash hold, connection backpressure, and whether the loop should shed non-core work.

2. `processLoopIngestPhase(...)`
Owns BLE ingest, queue drain, GPS runtime refresh, and the loop settings snapshot.
This phase is the producer for `LoopSettingsPrepValues`.

3. `loop()` in [src/main.cpp](/Users/ajmedford/v1g2_simple/src/main.cpp)
Owns the OBD runtime refresh and speed selection refresh.
`obdRuntimeModule.update(now, ...)` and `speedSourceSelector.update(now)` must run before any display, lockout, or Wi-Fi consumer reads their state.

4. `processLoopDisplayPreWifiPhase(...)`
Consumes current-loop snapshots and runs display/lockout-side work plus the pre-Wi-Fi auto-push hook.
This phase does not export mutable phase state back to `loop()`.
Parsed-frame BLE volume execution for lockout pre-quiet and alert fade lives under this phase via `DisplayOrchestrationModule`.

5. `processLoopWifiPhase(...)`
Consumes the per-loop settings snapshot and runtime snapshot, then owns Wi-Fi runtime progression for that loop.

6. `processLoopFinalizePhase(...)`
Owns post-display connection-state dispatch cadence, periodic maintenance, and loop-tail bookkeeping.

## Producer and consumer rules

- Producers call `update(...)` exactly once per loop for state that advances over time.
- Consumers read `snapshot()` for committed state.
- If a caller needs a non-committing point-in-time view, it must use a pure read helper such as `speedSourceSelector.snapshotAt(nowMs) const`.
- Read helpers must not increment counters, switch sources, or otherwise mutate runtime state.

## Current explicit owners

- BLE ingest state: `LoopIngestModule`
- GPS runtime freshness/state: `LoopIngestModule` via `gpsRuntimeModule.update(nowMs)`
- OBD runtime freshness/state: `loop()` in [src/main.cpp](/Users/ajmedford/v1g2_simple/src/main.cpp)
- Speed arbitration/counters: `loop()` in [src/main.cpp](/Users/ajmedford/v1g2_simple/src/main.cpp) via `speedSourceSelector.update(now)`
- Display/lockout refresh: `LoopDisplayModule`
- Parsed-frame BLE volume execution: `DisplayOrchestrationModule`
- Wi-Fi runtime progression: `WifiRuntimeModule`
- Connection-state dispatch cadence: `LoopPostDisplayModule`

## Anti-drift rule

When a new feature needs shared runtime state, choose one mutating owner first. Any other caller must consume snapshots or explicit outputs from that owner instead of recomputing or re-updating the same state.

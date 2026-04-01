# Runtime Ownership Contract

This repo’s main loop is split into a small set of ownership phases so state mutation stays explicit. The goal is to prevent new features from drifting back into duplicate owners, read-side effects, or order-dependent hidden behavior.

## Per-loop ownership

1. `processLoopConnectionEarlyPhase(...)`
Owns early connection-derived decisions such as boot splash hold, connection backpressure, and whether the loop should shed non-core work.

2. `processLoopIngestPhase(...)`
Owns BLE ingest, queue drain, OBD runtime refresh, and the loop settings snapshot.
This phase is the producer for `LoopSettingsPrepValues`.

3. `loop()` in [src/main.cpp](../src/main.cpp)
Owns the OBD runtime refresh and speed selection refresh.
`obdRuntimeModule.update(now, ...)` and `speedSourceSelector.update(now)` must run before any display or Wi-Fi consumer reads their state.

4. `processLoopDisplayPreWifiPhase(...)`
Consumes current-loop snapshots and runs display-side work plus the pre-Wi-Fi auto-push hook.
This phase does not export mutable phase state back to `loop()`.
`LoopDisplayModule` is a side-effect-only phase module; it does not return test-only priority state to `loop()`.
Parsed-frame BLE volume execution for alert fade lives under this phase via `DisplayOrchestrationModule`.

5. `processLoopWifiPhase(...)`
Consumes the per-loop settings snapshot and runtime snapshot, then owns Wi-Fi runtime progression for that loop.
Its phase context is data-only; `WifiRuntimeModule` executes Wi-Fi manager progression only through `begin(...)` providers.

6. `processLoopFinalizePhase(...)`
Owns post-display connection-state dispatch cadence, periodic maintenance, and loop-tail bookkeeping.

## Producer and consumer rules

- Producers call `update(...)` exactly once per loop for state that advances over time.
- Consumers read `snapshot()` for committed state.
- If a caller needs a non-committing point-in-time view, it must use a pure read helper such as `speedSourceSelector.snapshotAt(nowMs) const`.
- Read helpers must not increment counters, switch sources, or otherwise mutate runtime state.
- Phase context structs are data-only loop inputs. Execution hooks are configured once via module `begin(...)` providers rather than injected ad hoc per call.
- Boot and loop-carried runtime state in `main.cpp` is grouped under `MainRuntimeState` instead of scattered file-static variables.

## Current explicit owners

- BLE ingest state: `LoopIngestModule`
- OBD runtime freshness/state: `loop()` in [src/main.cpp](../src/main.cpp)
- Speed arbitration/counters: `loop()` in [src/main.cpp](../src/main.cpp) via `speedSourceSelector.update(now)`
- Display refresh: `LoopDisplayModule`
- Display pipeline throttles and recovery cadence: `DisplayPipelineModule` instance state
- Auto-push request arbitration/execution: `AutoPushModule` via `queueSlotPush()` / `queuePushNow()` into `process()`
- Parsed-frame BLE volume execution: `DisplayOrchestrationModule`
- Wi-Fi runtime progression: `WifiRuntimeModule`
- Connection-state dispatch cadence: `LoopPostDisplayModule`
- Domain-specific settings writes: `WifiSettingsApiService::handleApiDeviceSettingsSave()` owns AP/proxy/power/dev writes on `POST /api/device/settings`.
- Runtime reapply after persisted settings changes: `SettingsRuntimeSync` helper bundles own the shared OBD/selector reapply groupings so restore and config writes do not hand-roll divergent apply paths.

Speed consumers must not derive fallback speed from raw OBD runtime state. If the selector did not publish a committed speed for the current loop, speed is treated as unavailable.

## Resolved duplicate-owner consolidations

- Auto-push execution: connect-time selection, tap-triggered slot changes, manual profile pushes, and explicit push-now requests now queue through `AutoPushModule` request APIs before `AutoPushModule::process()` executes them. New requests must not clobber an in-flight push or bypass the shared retry/error path.
- Alert-time volume transitions: `DisplayOrchestrationModule` is now the sole BLE volume executor. `VolumeFadeModule` only emits explicit decisions for it to arbitrate. Fade/restore execution must not issue a competing `setVolume()` in the same alert path.
- Wi-Fi start/re-enable lifecycle: `WiFiManager` lifecycle state now owns AP/STA/AP+STA transitions. Boot auto-start and touch/manual UI paths request `WiFiManager::startSetupMode(...)` directly, while `WifiOrchestratorModule` is limited to one-time Wi-Fi callback binding and must not mutate lifecycle state in parallel.
- Speed arbitration: `speedSourceSelector.update(now)` remains the mutating owner for committed speed selection. OBD config and restore paths may only sync selector enable inputs through `speedSourceSelector.syncEnabledInputs(obdEnabled)`; downstream modules may read `selectedSpeed()` / `snapshot()` or pure `snapshotAt(nowMs)` views, but they must not reinterpret raw OBD runtime state as fallback arbitration.

## Anti-drift rule

When a new feature needs shared runtime state, choose one mutating owner first. Any other caller must consume snapshots or explicit outputs from that owner instead of recomputing or re-updating the same state.

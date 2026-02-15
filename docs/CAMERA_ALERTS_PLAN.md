# Camera Alerts Plan (Draft)

> Status: Draft v0.8 (implementation-aligned)  
> Date: February 15, 2026  
> Scope: New camera alerts built on current GPS runtime and current main loop

## Goal

Add camera alerts without destabilizing the core detector pipeline.

## Implementation Status

- Camera runtime hook is active in `src/main.cpp` with low-priority guard usage.
- Loader task, immutable index build/swap, and enforcement matching are implemented under `src/modules/camera/`.
- Camera APIs are active in `src/wifi_manager.cpp`: `/api/cameras/status`, `/api/cameras/events`, and `/api/cameras/catalog`.
- Camera UI page is active at `interface/src/routes/cameras/+page.svelte` with runtime/status/event/catalog views.
- Persistent `cameraEnabled` setting is active and gated by GPS:
  `effectiveCameraEnabled = gpsEnabled && cameraEnabled`.
- Forward-only camera lifecycle is active with:
  - signal preemption by live V1 alerts,
  - one-shot camera display token rendering,
  - one-shot `"<type> ahead"` voice on camera alert start.

## Hard Constraints

1. Do not reuse `_disabled` code paths in runtime.
2. Use current GPS runtime only (`gpsRuntimeModule.snapshot(nowMs)`).
3. Core path always wins:
   `BLE/process + parser + display pipeline + mute commands`.
4. OBD, GPS enrichment, and camera work are lower priority than core.
5. Fail open: if camera work is skipped/fails, core behavior remains unchanged.
6. Camera dataset allocations for M2+ must not consume internal SRAM at
   runtime scale; large camera buffers are PSRAM-only.
7. Camera dataset loading must never run as blocking file I/O in `loop()`.

## Current Reality (Code-Accurate)

1. Active GPS runtime is in `src/modules/gps/gps_runtime_module.*`.
2. Active GPS ring log is in `src/modules/gps/gps_observation_log.*`.
3. Overload guards already exist in `src/main.cpp`:
   `skipNonCoreThisLoop` and `overloadThisLoop`.
4. Active GPS APIs exist in `src/wifi_manager.cpp`:
   `/api/gps/status` and `/api/gps/observations`.
5. Active camera runtime hook exists in `src/main.cpp`:
   `cameraRuntimeModule.process(now, skipNonCoreThisLoop, overloadThisLoop, parser.hasAlerts())`.
6. Active camera APIs exist in `src/wifi_manager.cpp`:
   `/api/cameras/status`, `/api/cameras/events`, `/api/cameras/catalog`.
7. Loader task and atomic ready-buffer swap are active in
   `src/modules/camera/camera_data_loader.*`.
8. Camera binary data files exist in `camera_data/` and camera build
   tooling exists in `tools/`.
9. Camera web UI route + nav entry exist in `interface/src/routes/cameras/+page.svelte`
   and `interface/src/routes/+layout.svelte`.

## Legacy Rejection

- `_disabled/gps_handler.*` is not a dependency target.
- `_disabled/lockout_manager.*` and `_disabled/auto_lockout_manager.*` are not implementation bases.
- `_disabled` is reference-only for lessons learned.

## Priority Model

1. `P0 Core`: BLE state machine, packet processing, display pipeline, mute execution.
2. `P1 Secondary`: OBD sampling, speed source arbitration, GPS ingest.
3. `P2 Camera`: camera matching, camera event logging, camera UI/audio overlays.

Camera work must not preempt `P0` or force `P1` regressions.

## Minimal-Impact Architecture

Add new code only under `src/modules/camera/`:

- `camera_runtime_module.h/.cpp`: runtime tick + gating + cooldown logic.
- `camera_index.h/.cpp`: immutable spatial index for fast nearby lookup.
- `camera_data_loader.h/.cpp`: best-effort binary load/reload.
- `camera_event_log.h/.cpp`: bounded ring for diagnostics/API (main-loop-owned in M2/M3).

Boundary rule: main loop gets one call (`cameraRuntimeModule.process(...)`) and one lightweight snapshot accessor. No camera logic in core BLE/parser/display internals.

## Main Loop Integration Contract

Implemented insertion point: low-priority section after parsed-frame display handling and behind overload guards.

Rules:

1. Never run camera tick when `skipNonCoreThisLoop` is true.
2. Skip camera tick when `overloadThisLoop` is true.
3. Camera tick frequency hard-limited (`<= 5Hz` initially).
4. No filesystem I/O in tick path.
5. No JSON parsing in tick path.
6. No blocking waits in tick path.
7. M2+ load/reload work runs outside `loop()` and publishes data via a
   non-blocking swap/ready flag contract.

If any guard trips, increment camera skip counters and return immediately.

## Loader Task Contract (M2 Commit)

1. Camera load/reload runs in dedicated FreeRTOS loader task, not in `loop()`.
2. Loader task priority is below BLE/display critical work (`priority=1` target).
3. Loader reads camera files in bounded chunks (`~1024 records` per chunk) and
   yields with `vTaskDelay(1)` between chunks.
4. Loader builds an inactive index buffer in-task end-to-end:
   decode, key derivation, sort, and span-table build.
5. Any long-running build phase must stay in the loader task and yield
   cooperatively (`vTaskDelay(1)`) between bounded sub-steps.
6. Runtime continues using current active index until swap.
7. Publish step uses atomic readiness/version state and pointer swap so
   `cameraRuntimeModule.process(...)` never blocks.
8. Reload requests are coalesced (one pending flag/version) to avoid task spam.

This contract is now implemented for M2; any changes to task/swap behavior must
preserve ESP32/FreeRTOS safety and non-blocking loop guarantees.

## GPS Contract for Camera Runtime

1. Read GPS state only from `GpsRuntimeStatus` snapshot.
2. Require `enabled && hasFix && locationValid` before matching.
3. Initial thresholds (M2 default):
   - `sampleAgeMs > 2000`: skip matching.
   - `speedMph < 3`: skip matching.
   - `sampleAgeMs > 1000`: distance-only (no heading corridor).
4. Do not read GPS UART/serial directly from camera code.
5. M4 forward-only requirement:
   - camera alerting is allowed only for cameras ahead of travel direction,
   - non-forward cameras must never alert.
6. Heading/course prerequisites for M4:
   - expose course-over-ground in `GpsRuntimeStatus` (parsed from NMEA RMC),
   - require heading sample freshness (`<= 2000 ms`),
   - if heading is missing/stale, do not start camera alert (fail open).
7. Forward corridor gate (M4 defaults):
   - compute bearing from current GPS position to candidate camera,
   - allow alert start only when heading delta is within entry corridor
     (`|delta| <= 35 deg`).
8. Turn-away graceful clear (M4 defaults):
   - if active camera heading delta exceeds clear corridor (`|delta| >= 55 deg`)
     for 2 consecutive camera ticks, clear alert state without re-show,
   - if user turns away before arrival, camera alert clears and stays cleared
     for that pass (one-and-done behavior).

## Camera Data Contract

Runtime input format: binary `VCAM` records (header + fixed-size records).

Phase-1 loader behavior:

1. Support current binary naming used in repo artifacts:
   `alpr.bin`, `redlight_cam.bin`, `speed_cam.bin`.
2. Validate header/version/record size before accepting data.
3. Build immutable in-memory index once load succeeds.
4. On load failure, leave camera runtime disabled and non-fatal.
5. Enforce pre-allocation guard before load:
   `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` must satisfy required
   buffer target with headroom.
6. If pre-allocation guard fails: increment load-failure counters, keep camera
   runtime disabled, and do not fall back to internal SRAM.

Dataset staging for rollout:

1. M2 default: load enforcement sets first (`speed_cam.bin`, `redlight_cam.bin`).
2. ALPR dataset (`alpr.bin`) is optional and separately gated.

## Matching Strategy

1. Convert coordinates to fixed-point (`E5`) for index keys.
2. Use coarse buckets/grid to narrow candidates.
3. Enforce hard raw scan cap per tick (`<=128` records visited before
   distance filter); stop span walk once cap is reached.
4. M2 started distance-only matching; current implementation uses
   heading-corridor forward-only matching for camera alert starts.
5. Cooldown state is bounded: reuse event ring as cooldown cache (no full
   per-camera cooldown table).
6. Avoid dynamic map allocation for cooldown state at full dataset scale.

## Spatial Index Design (M2 Commit)

1. Camera records are kept in one contiguous PSRAM array.
2. Grid metadata is span-based:
   `CellSpan{cellKey, beginIndex, endIndex}`.
3. Cell key resolution: `0.01°` lat/lon grid.
4. Span-memory budget contract:
   - compute projected span bytes as `uniqueCellCount * sizeof(CellSpan)`,
   - enforce `CameraIndex::kSpanBudgetBytes` (M2 default target: 64 KiB),
   - if projected span bytes exceed budget, reject that dataset load in M2.
5. M2 operational mode:
   - enforcement datasets (`speed_cam.bin`, `redlight_cam.bin`) are default,
   - ALPR remains optional and stays disabled unless span-budget and perf
     checks pass.
6. Load path:
   - decode VCAM records,
   - compute `cellKey`,
   - sort by `cellKey`,
   - build compact span table.
7. Query path:
   - compute current cell,
   - scan 3x3 neighboring cells via span lookup,
   - apply raw scan cap first (`<=128` visited records, overflow counted),
   - run exact distance filter on visited records.
8. Span metadata currently resides in PSRAM (same class as records) to keep
   internal SRAM pressure low; budget checks still gate dataset acceptance.

This design is now fixed for M2 unless profiling shows a hard regression.

## Event Log Ownership Contract (M2/M3)

1. `CameraEventLog` is main-loop-owned in M2/M3.
2. Publish path: main loop only.
3. Read path: API/status code consumes loop-produced snapshots; no direct
   cross-thread mutation/read of ring internals.
   Current implementation detail: camera API handlers execute from
   `wifiManager.process()` in `loop()`, so reads are same-context today.
4. If any background thread needs direct access in future, `CameraEventLog`
   must be upgraded to explicit synchronization (`portMUX` or equivalent)
   before enabling that access.

## Initial Performance Targets

- Camera tick: `<= 250us` typical, `<= 800us` capped worst-case.
- Match cadence: max `5Hz`.
- Event append: O(1), bounded ring.
- Budget breach response: auto-throttle and counter increments.

Targets are provisional until hardware profiling confirms them.

## API Rollout (Phased)

### Phase 1 (Read-Only)

- `GET /api/cameras/status`
- `GET /api/cameras/events`
- `GET /api/cameras/catalog`

### Phase 2 (Controlled Write)

- Camera runtime toggle is currently provided via `POST /api/settings` with
  `cameraEnabled` (persistent), still GPS-gated at runtime.
- `POST /api/cameras/reload` remains deferred/best-effort.

No test/sync/upload endpoints until runtime stability is verified.

## UI Rollout (Phased)

1. Camera diagnostics/status card is implemented.
2. Recent camera event table is implemented.
3. Dataset catalog table (`alpr`, `speed`, `redlight`) is implemented.
4. Camera enable toggle is implemented (persistent `cameraEnabled` setting).
5. Reload control remains deferred until additional runtime profiling.

No high-frequency polling changes until camera runtime impact is measured.

## M4 Camera Alert UX Contract (Draft)

1. Reuse existing display primitives only:
   - no new fonts,
   - no new draw regions/layout blocks,
   - no separate camera-specific renderer.
2. Camera label rendering must reuse the existing frequency/7-seg path as far
   as practical for limited text display.
3. Use the same top-arrow glyph path already used by V1 alerts for `^`;
   do not introduce a new arrow asset or alternate symbol pipeline.
4. Camera on-screen format is minimal and type-only:
   - `~ ALPR ^`
   - `~ SPEED ^`
   - `~ REDLIGHT ^`
5. Audio behavior is one-shot at alert start:
   - announce `"<type> ahead"` once,
   - do not repeat while the same camera event remains active.
6. No "camera clear" voice cue in baseline M4 (intentionally omitted to avoid
   annoyance).
7. No distance countdown UI/audio (no `X feet` ticks, no repeated distance
   refreshes).
8. Clear-to-resting rule:
   - clear camera UI once inside ~100 ft (`~30 m`) of the matched point, or
   - clear immediately if camera eligibility/match is no longer valid.
9. Forward-only rule:
   - camera UI/audio can trigger only when the matched camera is in the
     forward heading corridor from GPS course.
   - if heading corridor is lost (turn-away), clear gracefully and do not
     re-show in that pass.
10. Signal preemption rule:
   - any live V1 signal immediately takes display/audio priority over camera.
11. One-and-done behavior:
    - after preemption or clear, do not re-show the same camera alert in that
      pass; rely on cooldown/debounce for future re-entry.
12. "Ahead" wording is strict in M4:
    - "ahead" must be backed by heading corridor checks, not proximity-only.

## Observability

Add counters:

- `cameraTicks`
- `cameraTickSkipsOverload`
- `cameraTickSkipsNonCore`
- `cameraTickSkipsMemoryGuard`
- `cameraCandidatesChecked`
- `cameraMatches`
- `cameraAlertsStarted`
- `cameraBudgetExceeded`
- `cameraLoadFailures`
- `cameraLoadSkipsMemoryGuard`
- `cameraIndexSwapCount`
- `cameraIndexSwapFailures`

Expose via status API and perf snapshots.
Also expose loader/tick timing and memory telemetry:
- `cameraLastTick_us`, `cameraMaxTick_us`
- `cameraLastLoadMs`, `cameraMaxLoadMs`, `cameraLastSortMs`, `cameraLastSpanMs`
- `cameraLastInternalFree`, `cameraLastInternalBlock`, guard thresholds

## M2 Safe Implementation Order

1. Wire guarded runtime hook + status snapshot only (still no matching).
2. Implement loader task with decode/sort/span build + atomic inactive->active
   swap, then verify no loop-blocking behavior under reload.
3. Enable enforcement-only query path (`speed+redlight`) with raw scan cap and
   counters, still no UI/audio side effects.
4. Add read-only APIs (`/api/cameras/status`, `/api/cameras/events`,
   `/api/cameras/catalog`) and verify
   event-log ownership contract remains loop-only.
5. Add camera UI observability page and persistent enable toggle.
6. Run perf/soak gates before any M3+ audio/display behavior.

## Testing Gates

### Unit

1. Bucket narrowing correctness.
2. Radius/corridor match correctness.
3. Cooldown/debounce behavior.
4. No-fix/stale-sample gating.
5. Candidate cap enforcement.

### Integration

1. No BLE reconnect regression.
2. No display cadence regression.
3. No OBD/GPS starvation with camera enabled.
4. Camera disabled/failure path leaves behavior unchanged.

### Soak

1. Long-run memory stability.
2. No queue-drop growth vs baseline.
3. No core-loop latency regression vs baseline.

## Milestones

1. `M1` (complete): scaffolding + no-op hook + counters.
2. `M2` (implemented): binary load + immutable index + read-only API.
3. `M3` (implemented): lifecycle hardening + heading/course plumbing in GPS snapshot.
4. `M4` (implemented baseline): controlled one-shot display/audio integration with signal preemption.
5. `M5` (pending): optional controlled reload endpoint.

Advancement requires hardware perf check at each milestone.

## Executable Implementation Path (Post-Planning, No-Code)

This is the concrete execution order to move from current M2 runtime to
forward-only M4 camera UX without violating core-priority constraints.

### Stage 0: Spec Lock (Docs + API Contract Only)

1. Freeze camera display token set for limited 7-seg rendering:
   - `ALPR`, `SPEED`, `REDL` (or equivalent fixed set).
2. Freeze arrow source:
   - camera uses existing V1 front-arrow renderer path only (`DIR_FRONT`).
3. Freeze one-and-done lifecycle semantics:
   - alert start once, clear on pass/turn-away/preempt, no re-show in same pass.
4. Freeze audio baseline:
   - one-shot `"<type> ahead"` only if assets/API are available;
   - verified on `2026-02-14`: source clips exist in `tools/camera_audio/`
     (`cam_alpr.mul`, `cam_speed.mul`, `cam_redlight.mul`, `cam_both.mul`);
     active playback should still reuse `data/audio/dir_ahead.mul`;
   - otherwise ship display-only first and defer camera voice to follow-up.

### Stage 1 (M3-A): Heading/Course Plumbing in GPS Runtime

1. Add course fields to `GpsRuntimeStatus`:
   - `courseDeg`, `courseValid`, `courseSampleTsMs`, `courseAgeMs`.
2. Parse NMEA RMC course-over-ground and publish into snapshot.
3. Expose course validity/age in GPS API + perf telemetry.
4. Add unit tests for:
   - valid RMC course parse,
   - missing/invalid course handling,
   - staleness aging behavior.

### Stage 2 (M3-B): Forward-Only Match Gate in Camera Runtime

1. Keep current spatial narrowing + raw scan cap unchanged.
2. Add heading gate before alert start:
   - prefer record bearing metadata (`bearingTenthsDeg`, `toleranceDeg`) when valid,
   - fallback to geometric bearing only when record bearing is unknown.
3. Apply corridor thresholds with hysteresis:
   - entry `|delta| <= 35 deg`,
   - clear `|delta| >= 55 deg` for 2 ticks.
4. If heading is missing/stale:
   - fail open (no new camera alert start).

### Stage 3 (M3-C): Camera Alert Lifecycle Controller

1. Add explicit camera lifecycle state machine:
   - `IDLE`, `ACTIVE`, `PREEMPTED`, `SUPPRESSED_UNTIL_EXIT`.
2. Persist active camera context minimally:
   - camera id/type, start ts, last heading delta, clear reason.
3. Clear conditions:
   - within pass distance (~30 m),
   - heading corridor lost (turn-away),
   - camera eligibility invalid.
4. Re-show policy:
   - never re-show same pass after preempt/clear; require exit + re-entry.

### Stage 4 (M4-A): Minimal Display Integration

1. Reuse existing display primitives only (no new fonts/layout regions).
2. Render camera token in existing frequency area.
3. Render `^` using existing V1 arrow renderer path.
4. Immediate V1-signal preemption:
   - any live V1 alert overrides camera display on first frame.
5. No camera resume after preempt in same pass.

### Stage 5 (M4-B): Optional Camera Voice (After Drive Validation)

1. Keep camera voice best-effort and one-shot.
2. No "camera clear" voice in baseline.
3. Reuse existing V1 direction clip path for "`ahead`" (`dir_ahead.mul`);
   avoid any new audio pipeline/threading code in baseline.
4. `_disabled/` is not a source-of-truth for camera audio assets; only use
   assets that exist under active `data/audio` (or newly generated/committed
   assets validated in-repo).
5. If camera voice causes latency/jitter risk, disable by default and defer.

### Stage Gates (Must Pass Before Advancing)

1. No BLE reconnect reliability regression.
2. No display cadence regression vs baseline.
3. No additional WiFi SRAM watchdog incidents attributable to camera path.
4. Verified turn-away clear behavior in real drive logs.
5. Verified no camera re-show after preempt in same pass.

## Exit Criteria

1. Core loop behavior unchanged under normal and stress runs.
2. No measurable BLE/display reliability regression.
3. Camera subsystem degrades gracefully under resource pressure.
4. Docs/API only describe actually shipped endpoints and modules.

---
## Trust-But-Verify Audit (February 14, 2026)

### Verified Data (Repository Checks)

| File | Records | Bytes |
|------|---------|-------|
| `alpr.bin` | 70,327 | 1,687,864 |
| `speed_cam.bin` | 1,125 | 27,016 |
| `redlight_cam.bin` | 205 | 4,936 |
| **Total** | **71,657** | **1,719,816** |

Verified from VCAM headers (`magic=VCAM`, `version=1`, `recordSize=24`) and
file-size consistency checks.
Runtime note: `RawVcamRecord` is 24 bytes on disk; `CameraRecord` is 28 bytes
in memory after adding derived `cellKey`.

### Verified Grid/Sizing Facts (`0.01°`, February 14, 2026)

| Dataset | Unique Cells | Est. Span Bytes (12B/CellSpan) | Max 3x3 Fanout |
|---------|--------------|----------------------------------|----------------|
| Enforcement only (`speed+redlight`) | 792 | ~9.3 KiB | 39 |
| All datasets (`speed+redlight+alpr`) | 38,623 | ~452.6 KiB | 81 |

Implications:

1. Current data does **not** show 3x3 candidate explosion (`max=81`), so
   `<=128` raw scan cap is currently sufficient.
2. Enforcement-only span metadata remains small; current implementation keeps
   spans in PSRAM to avoid internal SRAM pressure.
3. Full ALPR span metadata is too large for current 64 KiB span budget and
   must remain gated/deferred unless budget policy changes.

### Finding Verification Matrix

| ID | Verdict | Verification Status | Notes |
|----|---------|---------------------|-------|
| F1 PSRAM omission risk | Confirmed risk | **Verified + inferred impact** | Dataset size is verified. Platform PSRAM flags are verified. Exact crash mode/threshold depends on runtime heap state. |
| F2 No index detail | Gap addressed for M2 (ALPR path deferred) | **Verified with repository data + sizing math** | v0.6/v0.7 lock scan-cap ordering, span budget checks, and enforcement-first loading. |
| F3 PSRAM latency budget | Confirmed gap | **Needs hardware measurement** | Repo does not contain camera PSRAM access benchmarks yet. |
| F4 Load blocking risk | Gap addressed in plan | **Verified + estimated timing** | v0.5 fixes loader-task contract. 400-800 ms timing remains estimate pending device measurement. |
| F5 Missing heading in snapshot | Confirmed | **Verified** | `GpsRuntimeStatus` has no heading field; `parseRmc` does not parse course field. |
| F6 Stale/speed thresholds undefined | Gap addressed in plan | **Verified** | v0.5 fixes explicit age/speed defaults; implementation validation pending. |
| F7 Cooldown memory unsized | Gap addressed in plan | **Verified** | v0.5 fixes bounded cooldown strategy (event-ring cache). |
| F8 ALPR dominance | Gap addressed in plan | **Verified data** | Data skew is verified; v0.5 fixes staged loading (enforcement first, ALPR gated). |
| F9 Doc filename mismatch | Confirmed | **Verified** | ARCHITECTURE/DEVELOPER still reference removed camera module names. |
| F10 Event-log thread model unstated | Gap addressed in plan | **Verified** | v0.5 codifies main-loop ownership and upgrade rule before cross-thread access. |
| F11 OOM recovery path undefined | Gap addressed in plan | **Verified** | v0.5 fixes preflight and no-internal-fallback policy; implementation validation pending. |

### Decisions Locked For M2/M3

1. **Memory policy:** camera record buffers at runtime scale are PSRAM-only.
   If PSRAM preflight fails, camera stays disabled.
2. **No internal fallback:** do not retry large camera buffers in internal
   SRAM.
3. **Load policy:** no blocking camera file load in `loop()`.
4. **Read/publish model:** camera runtime reads immutable active index only;
   loader prepares replacement index and swaps readiness atomically.
5. **Matching defaults:** until heading support exists in GPS snapshot, use
   distance-only matching with thresholds in GPS contract above.
6. **Cooldown policy:** bounded structure only (no full per-camera table).
7. **Dataset staging:** enforcement sets are default path; ALPR stays separate
   and gated.
8. **Scan cap semantics:** cap applies to raw span walk before distance filter.
9. **Build locality:** decode/sort/span build stay fully in loader task, never
   in `loop()`.

### Real Open Issues (After v0.7 Alignment)

1. **Hardware validation still required**:
   PSRAM access behavior and load timing must be profiled on target hardware.
2. **Forward-only M4 matching is blocked until heading is wired**:
   `GpsRuntimeStatus` currently has no heading/course field and
   `parseRmc(...)` does not publish course-over-ground yet.
3. **ALPR full-dataset mode remains deferred past M2**:
   with current data, full `0.01°` span metadata projects to ~452.6 KiB and
   exceeds current M2 span budget; future path is captured in
   "Known M4+ ALPR Design Fork" below.
4. **Count/decode still use a two-pass open/read flow**:
   accepted for M2 simplicity; single-pass optimization can be evaluated after
   hardware profiling.
5. **Cross-doc drift remains**:
   `ARCHITECTURE.md`, `DEVELOPER.md`, and `API.md` still contain stale camera references.

### Low-Severity Implementation Findings (Verified February 14, 2026)

1. `cameraRuntimeModule.begin(...)` now follows `gpsEnabled` at boot and runtime setting changes.
2. Cooldown scan now early-exits once entries are older than `kCooldownMs` (newest-first ring copy).
3. Plan now explicitly distinguishes 24-byte on-disk VCAM records vs 28-byte in-memory `CameraRecord`.
4. Loader count pass + decode pass double-open remains a known optimization item (non-blocking, low risk).
5. Loader failure branches now emit explicit `Serial` diagnostics for field debugging.
6. Event-log read ownership is now annotated where WiFi handlers read snapshots in loop context.

### Known M4+ ALPR Design Fork

If ALPR full-dataset mode is enabled in M4+, choose one of these designs
explicitly during M4 design review (do not mix both in first pass):

1. **Option A: PSRAM span metadata + PSRAM records**
   Keep current span-table query model, but allow ALPR spans to reside in
   PSRAM alongside records. Tradeoff: higher/random query latency risk.
2. **Option B: PSRAM sorted array + binary-search bounds**
   Remove ALPR span table requirement; keep ALPR records sorted by `cellKey`
   and derive per-cell ranges with binary searches at query time. Tradeoff:
   extra compare work per query, lower SRAM footprint.

Decision gates for M4+:

1. Must pass loop-latency and BLE stability gates from this plan.
2. Must show query latency distribution on hardware (not synthetic only).
3. Must include explicit fallback behavior if ALPR path exceeds budget in
   runtime conditions.

### Follow-Up Documentation Debt

1. Update `docs/ARCHITECTURE.md` camera module names to current `src/modules/camera/*`.
2. Update `docs/DEVELOPER.md` camera module references to current files.
3. Align `docs/API.md` camera endpoints/settings with actual implementation
   status per milestone.

---

## HW Field Test: WiFi vs Camera SRAM Contention (Feb 14, 2026)

### Observed Failure

With camera loader running and WiFi in AP+STA mode, WiFi self-shutdown was
triggered by the internal SRAM watchdog:

```
[WiFi] WARN: Internal SRAM low (mode=AP+STA free=19392 block=11252 need>=20480/10240) - grace 1500 ms
[WiFi] RECOVERED: Internal SRAM back above threshold after 6 ms
[WiFi] WARN: Internal SRAM low (mode=AP+STA free=19392 block=11252 need>=20480/10240) - grace 1500 ms
[WiFi] RECOVERED: Internal SRAM back above threshold after 5 ms
[WiFi] WARN: Internal SRAM low (mode=AP+STA free=19392 block=11252 need>=20480/10240) - grace 1500 ms
[WiFi] RECOVERED: Internal SRAM back above threshold after 6 ms
[WiFi] CRITICAL: Internal SRAM low for 1501 ms (free=13064 block=7156) - stopping WiFi
[SetupMode] Stopping WiFi: reason=low_dma manual=0 freeDma=13064 largestDma=7156
[SetupMode] WiFi OFF: reason=low_dma manual=0 radio=0 http=0 freeDma=42616 largestDma=18420 durMs=90
```

Three brief dips (recovering in 5-6 ms each), then a sustained drop from
19,392 → 13,064 bytes free (6,328 bytes consumed — matches span table size
for ~527 unique cells × 12 bytes/span).

### Root Cause Analysis

Pre-fix, three camera-module consumers of internal SRAM competed with WiFi's
20 KiB floor (AP+STA runtime threshold):

| Consumer | Location | SRAM Cost |
|----------|----------|-----------|
| **Span table (pre-fix)** | `buildSpans()` — `MALLOC_CAP_INTERNAL` | ~6–16 KiB (varies by unique cells) |
| **Chunk buffer fallback (pre-fix)** | `loadRecords()` — falls back to `MALLOC_CAP_8BIT` | 24 KiB if PSRAM alloc fails |
| **Loader task stack** | `xTaskCreatePinnedToCore` 8 KiB stack | 8 KiB (persistent while task exists) |

Sequence:
1. WiFi AP+STA is running, internal SRAM already near 20 KiB floor.
2. Span table allocated in internal SRAM → pushes free below threshold.
3. WiFi grace period (1,500 ms) expires → WiFi forcibly shut down.
4. After WiFi off, free jumps to 42,616 (radios released) — too late.

### Fix Applied (Feb 14, 2026)

**1. Moved span table to PSRAM** (`camera_data_loader.cpp` `buildSpans()`):
- Changed `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` → `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`
- Binary search over ~1K spans is O(log n); PSRAM latency is acceptable
- Renamed `kSpanSramBudgetBytes` → `kSpanBudgetBytes` in `camera_index.h`

**2. Removed chunk buffer internal SRAM fallback** (`camera_data_loader.cpp` `loadRecords()`):
- Previously: if PSRAM malloc failed, fell back to `MALLOC_CAP_8BIT` (internal SRAM, 24 KiB)
- Now: PSRAM-only; if PSRAM fails, operation aborts cleanly
- Rationale: 24 KiB surprise internal alloc would itself exceed WiFi headroom

**3. Raised memory guards to WiFi-aware levels**:
- `camera_data_loader.h`: `kMemoryGuardMinFreeInternal` 24 KiB → 32 KiB
- `camera_data_loader.h`: `kMemoryGuardMinLargestBlock` 11 KiB → 16 KiB
- `camera_runtime_module.h`: `kMemoryGuardMinFreeInternal` 21 KiB → 32 KiB
- `camera_runtime_module.h`: `kMemoryGuardMinLargestBlock` 11 KiB → 16 KiB
- Must always exceed WiFi AP+STA runtime threshold (20 KiB) + margin

### Updated SRAM Budget (Post-Fix Round 2)

After both fix rounds, camera module's internal SRAM footprint:

| Consumer | SRAM Cost | Duration | Notes |
|----------|-----------|----------|-------|
| Loader task stack | 8 KiB | **Transient** (load-time only) | Self-deletes after load completes |
| Span table | **0 KiB** | — | Moved to PSRAM |
| Chunk buffer | **0 KiB** | — | PSRAM-only, no fallback |
| **Total steady-state** | **0 KiB** | — | Task stack freed after load |
| **Total during load** | **8 KiB** | ~1-3 seconds | Task created on-demand, self-deletes |

WiFi AP+STA needs 20 KiB free internal. Camera module now has zero steady-
state SRAM footprint. During transient load (~1-3s), the 8 KiB task stack
is present but total free SRAM should remain above the WiFi floor.

### Lesson: SRAM Budget Coordination Contract

> **Rule**: Any module that allocates internal SRAM must account for WiFi's
> runtime floor. WiFi AP+STA requires ≥20 KiB free internal SRAM at all
> times. Camera memory guards must exceed this by ≥12 KiB margin (32 KiB).
>
> **Corollary**: Prefer PSRAM for all data structures that are not latency-
> critical at ISR/callback level. Span binary search is not ISR-level;
> PSRAM is fine.
>
> **Corollary 2**: FreeRTOS tasks that run briefly should self-delete to
> return their stack to the heap. Long-lived idle tasks permanently fragment
> internal SRAM.

### Fix Round 2: Heap Fragmentation (Feb 14, 2026)

**Symptom**: After Fix Round 1 (spans to PSRAM), WiFi still shut down. New
log showed `free=30948` (above 20 KiB threshold) but `block=8180` (below
10 KiB block threshold). Even after WiFi released its buffers:
`largestDma=8692` — meaning fragmentation is permanent.

**Root cause**: The camera loader FreeRTOS task stack (8 KiB) was allocated
from internal SRAM at boot and sat idle forever via `ulTaskNotifyTake(
portMAX_DELAY)`. This 8 KiB block in the middle of the heap permanently
split the largest contiguous block below WiFi's 10 KiB block threshold.

**Fix applied**:

1. **Self-deleting loader task** (`camera_data_loader.cpp`):
   - Task now processes all pending reloads in a `while` loop, then calls
     `vTaskDelete(nullptr)` to free its 8 KiB stack
   - `requestReload()` re-creates the task on-demand via `begin()`
   - The `begin()` guard (`if (loaderTask_) return`) makes re-creation safe
   - `loaderTask_ = nullptr` is set before `vTaskDelete()` so
     `requestReload()` can detect the task is gone

2. **Lowered WiFi block threshold** (`wifi_manager.h`):
   - `WIFI_RUNTIME_MIN_BLOCK_AP_STA` 10 KiB → 8 KiB (defense-in-depth)
   - WiFi TX/RX buffers are typically 1.6 KiB; 8 KiB contiguous is ample
   - Prevents false-positive shutdowns from transient fragmentation

### Remaining Risk: Transient Task Stack During Load

The 8 KiB loader task stack is allocated from internal SRAM by FreeRTOS
only during the load window (~1-3s). After load completes, the task self-
deletes and the stack is freed. If a reload is triggered during a period
of already-low internal SRAM, the 8 KiB allocation could temporarily push
the heap below WiFi thresholds. The 32 KiB memory guard should prevent
this, but worth monitoring.

Future optimization if needed: use `xTaskCreateStaticPinnedToCore` with a
PSRAM-backed stack buffer. Requires `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_
MEMORY=y` in sdkconfig.

### Verification Needed

1. Flash updated firmware and confirm WiFi stays alive through camera load.
2. Check `/api/cameras/status` for `loadSkipsMemoryGuard` counter — should
   remain 0 under normal operation.
3. Monitor `[WiFi] WARN` messages in serial — should not appear during
   camera load cycle.

---
## Web UI Camera Page Status (v0.7)

### Implemented

1. Route/UI:
   - `/cameras` page implemented at `interface/src/routes/cameras/+page.svelte`.
   - Nav links added in `interface/src/routes/+layout.svelte`.
2. Runtime observability:
   - Runtime/index/loader telemetry from `/api/cameras/status`.
   - Recent events table from `/api/cameras/events`.
3. Dataset catalog observability:
   - SD header scan endpoint `/api/cameras/catalog`.
   - UI shows ALPR/speed/redlight `present/valid/count/bytes`.
4. Camera runtime control:
   - Persistent `cameraEnabled` setting implemented.
   - Effective state is enforced as `gpsEnabled && cameraEnabled`.
   - UI toggle writes to `POST /api/settings` with `cameraEnabled`.

### Verified Behavior

1. Camera can be disabled independently without disabling GPS runtime.
2. If GPS is disabled, camera remains inactive even when `cameraEnabled=true`.
3. Settings backup/restore includes `cameraEnabled` and re-applies effective
   runtime state after restore.

### Remaining Open Items

1. `POST /api/cameras/reload` is still deferred.
2. `/api/cameras/catalog` currently performs SD header reads per request under
   SD mutex; this is acceptable for low-frequency UI polling but can be moved
   to cached metadata if request rates increase.
3. Docs sync remains: `docs/API.md`, `docs/DEVELOPER.md`, `docs/ARCHITECTURE.md`.

# Camera Alerts Plan (Draft)

> Status: Draft v0.2  
> Date: February 14, 2026  
> Scope: New camera alerts built on current GPS runtime and current main loop

## Goal

Add camera alerts without destabilizing the core detector pipeline.

## Implementation Status

- `M1` scaffold is now in code (no-op runtime hook + counters + camera module skeletons).
- No camera matching, filesystem load, display integration, or audio side effects are active yet.
- Runtime hook is low-priority and guard-railed by existing overload/non-core signals.

## Hard Constraints

1. Do not reuse `_disabled` code paths in runtime.
2. Use current GPS runtime only (`gpsRuntimeModule.snapshot(nowMs)`).
3. Core path always wins:
   `BLE/process + parser + display pipeline + mute commands`.
4. OBD, GPS enrichment, and camera work are lower priority than core.
5. Fail open: if camera work is skipped/fails, core behavior remains unchanged.

## Current Reality (Code-Accurate)

1. Active GPS runtime is in `src/modules/gps/gps_runtime_module.*`.
2. Active GPS ring log is in `src/modules/gps/gps_observation_log.*`.
3. Overload guards already exist in `src/main.cpp`:
   `skipNonCoreThisLoop` and `overloadThisLoop`.
4. Active GPS APIs exist in `src/wifi_manager.cpp`:
   `/api/gps/status` and `/api/gps/observations`.
5. There is currently no active camera runtime module in `src/`.
6. Camera binary data files exist in `camera_data/` and camera build tooling exists in `tools/`.

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
- `camera_event_log.h/.cpp`: bounded ring for diagnostics/API.

Boundary rule: main loop gets one call (`cameraRuntimeModule.process(...)`) and one lightweight snapshot accessor. No camera logic in core BLE/parser/display internals.

## Main Loop Integration Contract

Planned insertion point: low-priority section after parsed-frame display handling and behind overload guards.

Rules:

1. Never run camera tick when `skipNonCoreThisLoop` is true.
2. Skip camera tick when `overloadThisLoop` is true.
3. Camera tick frequency hard-limited (`<= 5Hz` initially).
4. No filesystem I/O in tick path.
5. No JSON parsing in tick path.
6. No blocking waits in tick path.

If any guard trips, increment camera skip counters and return immediately.

## GPS Contract for Camera Runtime

1. Read GPS state only from `GpsRuntimeStatus` snapshot.
2. Require `enabled && hasFix && locationValid` before matching.
3. Treat stale samples as ineligible for directional matching.
4. Do not read GPS UART/serial directly from camera code.

## Camera Data Contract

Runtime input format: binary `VCAM` records (header + fixed-size records).

Phase-1 loader behavior:

1. Support current binary naming used in repo artifacts:
   `alpr.bin`, `redlight_cam.bin`, `speed_cam.bin`.
2. Validate header/version/record size before accepting data.
3. Build immutable in-memory index once load succeeds.
4. On load failure, leave camera runtime disabled and non-fatal.

## Matching Strategy

1. Convert coordinates to fixed-point (`E5`) for index keys.
2. Use coarse buckets/grid to narrow candidates.
3. Enforce hard candidate cap per tick.
4. Run heading corridor checks only for narrowed candidates and valid heading context.
5. Add per-camera cooldown to avoid repeated retrigger noise.

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

### Phase 2 (Controlled Write)

- `POST /api/cameras/reload` (deferred/best-effort reload)

No test/sync/upload endpoints until runtime stability is verified.

## UI Rollout (Phased)

1. Add a camera diagnostics/status card in existing integrations style.
2. Add recent camera event table from bounded event log.
3. Add enable/reload controls only after profile data confirms low impact.

No high-frequency polling changes until camera runtime impact is measured.

## Observability

Add counters:

- `cameraTicks`
- `cameraTickSkipsOverload`
- `cameraTickSkipsNonCore`
- `cameraCandidatesChecked`
- `cameraMatches`
- `cameraAlertsStarted`
- `cameraBudgetExceeded`
- `cameraLoadFailures`

Expose via status API and perf snapshots.

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

1. `M1`: scaffolding + no-op hook + counters.
2. `M2`: binary load + immutable index + read-only API.
3. `M3`: silent (log-only) trigger mode.
4. `M4`: controlled audio/display integration.
5. `M5`: optional controlled reload endpoint.

Advancement requires hardware perf check at each milestone.

## Exit Criteria

1. Core loop behavior unchanged under normal and stress runs.
2. No measurable BLE/display reliability regression.
3. Camera subsystem degrades gracefully under resource pressure.
4. Docs/API only describe actually shipped endpoints and modules.

---

## Review Findings (February 14, 2026)

> Status: Open for resolution before M2 begins.
> Methodology: Full read of all `src/modules/camera/*`, `src/modules/gps/*`,
> `src/modules/lockout/*`, `src/main.cpp` loop integration, `platformio.ini`,
> binary data files, and `tools/enrich_osm/convert_to_binary.py`.

### Data Reality (Measured)

| File | Records | Raw Size |
|------|---------|----------|
| alpr.bin | 70,327 | 1,648 KB |
| speed_cam.bin | 1,125 | 26 KB |
| redlight_cam.bin | 205 | 5 KB |
| **Total** | **71,657** | **1,679 KB** |

Record format: 16-byte VCAM header + 24-byte records
(4 floats + int16 bearing + 6 uint8 fields).

Free internal SRAM after BLE+WiFi+display: ~250–320 KB typical.
PSRAM available: 8 MB (QIO-OPI, enabled in platformio.ini).

### F1 — CRITICAL: Plan Omits PSRAM — Will OOM Internal Heap

The plan never mentions PSRAM. The word does not appear. Raw camera data
(1,679 KB) is 5–7× the entire free internal heap. Any index overhead
(hash map buckets, sorted arrays, pointers) adds further. A default
`malloc()` on ESP32-S3 tries internal SRAM first; at 1.7 MB this is an
instant OOM crash taking down BLE, display, everything.

The board has 8 MB PSRAM (`BOARD_HAS_PSRAM`, `qio_opi` memory type) but
the codebase barely uses it — only OpenFontRender's font cache calls
`ps_malloc`. No camera code allocates from PSRAM.

**Required resolution (pick one):**

1. **PSRAM-only allocation** — all camera data via
   `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`. Simplest path.
2. **SD streaming** — never load full dataset; page on demand.
3. **Regional subset loading** — load only cameras within X km from SD.

If option 1: mandate pre-allocation check
(`heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= required + 1 MB`)
and never fall back to internal SRAM.

### F2 — HIGH: No Spatial Index Design — 70K Linear Scan = ~17 ms/tick

The plan says "coarse buckets/grid" but provides no specifics. The lockout
index (`lockout_index.h`) does O(N) linear scan over 200 entries in ~50 µs
(~0.25 µs/entry). At 70K entries: **~17.5 ms per tick**. At 5 Hz that
consumes 87 ms/s of loop budget, violating the 500 ms `loopMax_us` SLO
from a single subsystem.

**Unspecified details that matter:**

- Bucket/grid resolution (plan mentions E5 keys but no grid size).
- Data structure (`std::unordered_map` carries ~64 B/node on ESP32 newlib).
- Dense-area behavior (downtown Manhattan may have 200+ ALPR in one 1 km²
  cell — the "hard candidate cap" silently drops real matches).

**Recommended approach:** Flat sorted array in PSRAM, sorted by grid cell
key at load time. Binary search to find cell boundaries. Grid resolution
~0.01° (~1.1 km). Keep the grid lookup table (~few KB) in internal SRAM;
only the record array in PSRAM. This gives zero per-node overhead and
cache-friendly sequential access.

### F3 — HIGH: No PSRAM Access Latency in Performance Budget

PSRAM random access is 3–10× slower than internal SRAM due to cache line
misses. The plan's 250 µs target was presumably modeled on SRAM speed. A
grid lookup touching 20–50 scattered PSRAM records generates cache misses
at ~200 ns each — acceptable for small candidate sets (~10 µs), but the
budget must account for this.

**Recommendation:** Keep grid/bucket index metadata in internal SRAM.
Only the camera record array goes to PSRAM. This confines cache misses to
the final candidate check.

### F4 — MEDIUM: Load Path Will Block loop() — No Background Task Plan

Loading 1.7 MB from SD at typical SPI speeds (2–4 MB/s) takes 400–800 ms
of blocking I/O. The plan says "no filesystem I/O in tick path" (correct)
but does not specify where/when the load happens. If `loadDefault()` runs
in `loop()`, even behind overload guards, the result is:

- BLE disconnect (watchdog or scan timeout).
- `loopMax_us` blown past 500 ms SLO.
- OBD state machine interrupted mid-transition.

The previous `camera_manager.cpp` (now removed) used a FreeRTOS background
task. The current plan mentions no threading.

**Required resolution:**

- Load in a FreeRTOS task at priority 1 (below BLE/display).
- Yield every ~1000 records (24 KB) with 1 ms `vTaskDelay`.
- Signal completion via `std::atomic<bool> loadComplete_`.
- Index swap: build in secondary buffer, atomic pointer swap to main loop.

### F5 — MEDIUM: GPS Heading Not in Runtime Snapshot

The plan says "heading corridor checks" but `GpsRuntimeStatus` has no
heading/bearing field. GPS course-over-ground comes from RMC sentences
(already parsed by `gps_runtime_module.cpp`) but is not exposed in the
snapshot struct. At speeds below ~5 mph, GPS heading is random noise.

**Required resolution:**

- Add `float courseDeg` to `GpsRuntimeStatus`.
- Parse COG from RMC (field 8) — the NMEA parser already processes RMC.
- Skip heading corridor checks below 5 mph; use distance-only matching.

### F6 — MEDIUM: "Stale" and Speed Thresholds Undefined

The plan says "treat stale samples as ineligible" without defining stale.
`GpsRuntimeModule` uses `FIX_STALE_MS = 15000` (15 s). At 60 mph that is
0.25 miles of drift — matching cameras a quarter mile behind the vehicle.

**Required resolution:** Define concrete thresholds in the plan:

- `sampleAgeMs > 2000` → skip matching entirely.
- `speedMph < 3` → suppress matching (parked next to camera = infinite
  retrigger).
- `sampleAgeMs > 1000` → distance-only matching (no heading corridor).

### F7 — MEDIUM: Per-Camera Cooldown Memory Is Unspecified

The plan says "per-camera cooldown" without sizing. At 70K cameras, even
4 bytes per camera is 280 KB of cooldown state — doubling PSRAM usage.

**Recommended approach:** Use `CameraEventLog` (already a 64-entry ring)
as the cooldown table. On match, check if camera ID exists in the ring
with `(nowMs - event.tsMs) < cooldownMs`. 64 entries × 12 bytes = 768
bytes. If the user drives past 65+ distinct cameras before the cooldown
expires, the oldest entry overflows — acceptable because they are in a new
area by then.

### F8 — MEDIUM: ALPR (70K) Mixed with Enforcement (1.3K)

70,327 of 71,657 records are ALPR (license plate readers). ALPR cameras
do not trigger radar/laser — they are informational/privacy-awareness
only. Speed + red-light cameras (1,330 total, 31 KB) are enforcement and
directly affect the driver. The plan treats all camera types identically.

**Recommended approach:**

- Load speed_cam + redlight_cam first (31 KB — fits in internal SRAM
  with no PSRAM dependency).
- ALPR as optional separate index, PSRAM-only, loaded lazily.
- Settings toggle: "Enable ALPR alerts" (default off).
- This lets M2/M3 ship enforcement cameras with zero PSRAM risk, and adds
  ALPR as opt-in upgrade.

### F9 — LOW: ARCHITECTURE.md File Name Mismatch

`docs/ARCHITECTURE.md` references `camera_load_coordinator_module` and
`camera_alert_module`. Actual files are `camera_data_loader`,
`camera_index`, `camera_runtime_module`, `camera_event_log`. Will confuse
future readers.

### F10 — LOW: Event Log Thread Safety Unstated

`CameraEventLog` uses `uint8_t head_/tail_/count_` with no mutex or
atomic. If a future background loader or API handler calls `copyRecent()`
while `process()` calls `publish()`, reads will tear. The plan should
state explicitly: event log is main-loop-only, same as lockout index.

### F11 — LOW: OOM Recovery Path Not Defined

The plan says "camera disabled/failure path leaves behavior unchanged" but
does not define OOM recovery. If PSRAM allocation fails (fragmentation,
wrong board variant, silicon defect), the loader must:

1. Check `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` before
   allocating.
2. If insufficient, log failure, increment `cameraLoadFailures`, leave
   module disabled.
3. Never fall back to internal SRAM.

### Findings Summary

| # | Severity | Issue | Resolution |
|---|----------|-------|------------|
| F1 | **CRITICAL** | Plan omits PSRAM — 1.7 MB will OOM internal heap | Mandate PSRAM-only allocation |
| F2 | **HIGH** | No index design — 70K linear scan = 17 ms/tick | Flat sorted array + binary search |
| F3 | **HIGH** | No PSRAM latency in perf budget | Keep index in SRAM, data in PSRAM |
| F4 | **MEDIUM** | Load blocks loop — no background task | FreeRTOS task + atomic swap |
| F5 | **MEDIUM** | GPS heading missing from snapshot | Add courseDeg to GpsRuntimeStatus |
| F6 | **MEDIUM** | Stale/speed thresholds undefined | Define concrete ms/mph numbers |
| F7 | **MEDIUM** | Per-camera cooldown memory unsized | Use existing event ring as LRU |
| F8 | **MEDIUM** | ALPR (70K) mixed with enforcement (1.3K) | Split load; enforcement first, ALPR opt-in |
| F9 | LOW | Architecture doc file names wrong | Update ARCHITECTURE.md |
| F10 | LOW | Event log thread safety unstated | Document main-loop-only rule |
| F11 | LOW | OOM recovery undefined | Pre-allocation heap check |

### What Is Solid

- Overload guard integration (`skipNonCoreThisLoop`, `overloadThisLoop`).
- Counter observability (all the right things are counted).
- Milestone gating (hardware perf check required at each stage).
- Main-loop single-call boundary (`cameraRuntimeModule.process(...)`).
- Event ring design (bounded, O(1), no heap).
- M1 scaffold is clean and correctly no-ops.

### Blocking Items for M2

F1, F2, F3, and F4 must be resolved before M2 implementation begins.
F5 and F6 must be resolved before M3 (silent trigger mode).
F7 and F8 should be resolved before M3 for practical usability.

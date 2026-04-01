# GPS System Removal Plan

**Status as of April 1, 2026**

This document tracks the complete removal of GPS from the firmware — lockout system
(completed), GPS hardware module, GPS-based speed source, GPS time injection, GPS API
endpoints, GPS display indicators, GPS web UI, and all associated settings/tests/docs.

## Design Decisions

These decisions flow from CLAUDE.md guidance and project owner direction:

| Question | Decision | Rationale |
|----------|----------|-----------|
| Speed-based volume muting without GPS? | **OBD-only speed mute.** SpeedMuteModule stays, SpeedSourceSelector drops the GPS pointer. Speed mute only works when OBD is connected. | GPS was a fallback speed source. OBD is the preferred source. Without GPS hardware, there is no fallback — OBD or nothing. This is consistent with Mike Valentine's SAVVY pattern: SAVVY uses OBD speed. |
| TimeService without GPS? | **Keep TimeService.** Local clock updated via WiFi (`wifi_time_api_service` POST /api/time/sync, SOURCE_CLIENT_AP) and future NTP (SOURCE_SNTP_STA). Holds time without WiFi via monotonic offset. | GPS was one of four TimeService sources. The recently added `wifi_time_api_service` covers browser-clock sync. TimeService already holds time via monotonic offset after any source sets it once. |
| GPS API endpoints? | **Remove all three** (`/api/gps/status`, `/api/gps/observations`, `/api/gps/config`). | No GPS hardware → no GPS data to serve. Endpoints become meaningless. |
| GPS display indicator? | **Remove `drawGpsIndicator()`**, the "G{satCount}" badge, `colorGps` setting. | No GPS hardware → no satellite count to display. Frees 50×26 pixels. |
| GPS web UI? | **Remove all GPS sections** from Integrations page, Colors page GPS badge picker, Dashboard GPS pill. | No GPS API → no GPS UI. |
| `speedMuteRequireObd` setting? | **Remove.** Hard-code OBD-only behavior. | Without GPS, the setting's only purpose (choose GPS vs OBD) is moot. OBD is the only speed source. |
| `gpsEnabled` setting? | **Remove.** | No GPS hardware to enable/disable. |

---

## Completed Work (Phases 1–8 + Test Cleanup)

All production source code has been cleaned. Zero lockout references remain in `src/` or `include/`. Native tests pass (1208 cases, 0 failures). Device firmware builds clean (waveshare-349).

### What was removed from production

- `gps_lockout_safety.h` / `gps_lockout_safety.cpp` — deleted entirely
- `LockoutLearner` class and all lockout enforcement logic
- `LockoutRuntimeMode` enum, all `LOCKOUT_*` constants
- All `gpsLockout*` fields from `V1Settings`
- `GpsSettingsUpdate` lockout fields
- `normalizeLegacyLockoutRadiusScale` from `main_boot.cpp`
- `enableSignalTraceLogging` from `V1Settings`, `LoopSettingsPrepValues`, `LoopDisplayContext`, `DisplayOrchestrationParsedContext`
- `DisplayDirtyFlags::lockout` field
- `QuietOwner::LockoutMute`, `QuietOwner::LockoutOverride`, `QuietOwner::PreQuiet`
- `drawLockoutIndicator` / `ut_drawLockoutIndicator` from display indicators
- `setLockoutMuted`, `setLockoutIndicator`, `setPreQuietActive` from `display.h`
- `recordLockoutUs` from `LoopDisplayModule::Providers`
- `runLockoutLearner`, `runLockoutStoreSave`, `runLearnerPendingSave`, `nowEpochMsOr0` from `PeriodicMaintenanceModule::Providers` (replaced by `runStoreSave`)
- `lockoutPrioritySuppressed` bool from `runDisplayPipeline`
- `LockoutStatusSnapshot` from `StatusObservabilityPayload::appendStatusObservability`
- `lockoutLearner`, `perfCounters`, `eventBus` params from `GpsApiService::handleApiStatus` and `handleApiConfig`
- Lockout settings from `handleApiConfigGet` response (now just `success` + `enabled`)
- `handleApiConfig` error message changed from "Missing enabled or lockout settings" to "Missing enabled"
- `DisplayOrchestrationParsedResult` reduced from 3 fields to 1 (`runDisplayPipeline`)
- `DisplayPipelineModule::handleParsed` reduced from 2 params to 1 (removed bool)
- `VolumeZeroWarning::evaluate` removed lockoutActive bool parameter
- Perf CSV schema version bumped 22 → 23

### Test files fully cleaned

- `test_gps_api_service.cpp` — removed LockoutLearner mock, lockout band stubs, all lockout call args/assertions
- `test_display_orchestration_module.cpp` — removed 11 lockout/prequiet tests
- `test_display_rendering_indicators.cpp` — removed 5 lockout indicator tests
- `test_status_observability_payload.cpp` — removed LockoutStatusSnapshot
- `test_periodic_maintenance_module.cpp` — replaced lockout providers with `runStoreSave`
- `test_main_boot.cpp` — removed 10 `normalizeLegacyLockoutRadiusScale` tests
- `test_loop_display_module.cpp` — removed lockout perf, fixed struct initializers
- `test_main_loop_phases.cpp` — removed lockout calls, fixed struct initializers
- `test_loop_settings_prep_module.cpp` — removed `enableSignalTraceLogging` assertions
- `test_display_dirty_flags.cpp` — removed `f.lockout` assertions
- `test_wifi_status_api_service.cpp` — removed lockout JSON from status test
- `test_volume_fade.cpp` — renamed `inLockout` param to `suppressed`
- `test_device_nvs.cpp` — updated comment
- `test_display_pipeline_module.cpp` — removed second bool arg from `handleParsed`
- `test_display_vol_warn.cpp` — removed lockoutActive bool from `evaluate` calls
- `test_perf_csv_schema_contract.cpp` — updated schema version 22 → 23
- `test_wifi_display_colors_api_service.cpp` — removed `enableSignalTraceLogging` references
- `test/mocks/display.h` — removed lockout mock methods and tracking fields

---

## Remaining Cleanup (Phases 9–12)

The items below are stale references that don't affect compilation or runtime but should be cleaned for completeness.

---

### Phase 9: Test Fixture Data Files

These fixture files contain lockout data from old perf captures and replay tests.

| File | What to fix | Priority |
|------|------------|----------|
| `test/fixtures/perf/perf_boot_6_reduced.csv` | CSV header has `lockoutMax_us`, `lockoutSaveMax_us` columns. Remove columns from header and all data rows. | Low |
| `test/fixtures/perf/perf_boot_6_connect_burst_reduced.csv` | Same — `lockoutMax_us`, `lockoutSaveMax_us` columns. Remove. | Low |
| `test/fixtures/perf/core_soak_connect_burst_reduced.metrics.jsonl` | JSON lines contain `"lockout":{"coreGuardEnabled":true,...}` objects. Remove the `lockout` key from each JSON line. | Low |
| `test/fixtures/replay/corrupt_packet_rejection/expected.json` | Contains `"lockout_transitions": []`. Remove this key. | Low |

**Verification:** After editing, run `pio test -e native -f test_perf_csv_schema_contract` and `pio test -e native -f test_debug_perf_files_service` to confirm schema contract tests still pass. The CSV fixtures are read by tests that validate column positions — removing columns may require updating those tests too.

**Caution:** The perf CSV fixtures are used by `test_debug_perf_files_service` and potentially `test_perf_sd_logger`. Read those test files first to understand which columns they validate before modifying the CSVs.

---

### Phase 10: Interface (Web UI) Lockout Test Stubs

Two JS test files contain lockout properties in mock API responses.

| File | What to fix | Priority |
|------|------------|----------|
| `interface/src/routes/profiles/page.test.js` (line 15) | Remove `lockoutsEnabled: true` from mock settings response | Low |
| `interface/src/routes/dev/page.test.js` (line 33) | Remove `enableSignalTraceLogging: false` from mock settings response | Low |

**Verification:** Run `npm test` or `vitest` in the `interface/` directory after edits.

---

### Phase 11: Documentation Updates

These docs reference the lockout system. They should be updated to reflect its removal.

| File | Lockout refs | Action |
|------|-------------|--------|
| `docs/RUNTIME_OWNERSHIP.md` | 10 refs | Major rewrite — remove lockout ownership sections, update module map |
| `docs/REPO_REVIEW.md` | 8 refs | Remove lockout test descriptions, update module wiring references |
| `docs/TEST_BASELINE.md` | 4 refs | Remove references to `test_lockout_enforcer`, `lockout_area_safety`, lockout modules |
| `docs/ARCHITECTURE.md` | 2 refs | Remove lockout from architecture descriptions |
| `docs/FUNCTION_PRIORITY.local.md` | 3 refs | Remove lockout from function priority listings |
| `docs/MANUAL.md` | 1 ref | Remove lockout mention from user manual |
| `docs/OBD_MODULE_PLAN.md` | 1 ref | Remove lockout reference from OBD plan |
| `docs/ROAD_MAP_FORMAT.md` | 1 ref | Remove lockout from roadmap |
| `docs/API.md` | Check | Verify lockout API endpoints are removed from API docs |
| `test/README.md` (lines 88, 108) | 2 refs | Remove `test_lockout_enforcer` from test gate list, remove `--owner lockout` example |

**Priority:** Medium — documentation drift creates confusion for future contributors.

---

### Phase 12: Build Artifacts Cleanup

These are generated/historical files that can be cleaned up or ignored.

| Location | What | Action |
|----------|------|--------|
| `.artifacts/test_reports/mutation_*/workspace/test/mocks/settings.h` | Old test report snapshots contain full lockout enum/constants/fields | Delete stale mutation test report directories, or leave as historical |
| `.artifacts/dead_code_review_20260223.md` | References lockout settings key duplication | Leave as historical analysis document |
| `.artifacts/manual_soak_trace_*.stderr` | Contains `"lockout":{"coreGuardTripped":false}` in JSON payloads | Leave as historical trace data |
| `.artifacts/perf_boot_*_live.csv` | Perf CSV snapshots with lockout columns | Leave as historical data |
| `platformio.ini` (line 107-109) | Comment: `; Native replay environment — disabled (lockout replay suite removed)` | Remove commented-out `[env:native-replay]` block |

**Priority:** Low — artifacts don't affect builds or tests.

---

## Lockout Verification Checklist

After lockout phases (9–12) are complete:

```bash
pio test -e native
pio run -e waveshare-349
grep -rni "lockout\|LOCKOUT\|PreQuiet\|preQuiet\|enableSignalTrace\|LockoutMute\|LockoutOverride\|gps_lockout_safety\|coreGuard\|CoreGuard" \
  src/ include/ test/ --include="*.cpp" --include="*.h" --include="*.js" --include="*.svelte"
find src/ include/ -name "*lockout*" -type f
cd interface && npm test
```

---

---

# GPS Hardware & Module Removal

Everything below removes the GPS hardware module, GPS speed source, GPS time injection,
GPS API endpoints, GPS display indicator, GPS web UI, and all associated settings/tests.

**Estimated removal:** ~2,200 lines (1,630 production C++ + ~300 test + ~300 Svelte UI)

---

## Current GPS Footprint (What We're Removing)

### Production Source Files (src/modules/gps/)

| File | Lines | Purpose |
|------|-------|---------|
| `gps_runtime_module.h` | ~200 | GPS hardware module header — UART/NMEA, fix state, speed, scaffold samples |
| `gps_runtime_module.cpp` | ~800 | UART ingestion, NMEA parser (RMC, GGA), fix/speed/location tracking, time update to TimeService |
| `gps_observation_log.h` | ~80 | Ring buffer container for GPS observations (64 entries, thread-safe) |
| `gps_observation_log.cpp` | ~60 | Ring buffer publish/consume with drop tracking |
| `gps_runtime_status.h` | ~50 | Data-only snapshot struct (hasFix, speed, satellites, etc.) |
| `gps_api_service.h` | ~40 | REST API handler declarations |
| `gps_api_service.cpp` | ~250 | `/api/gps/status`, `/api/gps/observations` handlers |
| `gps_api_config_service.cpp` | ~150 | `/api/gps/config` GET/POST handlers, scaffold sample injection |

### Settings (src/settings.h — V1Settings struct)

| Field | Type | Default | Removal Action |
|-------|------|---------|----------------|
| `gpsEnabled` | bool | false | Delete field, NVS key, setter |
| `colorGps` | uint16_t | 0x07FF | Delete field, NVS key, color API handler |
| `speedMuteRequireObd` | bool | false | Delete field — OBD-only is now implicit |

*(SpeedMute settings `speedMuteEnabled`, `speedMuteThresholdMph`, `speedMuteHysteresisMph`, `speedMuteVolume` stay — they govern OBD-based speed muting.)*

### API Endpoints (src/wifi_routes.cpp)

| Method | Route | Action |
|--------|-------|--------|
| GET | `/api/gps/status` | Remove handler + route |
| GET | `/api/gps/observations` | Remove handler + route |
| GET | `/api/gps/config` | Remove handler + route |
| POST | `/api/gps/config` | Remove handler + route |

### Web UI (interface/src/)

| Location | GPS Content | Action |
|----------|------------|--------|
| `routes/integrations/+page.svelte` | GPS runtime card (enable toggle, fix status, satellites, mode) | Remove GPS card section |
| `routes/colors/+page.svelte` | GPS "G" badge color picker (`colorGps`) | Remove GPS color section |
| `routes/+page.svelte` | GPS status pill on dashboard | Remove GPS pill |
| `lib/stores/runtimeStatus.svelte.js` | `runtimeGpsStatus`, `runtimeGpsError`, `fetchRuntimeGpsStatus()`, GPS poll logic | Remove all GPS store exports and poll logic |

### Display (include/ + src/)

| Item | File | Action |
|------|------|--------|
| `drawGpsIndicator()` | `display_indicators.cpp` | Delete function |
| `gpsSatEnabled_`, `gpsSatCount_`, `gpsSatHasFix_` | `display.h` / `display_update.cpp` | Delete state fields + setters |
| `DisplayDirtyFlags::gpsIndicator` | `display_dirty_flags.h` | Delete flag |
| All `drawGpsIndicator()` call sites | `display_update.cpp`, `display_screens.cpp` | Remove calls |

### Tests

| Directory | Lines | Tests |
|-----------|-------|-------|
| `test/test_gps_runtime/` | ~400 | NMEA parser, fix state, scaffold injection |
| `test/test_gps_observation_log/` | ~150 | Ring buffer publish/consume, overflow |
| `test/test_gps_api_service/` | ~400 | API handlers, JSON responses, rate limiting |
| `test/test_obd_speed_source/` | ~350 | Speed source arbitration (GPS vs OBD) — **must be refactored, not deleted** |

### Main Wiring (src/main.cpp)

| Line Area | What | Action |
|-----------|------|--------|
| Includes | `#include "modules/gps/gps_runtime_module.h"`, `gps_observation_log.h` | Remove |
| Globals | `extern GpsRuntimeModule gpsRuntimeModule;`, `extern GpsObservationLog gpsObservationLog;` | Remove |
| Boot | `gpsRuntimeModule.begin(gpsEnabled, &gpsObservationLog)` | Remove |
| Speed wiring | `speedSourceSelector.wireSpeedSources(&gpsRuntimeModule, &obdRuntimeModule)` | Change to `wireSpeedSources(nullptr, &obdRuntimeModule)` or refactor GPS out of selector |
| Loop ingest | `loopIngestProviders.gpsRuntimeContext = &gpsRuntimeModule;` | Remove |
| Loop update | `gpsRuntimeModule.update(nowMs)` | Remove |
| Display pipeline | `gpsRuntimeModule` reference in display orchestration | Remove |
| Speed mute | `speedMuteRequireObd` check | Remove check — OBD-only is implicit |

### Hardware

| Component | Detail | Action |
|-----------|--------|--------|
| Serial2 UART | RX=pin3, TX=pin1, 9600 baud | Release pins, remove Serial2 init |
| GPIO2 (EN) | GPS module enable pin | Release pin |

---

## GPS Removal Phases

### Phase 13: GPS Hardware Module Deletion

**What:** Delete the entire `src/modules/gps/` directory and all GPS test directories.

**Files to delete:**
- `src/modules/gps/gps_runtime_module.h`
- `src/modules/gps/gps_runtime_module.cpp`
- `src/modules/gps/gps_observation_log.h`
- `src/modules/gps/gps_observation_log.cpp`
- `src/modules/gps/gps_runtime_status.h`
- `src/modules/gps/gps_api_service.h`
- `src/modules/gps/gps_api_service.cpp`
- `src/modules/gps/gps_api_config_service.cpp`
- `test/test_gps_runtime/` (entire directory)
- `test/test_gps_observation_log/` (entire directory)
- `test/test_gps_api_service/` (entire directory)

**Verification:** `find src/ -name "*gps*" -type f` returns nothing.

---

### Phase 14: SpeedSourceSelector — Remove GPS Path

**What:** Remove GPS as a speed source. SpeedSourceSelector becomes OBD-only.

**Changes:**
1. **`speed_source_selector.h`** — Remove `gps_` pointer, `gpsEnabled_` flag, `gpsSelections` counter. Remove GPS from `wireSpeedSources()` signature (take only OBD pointer).
2. **`speed_source_selector.cpp`** — Remove all GPS-fallback logic in `buildStatus()` / `update()`. OBD is the only source. If OBD unavailable, `selectedSpeed_.valid = false`.
3. **`SpeedSource` enum** — Remove `GPS` entry. Keep `OBD` and `None`.

**Downstream:**
- `main.cpp` — Change `wireSpeedSources(&gpsRuntimeModule, &obdRuntimeModule)` → `wireSpeedSources(&obdRuntimeModule)` (single arg).
- Speed mute `speedMuteRequireObd` check in main.cpp becomes dead code — remove it.

**Test impact:**
- `test/test_obd_speed_source/` — **Refactor** to remove all GPS scenarios (GPS-preferred, GPS-fallback, GPS-stale tests). Keep OBD arbitration tests. Rename if appropriate.

**Verification:** `pio test -e native -f test_obd_speed_source` passes with GPS scenarios removed.

---

### Phase 15: TimeService — Remove GPS Source

**What:** Remove `SOURCE_GPS` from TimeService. Time comes from WiFi sync (`SOURCE_CLIENT_AP` via `wifi_time_api_service`) and future NTP (`SOURCE_SNTP_STA`). Local clock holds time via monotonic offset once any source sets it.

**Changes:**
1. **`time_service.h`** — Remove `SOURCE_GPS` enum entry. Keep `SOURCE_CLIENT_AP`, `SOURCE_SNTP_STA`, `SOURCE_RTC`.
2. **`time_service.cpp`** — Remove any GPS-specific handling if present.
3. **GpsRuntimeModule** was the only caller of `timeService.setEpochBaseMs(..., SOURCE_GPS)` — already deleted in Phase 13.

**Verification:** TimeService tests pass. `wifi_time_api_service` tests confirm browser-clock sync still works.

---

### Phase 16: Settings Cleanup

**What:** Remove GPS-related settings fields and NVS keys.

**Changes:**
1. **`settings.h` (V1Settings):**
   - Delete `gpsEnabled` field
   - Delete `colorGps` field
   - Delete `speedMuteRequireObd` field
2. **`settings.h` (GpsSettingsUpdate):**
   - Delete entire struct (or reduce to speed-mute-only if it carried speed mute fields)
3. **`settings.h` (GpsSettingsApplyResult):**
   - Delete entire struct
4. **`settings.cpp` (SettingsManager):**
   - Delete `setGpsEnabled()` method
   - Delete `applyGpsSettingsUpdate()` method
   - Remove GPS NVS namespace keys
5. **NVS migration:** Ensure remove of stale NVS keys on boot (or accept orphaned keys — they're harmless).

**Test impact:** Update all test files that construct `V1Settings` or mock GPS settings fields.

**Verification:** `pio test -e native` passes. No reference to `gpsEnabled`, `colorGps`, or `speedMuteRequireObd` in src/include/test.

---

### Phase 17: Display — Remove GPS Indicator

**What:** Remove the "G{satCount}" satellite badge and all display GPS state.

**Changes:**
1. **`display_indicators.cpp`** — Delete `drawGpsIndicator()` function entirely.
2. **`display.h`** — Delete `gpsSatEnabled_`, `gpsSatCount_`, `gpsSatHasFix_` state fields and their setters (`setGpsIndicator()` or equivalent).
3. **`display_dirty_flags.h`** — Delete `DisplayDirtyFlags::gpsIndicator` flag.
4. **`display_update.cpp`** — Remove all `drawGpsIndicator()` call sites.
5. **`display_screens.cpp`** — Remove `drawGpsIndicator()` call site.
6. **Display orchestration** — Remove `gpsRuntimeModule` reference from display pipeline wiring.

**Test impact:** Remove GPS indicator tests from `test_display_rendering_indicators`, update display dirty flags tests.

**Verification:** `pio test -e native -f test_display` passes. No GPS references in display source.

---

### Phase 18: API Routes — Remove GPS Endpoints

**What:** Remove all `/api/gps/*` route registrations and handler wiring.

**Changes:**
1. **`wifi_routes.cpp`** — Remove route registrations for `/api/gps/status`, `/api/gps/observations`, `/api/gps/config`.
2. **Remove GPS includes** from wifi_routes.cpp.
3. **Remove GPS handler parameters** from any route registration functions.

**Test impact:** Update any WiFi route tests that reference GPS endpoints.

**Verification:** `pio test -e native` passes. `grep -rn "api/gps" src/` returns nothing.

---

### Phase 19: Main Wiring Cleanup

**What:** Remove all GPS references from main.cpp and boot sequence.

**Changes:**
1. **Includes** — Remove `gps_runtime_module.h`, `gps_observation_log.h`.
2. **Globals** — Remove `extern GpsRuntimeModule`, `extern GpsObservationLog`.
3. **Boot** — Remove `gpsRuntimeModule.begin()` call.
4. **Loop** — Remove `gpsRuntimeModule.update(nowMs)` call.
5. **Ingest providers** — Remove `gpsRuntimeContext` field and assignment.
6. **Speed mute** — Remove `speedMuteRequireObd` check (line ~1093). Speed validity = `speed.valid` only.
7. **Hardware** — Remove Serial2 UART init, GPIO2 EN pin setup.

**Verification:** `pio run -e waveshare-349` builds clean. `grep -rn "gpsRuntime\|gpsObservation\|GpsRuntime\|GpsObservation" src/ include/` returns nothing.

---

### Phase 20: Web UI — Remove GPS Sections

**What:** Remove all GPS content from the Svelte frontend.

**Changes:**
1. **`routes/integrations/+page.svelte`** — Remove GPS runtime card (enable toggle, fix status, satellites, mode display).
2. **`routes/colors/+page.svelte`** — Remove GPS "G" badge color picker section.
3. **`routes/+page.svelte`** — Remove GPS status pill from dashboard.
4. **`lib/stores/runtimeStatus.svelte.js`** — Remove `runtimeGpsStatus`, `runtimeGpsError`, `fetchRuntimeGpsStatus()`, GPS poll logic exports.
5. **Test stubs** — Remove GPS mock data from `interface/src/routes/*/page.test.js` files.

**Verification:** `cd interface && npm run build && npm test` passes. No GPS references in interface/src/.

---

### Phase 21: CI Infrastructure Cleanup

**What:** Update contract snapshots, stabilization manifest, mutation targets, and CI scripts.

**Changes:**
1. **`test/contracts/extern_usage_contract.txt`** — Run `python3 scripts/check_extern_usage_contract.py --update` to remove GPS extern entries.
2. **`test/contracts/perf_csv_column_contract.txt`** — Run `python3 scripts/check_perf_csv_column_contract.py --update` to remove GPS perf columns (e.g., `gpsObsDrops`).
3. **`config/stabilization_manifest.json`** — Remove GPS test entries and any GPS-specific stabilization paths.
4. **`test/mutations/critical_mutations.json`** — Remove any mutations targeting deleted GPS source files.
5. **Perf CSV schema version** — Bump version (23 → 24) if GPS columns are removed from perf output.
6. **docs/API.md** — Remove `/api/gps/*` endpoint documentation.
7. **docs/MANUAL.md** — Remove GPS module references.
8. **docs/RUNTIME_OWNERSHIP.md** — Remove GPS module ownership entries.
9. **docs/OBSERVABILITY.md** — Remove GPS metrics references.
10. **docs/ARCHITECTURE.md** — Remove GPS from architecture descriptions.

**Verification:** `./scripts/ci-test.sh` passes all gates.

---

## What Stays After GPS Removal

| Module | Status | Notes |
|--------|--------|-------|
| **SpeedMuteModule** | ✅ Stays | Pure decision function. Takes `speedMph` + `speedValid`. GPS-agnostic. OBD-only speed source. |
| **SpeedSourceSelector** | ✅ Stays (simplified) | OBD-only. No GPS fallback path. Single speed source. |
| **TimeService** | ✅ Stays | WiFi browser-sync (`wifi_time_api_service`), future NTP, RTC. No GPS time source. |
| **wifi_time_api_service** | ✅ Stays | POST /api/time/sync — browser clock → device epoch. Replaces GPS time. |
| **QuietCoordinator** | ✅ Stays | `SpeedVolume` quiet owner unchanged. No GPS coupling. |
| **OBD module** | ✅ Stays | Sole speed source for speed mute. |

## What's Gone After GPS Removal

| Component | Lines Removed | Notes |
|-----------|--------------|-------|
| `src/modules/gps/` (8 files) | ~1,630 | Hardware module, observation log, API service |
| GPS test directories (3) | ~950 | Runtime, observation, API tests |
| GPS settings fields (3) | ~30 | `gpsEnabled`, `colorGps`, `speedMuteRequireObd` |
| GPS API endpoints (4 routes) | ~50 | `/api/gps/status`, `/observations`, `/config` |
| GPS display indicator | ~40 | "G{satCount}" badge + dirty flag |
| GPS web UI sections | ~200 | Integrations card, Colors picker, Dashboard pill, stores |
| GPS test refactoring | ~200 | Speed source tests lose GPS scenarios |
| **Total estimated** | **~3,100** | |

## Final Verification Checklist

After all GPS phases (13–21) are complete:

```bash
# 1. Native tests
pio test -e native
# Expected: 1100+ test cases, 0 failures

# 2. Device firmware build
pio run -e waveshare-349
# Expected: SUCCESS, further size reduction

# 3. Full CI suite
./scripts/ci-test.sh
# Expected: All gates pass

# 4. No GPS source files remaining
find src/ include/ -iname "*gps*" -type f
# Expected: empty

# 5. No GPS references in production or test code
grep -rni "gpsRuntime\|gpsObservation\|GpsRuntime\|GpsObservation\|GpsApi\|drawGpsIndicator\|gpsSat\|gpsEnabled\|colorGps\|speedMuteRequireObd\|api/gps" \
  src/ include/ test/ --include="*.cpp" --include="*.h" --include="*.js" --include="*.svelte"
# Expected: empty (docs/artifacts may still reference for historical context)

# 6. Interface build + tests
cd interface && npm run build && npm test
# Expected: clean build, all tests pass
```

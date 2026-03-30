# Test Baseline

Established 2026-03-29. This document records what the test suite looks like at
the first clean, trusted baseline — after struct unification but before any
architecture refactoring.

---

## Baseline Numbers

| Metric | Value |
|---|---|
| Total test cases | 1471 |
| Passing | 1466 |
| Skipped | 5 |
| Failing | 0 |
| Runtime (native) | ~1m20s |

Run with: `pio test -e native`

The 5 skipped tests are device-only tests excluded from the native environment
by the build config. They are expected and do not represent failures.

---

## What Changed to Reach This Baseline

The baseline was established immediately after fixing struct drift between real
headers and test mocks. Before this work, several structs were independently
redefined in multiple places, meaning tests could silently compile against a
different struct shape than firmware saw at runtime.

Fixes applied:

- Extracted `GpsRuntimeStatus` into `src/modules/gps/gps_runtime_status.h`
  (pure C++, no Arduino dependency). All mocks and test files now include
  the canonical definition — no local copies remain.

- Extracted `Band`, `Direction`, `AlertData`, `DisplayState` into
  `src/packet_parser_types.h`. Same principle: one definition, included
  everywhere.

- Updated `test/mocks/modules/gps/gps_runtime_module.h` to include the
  canonical struct. The old mock had `uint16_t hdop` (type mismatch vs
  the real `float`), missing ~20 fields, and was silently passing tests
  against the wrong layout.

- Removed local `GpsRuntimeStatus` redefinitions from:
  `test_lockout_enforcer.cpp`, `test_lockout_area_safety.cpp`,
  `test_gps_api_service.cpp`

Known remaining exceptions (intentional, self-contained):

- `test_display.cpp` — defines its own `AlertData`/`DisplayState` locally.
  This test is a standalone display-logic test with no dependency on the
  shared mock infrastructure. Migration deferred; the local types don't
  interact with the canonical ones.

- `test_alert_persistence.cpp` — defines its own `AlertData` with an extra
  `alertIndex` field specific to alert persistence logic. Also intentional.

---

## Architecture Violations — Phase 5 Status (updated 2026-03-29)

### HIGH — Direct global access inside modules ✅ RESOLVED

All `main_globals.h` includes have been removed from `src/modules/`. The scan
confirmed zero remaining violations of this type.

| File | Status | Notes |
|---|---|---|
| `src/modules/debug/debug_api_service.cpp` | ✅ Fixed | `begin()` injection via `debug_api_service_deps.h` |
| `src/modules/debug/debug_api_scenario_service.cpp` | ✅ Fixed | Uses same deps header |
| `src/modules/speed/speed_source_selector.cpp` | ✅ Fixed | `wireSpeedSources()` injection |
| `src/modules/obd/obd_api_service.cpp` | ✅ Already clean | Passes deps as params |
| `src/modules/gps/gps_api_service.cpp` | ✅ Already clean | Passes deps as params |
| `src/modules/lockout/lockout_api_service.cpp` | ✅ Already clean | Passes deps as params |
| `src/modules/obd/obd_runtime_module.cpp` | ⚠️ Deferred | `obdBleClient` access; all 33 callsites inside `#ifndef UNIT_TEST`. Tests unaffected. Bidirectional dep (`init(this)`) makes clean injection complex. Revisit when OBD BLE path is next reworked. |

### MEDIUM — `std::function` wiring (retired pattern)

~14 files still use `std::function` in wiring structs. Migrate to
`Providers` + `void* ctx` when each file is next touched — do not bulk-migrate.

Remaining files: WiFi API services (10 files), OBD/GPS API services,
debug services. See `ARCHITECTURE.md` for the correct pattern.

### MEDIUM — `extern` globals exported from module headers

Externs in module headers make globals reachable without injection.
Target for removal as modules are converted to `begin()` injection:
`gps_runtime_module.h`, `obd_runtime_module.h`, `obd_ble_client.h`,
`lockout_index.h`, `signal_capture_module.h`

### LOW — `begin()` parameter count

`display_orchestration_module.h` (12 params) and
`lockout_orchestration_module.h` (11 params) exceed the 6-parameter
guideline. Evaluate grouping related dependencies into a struct when
those modules are next refactored.

---

## Protecting the Baseline

Before starting any refactor: `pio test -e native` — note the passing count.
After the refactor: same command. Any regression must be explained and resolved
before committing.

The baseline is 1466 passing / 0 failing / 5 skipped. Do not merge work that reduces this.

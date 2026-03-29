# Test Baseline

Established 2026-03-29. This document records what the test suite looks like at
the first clean, trusted baseline — after struct unification but before any
architecture refactoring.

---

## Baseline Numbers

| Metric | Value |
|---|---|
| Total test cases | 1302 |
| Passing | 1297 |
| Skipped | 5 |
| Failing | 0 |
| Runtime (native) | ~1m39s |

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

## Architecture Violations Inventoried (Not Yet Fixed)

The following violations were identified during the Phase 4 scan but are NOT
fixed in this baseline. They represent the Phase 5 work queue.

### HIGH — Direct global access inside modules

Modules that reach for globals directly instead of receiving dependencies
through `begin()`. These undermine testability and create hidden coupling.

| File | Globals accessed |
|---|---|
| `src/modules/speed/speed_source_selector.cpp` | `gpsRuntimeModule`, `obdRuntimeModule` |
| `src/modules/obd/obd_runtime_module.cpp` | `obdBleClient` |
| `src/modules/obd/obd_api_service.cpp` | `obdRuntimeModule`, `settingsManager` |
| `src/modules/gps/gps_api_service.cpp` | `gpsRuntimeModule` |
| `src/modules/lockout/lockout_api_service.cpp` | `lockoutIndex`, others |
| `src/modules/debug/debug_api_service.cpp` | `main_globals.h` (known, flagged) |
| `src/modules/debug/debug_api_scenario_service.cpp` | `main_globals.h` (known, flagged) |

### MEDIUM — `std::function` wiring (retired pattern)

18 files use `std::function` in wiring structs. Heap allocation overhead is
unacceptable on ESP32. These are to be migrated to `Providers` + `void* ctx`
when each file is next touched.

Bulk of the work is in WiFi API services (10 files) and touch/debug services.
See `ARCHITECTURE.md` for the correct pattern.

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

## Recommended Order for Phase 5 Refactoring

Start with the most isolated modules and work inward toward core:

1. **Debug API services** — known violation, low risk, isolated.
   Fix `main_globals.h` dependency by injecting globals via `begin()`.

2. **GPS API service** — small file, single global dependency.

3. **OBD API service** — similar to GPS service.

4. **WiFi API services** — bulk `std::function` migration. Do one at a time.
   Confirm each passes the 1297 baseline before moving to the next.

5. **speed_source_selector** — injecting `gpsRuntimeModule` and
   `obdRuntimeModule` via `begin()` is the right fix here.

6. **Lockout/OBD orchestration** — tackle last; most coupled.

---

## Protecting the Baseline

Before starting any refactor: `pio test -e native` — note the passing count.
After the refactor: same command. Any regression must be explained and resolved
before committing.

The baseline is 1297 passing / 0 failing. Do not merge work that reduces this.

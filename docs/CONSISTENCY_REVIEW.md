# V1 Gen2 Simple Consistency Review (Claim Ledger)

> Reviewed: February 13, 2026  
> Method: Static source audit (no hardware/runtime profiling in this pass)  
> Scope: `src/`, `include/`, `src/modules/`, `interface/`, `platformio.ini`

## Status Key

- `REAL`: Confirmed in current source.
- `PARTIAL`: Claim intent is true but wording needed correction.
- `OUTDATED`: Claim no longer matches current source.
- `RUNTIME`: Plausible concern that needs device/runtime validation.

## Claim Ledger

| ID | Claim | Status | Evidence | How To Verify |
|---|---|---|---|---|
| C01 | Logging styles are mixed (`Serial`, `SerialLog`, `DBG_LOGF`, module macros). | REAL | `src/main.cpp`, `src/display.cpp`, `src/audio_beep.cpp`, `src/modules/*` | `rg -n "Serial\\.|SerialLog|DBG_LOGF" src include -S` |
| C02 | Log prefix style is inconsistent (`[DISP]`, `[Display]`, `[PERF]`, `[Perf]`, no prefix). | REAL | `src/display.cpp`, `src/perf_metrics.cpp`, `src/main.cpp`, `src/modules/touch/tap_gesture_module.cpp` | `rg -n "\\[DISP\\]|\\[Display\\]|\\[PERF\\]|\\[Perf\\]" src -S` |
| C03 | Some error logs are debug-gated in audio/BLE paths. | REAL | `src/audio_beep.cpp`, `src/ble_client.cpp`, `src/debug_logger.h`, `platformio.ini` | `rg -n "DISABLE_DEBUG_LOGGER|AUDIO_LOG|BLE_SM_LOGF" src platformio.ini -S` |
| C04 | BLE exponential backoff logic is duplicated. | REAL | `src/ble_client.cpp` reconnect paths | `rg -n "Calculate exponential backoff|BACKOFF_BASE_MS" src/ble_client.cpp -S` |
| C05 | `Band -> string` conversion exists in multiple paths. | REAL | `src/packet_parser.cpp`, `src/display.cpp` | `rg -n "bandToString\\(" src -S` |
| C06 | `wifi_manager.cpp` had duplicate `perf_metrics.h` include. | REAL | `src/wifi_manager.cpp` include section (now corrected) | `rg -n "perf_metrics\\.h" src/wifi_manager.cpp -S` |
| C07 | Settings mutations in handlers used `const_cast` in multiple paths. | REAL | `src/wifi_manager.cpp` handlers (now replaced with mutable API) | `rg -n "const_cast<\\s*V1Settings" src/wifi_manager.cpp -S` |
| C08 | OBD oil temp flow restored CAN header but ignored restore result. | REAL | `src/obd_handler.cpp` `requestOilTemp()` (now logs failure) | `rg -n "ATSH7E0|ATSH7DF|requestOilTemp" src/obd_handler.cpp -S` |
| C09 | Module loop API naming is mixed (`process`, `update`, `handleParsed`, `select`). | REAL | `src/main.cpp`, `src/modules/*` | `rg -n "process\\(|update\\(|handleParsed\\(|select\\(" src/modules src/main.cpp -S` |
| C10 | UI still posted legacy `POST /settings` from Settings/Colors pages. | REAL | `interface/src/routes/settings/+page.svelte`, `interface/src/routes/colors/+page.svelte` (now API-first + fallback) | `rg -n "fetch\\('/settings'|/api/settings" interface/src/routes -S` |
| C11 | Backend keeps both `POST /api/settings` and legacy `POST /settings`. | REAL | `src/wifi_manager.cpp` route registration | `rg -n "server\\.on\\(\"/api/settings\"|server\\.on\\(\"/settings\"" src/wifi_manager.cpp -S` |
| C12 | Windows env omitted `ESP32QSPI_FREQUENCY=80000000` vs primary env. | REAL | `platformio.ini` env blocks (now aligned) | `rg -n "ESP32QSPI_FREQUENCY|\\[env:waveshare-349|\\[env:waveshare-349-windows" platformio.ini -S` |
| C13 | `resetPriorityState()` was listed dead/no-op. | PARTIAL | It is called in disconnect flow and tests, but parser implementation is intentionally no-op. | `rg -n "resetPriorityState\\(" src test -S` |
| C14 | `DisplayMode` was listed dead. | OUTDATED | Active in module pipeline and main loop wiring. | `rg -n "DisplayMode" src -S` |
| C15 | “4 PERF_INC wrappers incl. volume-fade wrapper” claim. | OUTDATED | Current wrappers differ; cited wrapper count/identity drifted. | `rg -n "#define\\s+\\w*PERF_INC\\(" src/modules src -S` |
| C16 | “Zero shared UI layout/components” claim. | OUTDATED | `+layout.svelte` provides shared shell/nav/warnings. | `rg -n "\\+layout\\.svelte|nav|children" interface/src/routes -S` |

## Runtime / Hardware Validation Items

| ID | Runtime Claim | Status | Why Runtime Is Required |
|---|---|---|---|
| R01 | Audio amp sequencing asymmetry impact. | RUNTIME | Code pattern exists, but user impact depends on real timing/device behavior. |
| R02 | Exact display flush cost/wasted ms claims. | RUNTIME | Requires on-device timing capture, not static inference. |
| R03 | Full-frame vs partial flush benefit. | RUNTIME | Needs target panel profiling in representative scenarios. |
| R04 | Perf max-counter race significance. | RUNTIME | Must be measured under stress; static race plausibility does not quantify impact. |
| R05 | Production impact of debug-gated logs. | RUNTIME | Depends on build flags used in deployed artifacts. |

## Practical Risk View

- High-confidence maintainability risks: logging inconsistency, duplicate logic blocks, API consistency debt.
- Medium behavior risks: settings mutation boundaries and hidden failure observability.
- Runtime-only risks: display timing/perf impact and audio sequencing impact.

## Policy For Future Reviews

Use this template per claim:

- Claim
- Status (`REAL` / `PARTIAL` / `OUTDATED` / `RUNTIME`)
- Evidence (exact file + function)
- Last revalidated (date + commit SHA)
- Verification command and/or hardware test

This prevents drift and makes follow-up work safe to split into small commits.

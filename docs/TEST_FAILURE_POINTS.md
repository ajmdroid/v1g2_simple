# Test Failure Points Audit

> Date: March 8, 2026
> Scope: current repo test inventory plus latest local `./scripts/ci-test.sh` run

This file documents the failure points the test suite is explicitly designed to
catch today, plus the practical ways the harness itself can fail.

## Current Baseline

- Total suites in `test/`: 83
- Native suites: 74
- Device-only suites: 9
- Frontend unit test files in `interface/`: 3
- Frontend unit tests: 11 passed
- Frontend scoped coverage baseline:
  - statements: 91.66% (209/228)
  - branches: 80.35% (135/168)
  - functions: 100% (44/44)
  - lines: 97.71% (171/175)
- Current frontend coverage scope:
  - `interface/src/lib/utils/colors.js`
  - `interface/src/lib/utils/lockout.js`
  - `interface/src/lib/components/ToggleSetting.svelte`
- Latest local `./scripts/ci-test.sh` native result on `dev` (March 8, 2026):
  918 cases, 913 passed, 5 skipped
- Hardware gates were not rerun in this audit; this document uses the current
  scripts and test inventory for the hardware section

## What The Suite Is Good At Catching

### 1. API and schema validation failures

Representative suites:

- `test_wifi_client_api_service`
- `test_wifi_control_api_service`
- `test_wifi_settings_api_service`
- `test_wifi_v1_profile_api_service`
- `test_wifi_v1_sync_api_service`
- `test_camera_alert_api_service`
- `test_lockout_store`

Typical failure points covered:

- missing request bodies or missing required fields
- invalid JSON payloads
- invalid booleans, numeric tokens, or profile names
- invalid schema versions or malformed lockout JSON
- 400/404/500 mapping for expected bad inputs
- partial-mutation prevention on rejected writes

Coverage signal:

- At least 82 failure-oriented test names target input validation and malformed
  payload handling

### 2. Rate-limit and route-guard failures

Representative suites:

- `test_wifi_client_api_service`
- `test_wifi_control_api_service`
- `test_wifi_status_api_service`
- `test_wifi_time_api_service`
- `test_lockout_api_service`
- `test_debug_api_service`
- `test_gps_api_service`

Typical failure points covered:

- duplicate or rapid API requests short-circuiting correctly
- route guard behavior preserving existing double-rate-limit semantics
- debug and settings endpoints refusing over-frequent writes

Coverage signal:

- At least 38 tests target rate limiting or short-circuit behavior

### 3. Timing, wraparound, timeout, stale-data, and debounce failures

Representative suites:

- `test_gps_runtime`
- `test_wifi_process_cadence_module`
- `test_loop_tail_module`
- `test_periodic_maintenance_module`
- `test_connection_state_cadence_module`
- `test_lockout_pre_quiet`
- `test_power_module`

Typical failure points covered:

- millis wraparound handling
- stale GPS fix invalidation
- detection timeouts
- dwell and debounce windows
- boot hold / settle timing
- timeout-based auto power-off
- stale candidate pruning

Coverage signal:

- At least 59 tests target timeout, wrap-safe timing, stale state, or debounce

### 4. Resource pressure, overflow, and queue-drop failures

Representative suites:

- `test_system_event_bus`
- `test_device_heap`
- `test_device_heap_stress`
- `test_device_freertos`
- `test_device_event_bus`
- `test_gps_lockout_safety`
- `test_lockout_index`

Typical failure points covered:

- OOM returning `null` instead of crashing
- repeated alloc/free leak detection
- queue overflow behavior
- event bus overflow preferring frame-drop behavior
- full-index lockout behavior and eviction
- DMA floor and queue high-water pressure in soak scripts

Coverage signal:

- At least 52 tests target overflow, full-capacity, drop, or leak conditions

### 5. Disconnect/reconnect and recovery failures

Representative suites:

- `test_ble_client`
- `test_connection_state`
- `test_lockout_pre_quiet`
- `test_lockout_runtime_mute`
- `test_power_module`
- `test_wifi_boot_policy`
- `test_display`

Typical failure points covered:

- BLE disconnect and reconnect state transitions
- backoff / hard-reset thresholds
- reconnect recovery without stale state leakage
- clearing state after alert exit or preview end
- preventing RESTING when V1 is disconnected after test mode

Coverage signal:

- At least 80 tests target recovery, clear/reset, disconnect, restore, or rearm

### 6. Parser and stream-integrity failures

Representative suites:

- `test_packet_parser`
- `test_packet_parser_stream`
- `test_ble_display_pipeline`
- `test_road_map_reader`
- `test_json_stream_response`

Typical failure points covered:

- bad packet framing
- duplicate row handling
- partial alert-table timeout reset
- stale row reuse prevention
- ambiguous first-row handling
- stream write retry budget exhaustion
- bad map magic / short buffer rejection

Coverage signal:

- At least 73 tests target parsing, duplication, truncation, decode, checksum,
  or round-trip integrity

### 7. Display-path regressions

Representative suites:

- `test_display`
- `test_display_orchestration_module`
- `test_display_restore_module`

Typical failure points covered:

- no-redraw caching invariants
- frequency-tolerance redraw suppression
- stale lockout badge clearing on disconnect
- preview restore timing
- full-screen clear stability
- "stuck screen" regression where test mode ended and UI did not return to
  SCANNING when V1 was disconnected

### 8. GPS / lockout safety regressions

Representative suites:

- `test_lockout_enforcer`
- `test_lockout_learner`
- `test_lockout_index`
- `test_lockout_pre_quiet`
- `test_drive_scenario`
- `test_gps_lockout_safety`

Typical failure points covered:

- no GPS fix / no location gating
- wrong band / wrong frequency / wrong direction mismatch
- unsupported-band handling
- stale candidate pruning
- clean-pass demotion policy
- queue/perf/event drop safety trip points
- GPS flicker without flip-flop behavior

### 9. Frontend shared-helper and control regressions

Representative tests:

- `interface/src/lib/utils/colors.test.js`
- `interface/src/lib/utils/lockout.test.js`
- `interface/src/lib/components/ToggleSetting.test.js`

Typical failure points covered:

- invalid RGB565 and RGB888 color parsing
- boolean normalization drift for display color settings
- malformed custom-color hex generation
- lockout clamp and formatting helper regressions
- map-link and direction-summary regressions in lockout UI helpers
- shared toggle controls failing to reflect checked state or emit change events

Coverage signal:

- 11 frontend unit tests now gate the shared helper/control layer
- scoped coverage currently measures `colors.js`, `lockout.js`, and
  `ToggleSetting.svelte`

## Highest-Value Suites To Read First

These suites contain the densest concentration of failure handling:

- `test_lockout_index`: capacity, merge, duplicate suppression, out-of-range,
  direction gating, unsupported bands, confidence decay
- `test_lockout_pre_quiet`: BLE disconnects, GPS loss/flicker, debounce,
  safety timeout, reentry behavior
- `test_ble_client`: backoff progression, hard reset threshold, checksum and
  state-string edge cases
- `test_display`: restore invariants, redraw suppression, forced redraw,
  boundary clamping, display torture coverage
- `test_lockout_store`: malformed JSON, schema rejection, overflow truncation,
  direction fallback, dirty/stat reset
- `test_lockout_learner`: stale pruning, batch backlog, rate limiting, full
  index failure, no-location handling
- `test_wifi_client_api_service`: malformed payloads, disconnected-state
  behavior, scan initiation, credential clearing
- `test_wifi_boot_policy`: BLE settle gates, DMA gates, timeout unlock, WiFi
  disabled-state behavior
- `test_lockout_enforcer`: no-fix/no-alert short paths, mismatch blocking,
  clean-pass behavior, shadow mode differences
- `test_gps_runtime`: checksum/date/coordinate rejection, stale-fix behavior,
  overlong sentence rejection, timeout disable path

## Harness And Gate Failure Points

### `./scripts/ci-test.sh`

Now fails on:

- contract guard mismatches
- extern/global usage contract mismatches
- native test failures
- frontend lint/type failures via `svelte-check`
- frontend unit test failures via Vitest
- frontend coverage generation/config regressions
- web build/deploy failures
- web asset budget or packaging regressions
- firmware static-analysis failures via `pio check`
- firmware build failures

### `./scripts/device-test.sh`

Concrete fail triggers:

- no response from the metrics endpoint after retry budget
- metrics JSON missing required keys:
  - `rxPackets`
  - `parseSuccesses`
  - `parseFailures`
  - `dispPipeMaxUs`
  - `wifiMaxUs`
  - `displayUpdates`
- camera smoke returning non-zero or explicit `failure=...`
- short radar scenario timing out
- scenario metrics reset/start/cleanup failure
- `eventsTotal == 0`
- `eventsEmitted < eventsTotal`
- `rxPackets`, `parseSuccesses`, or `displayUpdates` below required deltas
- `parseFailures != 0`
- synthetic suite failure when `uptimeMs` decreases between test items

Operational prerequisites:

- default metrics URL points at `http://192.168.160.212/api/debug/metrics`
- camera smoke requires a local Chrome/Chromium install
- authoritative results require the hardware image on the target device to
  match the branch under test

### `./scripts/run_real_fw_soak.sh`

Concrete fail triggers:

- serial monitor exits during soak
- serial panic/WDT signatures
- serial reset signatures
- panic endpoint reports crashes
- metrics required but no successful metrics window captured
- metrics sample count below `--min-metrics-ok-samples`
- `parseFailures`, `queueDrops`, `perfDrop`, `eventBus` drops, or
  `oversizeDrops` above thresholds
- loop / flush / WiFi / BLE drain / SD / FS latency peaks above configured max
- queue high-water above threshold
- WiFi connect deferrals above profile allowance
- DMA free / largest-block floors below threshold
- BLE mutex timeout counters above threshold
- transition drive errors or incomplete flap cycles
- AP-down / proxy-off transition counts below minimum
- unstable recovery events after AP down or proxy advertising off
- recovery time or samples-to-stable above configured max
- display drive mode producing zero or too few display updates

Special result:

- `INCONCLUSIVE` if no signal sources were captured

## Conditional Or Skipped Coverage

- `test_packet_parser_stream` contains 5 strict backup-audit tests that are
  skipped unless `V1_STRICT_BACKUP_AUDIT=1`
- `test_device_psram` skips when PSRAM is not present
- one `test_device_heap` path skips when PSRAM is absent
- `test_device_nvs` can ignore `nvs_get_stats` checks on IDF variants that do
  not expose that API
- `test_device_coexistence` can ignore one pressure-allocation path if the
  pressure allocation cannot be created

## Blind Spots And Follow-Ups

- Extern/global drift is now snapshotted in CI, but the current global surface
  still allows direct subsystem access in legacy paths
- Frontend browser/route tests are still absent; current frontend unit coverage
  is scoped to shared helpers and one reusable control
- The strict packet-parser backup audit is off by default, so part of the
  parser/backup compatibility coverage is opt-in
- Hardware gate results are only as good as the flashed firmware on the device;
  the reachable device at `192.168.160.63` likely needs flash alignment before
  it can be used for authoritative review
- Some older docs still mention larger native test counts than the current
  local run; use this file plus the current script outputs as the active audit

## Recommended Focus Order When A Failure Appears

1. Reboot / panic / reset evidence
2. Parser, queue, perf-drop, or event-drop counters
3. Transition recovery and BLE/WiFi coexistence churn
4. DMA floors, queue high-water, or WiFi deferral pressure
5. API/schema validation regressions
6. Display restore / redraw path regressions

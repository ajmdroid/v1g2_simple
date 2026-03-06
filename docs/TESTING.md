# Testing Suite Documentation

> Status: Active
> Date: March 5, 2026

This document defines V1-Simple's standard test procedures, soak/stress testing protocols, drive-log analysis, and how everything ties back to the performance SLOs in `PERF_SLOS.md`.

---

## Table of Contents

1. [Quick Reference](#quick-reference)
2. [Standard Code Tests (Pre-Push)](#standard-code-tests-pre-push)
3. [Contract Guards](#contract-guards)
4. [Native Unit Tests](#native-unit-tests)
5. [Functional Test Gate](#functional-test-gate)
6. [Device Hardware Tests](#device-hardware-tests)
7. [Soak & Stress Testing](#soak--stress-testing)
8. [Drive Log Analysis](#drive-log-analysis)
9. [Performance SLO Scoring](#performance-slo-scoring)
10. [Web Baseline Testing](#web-baseline-testing)
11. [Iron Gate (Ship Gate)](#iron-gate-ship-gate)
12. [Test Artifacts & Reports](#test-artifacts--reports)
13. [Test Pyramid Summary](#test-pyramid-summary)

---

## Quick Reference

```bash
# ── Standard pre-push (REQUIRED before every commit) ──
./scripts/ci-test.sh

# ── Native unit tests only ──
pio test -e native

# ── Functional integration tests ──
./scripts/run_functional_tests.sh

# ── Device hardware tests ──
./scripts/run_device_tests.sh              # Core suites
./scripts/run_device_tests.sh --quick      # Boot + heap only
./scripts/run_device_tests.sh --full       # Device + native on hardware

# ── Soak & stress ──
./scripts/run_device_soak.sh --cycles 20   # Device test flake detection
./scripts/run_real_fw_soak.sh \            # Production firmware soak
  --duration-seconds 900 \
  --metrics-url http://192.168.35.5/api/debug/metrics

# ── Drive log scoring ──
python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1.csv \
  --profile drive_wifi_off

# ── Ship gate ──
./scripts/iron-gate.sh
```

---

## Standard Code Tests (Pre-Push)

**Every commit must pass `./scripts/ci-test.sh` before being pushed.** This is the single standard test for all code changes. It runs the full local CI pipeline:

### Pipeline Steps (in order)

| # | Step | What it does |
|---|------|--------------|
| 1 | Contract guards (9 checks) | Enforce architectural invariants (see below) |
| 2 | Native unit tests | `pio test -e native` — 960+ test cases |
| 3 | Web interface build | `npm ci && npm run build && npm run deploy` |
| 4 | Web asset guardrails | Size budget and duplicate-asset checks |
| 5 | Firmware build | `pio run -e waveshare-349` |
| 6 | Size report | Flash/RAM usage summary |

A failure at any step aborts the pipeline (`set -e`).

---

## Contract Guards

Contract guards are static-analysis scripts that enforce architectural invariants. They prevent regressions in critical subsystem boundaries without running any firmware. Each guard snapshots its expected state in `test/contracts/`.

| Guard | Script | What it enforces |
|-------|--------|-----------------|
| WiFi API routes | `check_wifi_api_contract.py` | HTTP route signatures, rate-limit policies, delegate bindings, shim absence |
| BLE hot-path | `check_ble_hot_path_contract.py` | No `Serial.print`, `malloc`, `delay`, or blocking semaphore in BLE callbacks; `parser→parse()` only from `BleQueueModule::process()` |
| Perf CSV columns | `check_perf_csv_column_contract.py` | `PERF_CSV_HEADER` column order/count matches `snprintf()` format fields |
| Display flush | `check_display_flush_discipline_contract.py` | `DISPLAY_FLUSH` call sites stable; no heap allocation outside `begin`/`init` scopes |
| SD lock discipline | `check_sd_lock_discipline_contract.py` | SD lock class usage by file; no raw `xSemaphoreTake(sdMutex)` outside `storage_manager`; no `SDLockBlocking` in `main.cpp` |
| Main loop order | `check_main_loop_call_order_contract.py` | Subsystem call order in `loop()`: BLE → queue → display → WiFi; no `delay()` or blocking waits |
| Frontend HTTP resilience | `check_frontend_http_resilience_contract.py` | All HTTP requests use `fetchWithTimeout`; all polling uses `createPoll` |
| Web asset budget | `check_web_asset_budget.py` | No duplicate raw+gz assets; `data/` total ≤ 2.1 MB; no legacy audio clips |

Contract snapshot files live in `test/contracts/`:

```
test/contracts/
├── ble_hot_path_contract.txt
├── display_flush_discipline_contract.txt
├── main_loop_call_order_contract.txt
├── perf_csv_column_contract.txt
├── sd_lock_discipline_contract.txt
├── wifi_handler_policy_contract.txt
├── wifi_local_handler_route_contract.txt
├── wifi_route_contract.txt
└── wifi_shim_absence_contract.txt
```

When a contract-guarded area changes intentionally, update the snapshot file in the same commit.

---

## Native Unit Tests

Native tests run on the host machine using PlatformIO's native environment. They mock ESP32/Arduino hardware and test logic in isolation.

```bash
# Run all native tests
pio test -e native

# Run a specific suite
pio test -e native --filter test_packet_parser
pio test -e native --filter test_display
pio test -e native --filter test_drive_scenario

# Verbose output
pio test -e native -v
```

### Current Baseline

| Metric | Value |
|--------|-------|
| Test suites | 76 |
| Test cases | 960+ |
| Environment | `native` (host machine) |

### Key Suite Categories

- **Packet parsing** — V1 protocol decoding, boundary conditions
- **Display system** — Band/direction decoding, frequency tolerance, cache invalidation, stress
- **Drive scenarios** — Replay-driven cross-module integration flows
- **WiFi manager** — State machine, boot policy, connection behavior
- **Lockout enforcer** — Enforcement decisions, GPS thresholds
- **Alert persistence** — State tracking across connection cycles
- **BLE client** — Connection state, command pacing, reconnection logic

---

## Functional Test Gate

Focused behavior-level integration tests that exercise cross-module flows.

```bash
# Native only
./scripts/run_functional_tests.sh

# Include device hardware
./scripts/run_functional_tests.sh --with-device
```

### Suites Covered

| Suite | What it tests |
|-------|--------------|
| `test_drive_scenario` | End-to-end drive replay flows |
| `test_lockout_enforcer` | Lockout enforcement decisions |
| `test_wifi_boot_policy` | WiFi startup gating logic |
| `test_wifi_manager` | WiFi state machine behavior |

Reports are written to `.artifacts/test_reports/functional_<timestamp>/` in JSON, JUnit XML, and log formats.

---

## Device Hardware Tests

Tests that run on real ESP32-S3 hardware to catch issues native tests cannot reproduce: heap fragmentation, PSRAM corruption, FreeRTOS contention, radio coexistence, NVS persistence, and I2C interaction.

### Running

```bash
# All device suites (boot → heap → PSRAM → RTOS → NVS → battery → radio)
./scripts/run_device_tests.sh

# Quick sanity (boot + heap only)
./scripts/run_device_tests.sh --quick

# Full (device suites + shared native suites on hardware)
./scripts/run_device_tests.sh --full

# Include stress suites
./scripts/run_device_tests.sh --stress

# Single suite
pio test -e device --filter test_device_heap
```

### Device Suite Reference

| Suite | Category | What it catches |
|-------|----------|-----------------|
| `test_device_boot` | Core / System | Post-boot baseline, CPU/PSRAM detection, flash/partition validation |
| `test_device_heap` | Core / Memory | Internal SRAM leaks, fragmentation, OOM resilience |
| `test_device_psram` | Core / Memory | PSRAM detection, 4 MB pattern-verify, write-speed sanity |
| `test_device_freertos` | Core / RTOS | Queue overflow, semaphore, cross-task communication |
| `test_device_event_bus` | Core / Concurrency | SystemEventBus under real portMUX across cores |
| `test_device_nvs` | Dependent / Persistence | NVS write/read, namespace A/B toggle, XOR obfuscation |
| `test_device_battery` | Dependent / Hardware | ADC sampling, TCA9554 I2C, power-latch, button GPIO |
| `test_device_coexistence` | Dependent / Radio | WiFi AP heap impact, DMA gate, BLE+WiFi contention |
| `test_device_heap_stress` | Stress | Fragmentation churn, alloc/free leaks, near-OOM (manual run) |

### Prerequisites

- ESP32-S3 connected via USB
- Auto-detects serial port (or set `DEVICE_PORT` env var)
- Port locking prevents concurrent test conflicts

---

## Soak & Stress Testing

Soak testing validates stability over extended periods. There are two distinct soak modes:

### 1. Device Test Soak (Flake Detection)

Repeats the device test suite N times to catch intermittent failures and collect stability metrics.

```bash
# 20-cycle soak with 6s cooldown between cycles
./scripts/run_device_soak.sh --cycles 20 --cooldown-seconds 6

# Stop on first failure
./scripts/run_device_soak.sh --cycles 50 --stop-on-fail

# Quick mode (boot + heap only per cycle)
./scripts/run_device_soak.sh --cycles 20 --quick

# Full mode (device + native suites per cycle)
./scripts/run_device_soak.sh --cycles 10 --full
```

**Output**: `cycles.csv` with per-cycle metrics (heap, WDT count, event bus stats, reset reasons) plus `summary.md`.

**Artifacts**: `.artifacts/test_reports/device_soak_<timestamp>/`

### 2. Real Firmware Soak (Production Stability)

Flashes the production firmware image (not test firmware) and monitors runtime metrics over an extended duration via the HTTP debug endpoint.

```bash
# Standard 15-minute soak
./scripts/run_real_fw_soak.sh \
  --duration-seconds 900 \
  --metrics-url http://192.168.35.5/api/debug/metrics

# Display-path stress soak (forces display redraws)
./scripts/run_real_fw_soak.sh --skip-flash \
  --duration-seconds 900 \
  --metrics-url http://192.168.35.5/api/debug/metrics \
  --require-metrics --min-metrics-ok-samples 50 \
  --drive-display-preview --display-drive-interval-seconds 6 \
  --min-display-updates-delta 100

# BLE transition stress (proxy on/off flap)
./scripts/run_real_fw_soak.sh --skip-flash \
  --duration-seconds 900 \
  --metrics-url http://192.168.35.5/api/debug/metrics \
  --drive-transition-flap

# Derive gates from a baseline session
./scripts/run_real_fw_soak.sh --skip-flash \
  --duration-seconds 900 \
  --metrics-url http://192.168.35.5/api/debug/metrics \
  --baseline-perf-csv /Volumes/SDCARD/perf/perf_boot_1.csv
```

**Key options:**

| Flag | Purpose |
|------|---------|
| `--duration-seconds N` | Soak duration (default: 300s) |
| `--metrics-url URL` | HTTP endpoint for runtime metrics |
| `--skip-flash` | Reuse existing firmware on device |
| `--require-metrics` | Fail if no telemetry captured |
| `--drive-display-preview` | Force display redraw during soak |
| `--drive-transition-flap` | BLE proxy on/off cycling |
| `--baseline-perf-csv PATH` | Derive pass/fail gates from a known-good session |
| `--latency-gate-mode strict` | Peak-only latency gating (default: hybrid N-of-M) |

**Latency gating**: By default, `run_real_fw_soak.sh` uses **hybrid** mode — strict peak gates are reported, but pass/fail uses a robust N-of-M check (max 5% over-limit samples, min 8 samples). Use `--latency-gate-mode strict` for peak-only enforcement.

**Inconclusive runs**: If no telemetry is captured, the run exits with code `2` (`INCONCLUSIVE`) rather than reporting a false pass.

### Metrics Gated During Soak

The real firmware soak validates the same metrics defined in `PERF_SLOS.md`:

- **Latency ceilings**: loop, BLE drain, BLE process, display pipeline, flush, SD, FS, WiFi
- **Integrity counters**: queue drops, parse failures, oversize drops, BLE mutex timeouts
- **Memory floors**: DMA free minimum, DMA largest block minimum
- **Queue health**: queue high-water mark
- **WiFi stability**: connection defer counts

### 3. Hardware Test Harness (device-test.sh)

The comprehensive hardware test harness runs a structured sequence:

```bash
./scripts/device-test.sh \
  --metrics-url http://192.168.35.5/api/debug/metrics \
  --duration-seconds 60 \
  --rad-scenario RAD-03
```

**Sequence:**
1. Poll uptime continuity — detect unintended reboots
2. Metrics endpoint sanity — verify `/api/debug/metrics` responds
3. Radar scenario — run short V1 scenario, verify BLE rx/parse/display deltas
4. Soak cycle 1 — 60s real firmware soak with display stress
5. Soak cycle 2 — 60s core stability without display churn
6. Transition qualification — optional BLE proxy on/off flap

---

## Drive Log Analysis

After a drive, the device writes perf CSVs to the SD card at `/perf/perf_boot_<bootId>.csv`. These logs are the primary source for post-mortem analysis.

### Scoring a Drive Log

```bash
# Score against the drive_wifi_off profile
python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1.csv \
  --profile drive_wifi_off

# JSON output for automation
python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1.csv \
  --profile drive_wifi_off --json

# Score with WiFi AP active profile
python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1.csv \
  --profile drive_wifi_ap
```

**Exit codes:**

| Code | Meaning |
|------|---------|
| `0` | All hard SLOs pass (advisories pass or warn) |
| `1` | Hard SLOs pass, one or more advisories fail |
| `2` | One or more hard SLOs fail |
| `3` | Input/tool error |

### Session Selection

The scoring tool can target specific sessions within a multi-session CSV:

- `last-connected` — last session with a BLE connection (default)
- `last` — most recent session regardless
- `longest-connected` — longest BLE-connected session
- `longest` — longest session overall
- Session by index

### Analyzing Drive Logs

```bash
# Correlate drive session across categories
python tools/analyze_drive_log.py /Volumes/SDCARD/perf/perf_boot_1.csv
```

This tool analyzes:
- **RSSI trend** — signal strength over time, drop detection
- **GPS fix quality** — fix status, drift detection
- **Alert patterns** — alert frequency, lockout/auto-lockout activity

### Deriving Soak Baselines from Drive Data

```bash
# Extract RX/parse gates from a known-good drive
python tools/soak_parse_baseline.py /Volumes/SDCARD/perf/perf_boot_1.csv
```

Derives minimum RX and parse delta gates that soak runs can use for validation.

### Runtime Metrics Smoke Check

```bash
# Non-destructive counter verification
python tools/smoke_metrics_runtime.py --profile power_safe

# Full check including shutdown triggers
python tools/smoke_metrics_runtime.py --profile power_full
```

Validates that perf counters increment correctly and reflect in the CSV log.

---

## Performance SLO Scoring

All performance validation is defined in `PERF_SLOS.md` and scored by `tools/score_perf_csv.py`.

### Run Profiles

| Profile | Description |
|---------|-------------|
| `drive_wifi_off` | Normal driving, setup AP not started (default boot behavior) |
| `drive_wifi_ap` | Driving with setup AP intentionally active |

### Hard SLOs (Release Gates)

| Metric | `drive_wifi_off` | `drive_wifi_ap` | Category |
|--------|--:|--:|---|
| `qDrop` | `== 0` | `== 0` | BLE queue integrity |
| `parseFail` | `== 0` | `== 0` | Parser integrity |
| `oversizeDrops` | `== 0` | `== 0` | Packet framing safety |
| `bleMutexTimeout` | `== 0` | `== 0` | BLE lockup guard |
| `loopMax_us` | `≤ 250 ms` | `≤ 250 ms` | Loop stall ceiling |
| `bleDrainMax_us` | `≤ 10 ms` | `≤ 10 ms` | Main-loop BLE drain |
| `bleProcessMax_us` | `≤ 120 ms` | `≤ 120 ms` | BLE process budget |
| `dispPipeMax_us` | `≤ 80 ms` | `≤ 80 ms` | Display pipeline |
| `flushMax_us` | `≤ 100 ms` | `≤ 100 ms` | Storage flush |
| `sdMax_us` | `≤ 50 ms` | `≤ 50 ms` | SD write chunk |
| `fsMax_us` | `≤ 50 ms` | `≤ 50 ms` | FS serve |
| `queueHighWater` | `≤ 12` | `≤ 12` | Queue occupancy margin |
| `dmaLargestMin` | `≥ 10 KB` | `≥ 10 KB` | DMA block floor |
| `dmaFreeMin` | `≥ 20 KB` | `≥ 20 KB` | DMA free-memory floor |
| `wifiConnectDeferred` | `== 0` | `≤ 5` | WiFi transition pressure |
| `wifiMax_us` | `≤ 1 ms` | `≤ 5 ms` | WiFi work budget |

### Advisory SLOs (Trend/Monitor)

| Metric | Limit | What it tracks |
|--------|------:|----------------|
| `cmdPaceNotYetPerMin` | `≤ 25` | BLE command pacing pressure |
| `displaySkipPct` | `≤ 20%` | Display throttle ratio |
| `displaySkipsPerMin` | `≤ 120` | UI draw-throttle pressure |
| `gpsObsDropsPerMin` | `≤ 200` | GPS observation consumer lag |
| `audioPlayBusyPerMin` | `≤ 2` | Audio contention signal |
| `reconn` | `≤ 2` | Connection stability trend |
| `disc` | `≤ 2` | Disconnect trend |

---

## Web Baseline Testing

Captures UI and API latency baselines from a running device.

```bash
python tools/web_baseline.py \
  --base-url http://192.168.35.5
```

Tests per-route latency for `/`, `/audio`, `/settings`, and all API contract routes. Outputs CSV and markdown latency reports to `.artifacts/web_baseline/`.

---

## Iron Gate (Ship Gate)

Single-command release gate that runs all critical checks in sequence.

```bash
./scripts/iron-gate.sh
```

### Gate Points

| # | Gate | Tool |
|---|------|------|
| 1 | Firmware build | `pio run -e waveshare-349` |
| 2 | SD lock contract | `check_sd_lock_discipline_contract.py` |
| 3 | Parser native smoke | `pio test -e native --filter test_packet_parser` |
| 4 | Device suite | `device-test.sh` |

Results are written to `.artifacts/iron_gate/iron_<timestamp>/` as TSV and markdown.

---

## Test Artifacts & Reports

All test outputs go to `.artifacts/`:

```
.artifacts/
├── test_reports/
│   ├── device_<timestamp>/
│   │   └── device.log
│   ├── device_soak_<timestamp>/
│   │   ├── cycles.csv          # Per-cycle metrics
│   │   └── summary.md          # Human-readable summary
│   └── functional_<timestamp>/
│       ├── native.json          # Machine-readable results
│       ├── native.xml           # JUnit XML
│       └── native.log           # Full output log
├── iron_gate/
│   └── iron_<timestamp>/
│       ├── results.tsv          # Gate point pass/fail
│       └── summary.md
└── web_baseline/
    ├── web_*.csv                # Latency samples
    └── baseline.md              # Latency report
```

---

## Test Pyramid Summary

The testing strategy follows a layered pyramid, from fastest/cheapest to slowest/most realistic:

| Layer | Tool | Scope | Speed | When to run |
|-------|------|-------|-------|-------------|
| **Contract guards** | `check_*.py` scripts | Architectural invariants | Seconds | Every commit (`ci-test.sh`) |
| **Native unit tests** | `pio test -e native` | Logic in isolation (960+ cases) | ~1 min | Every commit (`ci-test.sh`) |
| **Functional gate** | `run_functional_tests.sh` | Cross-module integration | ~2 min | Before merge, after behavioral changes |
| **Device tests** | `run_device_tests.sh` | Real hardware verification | ~5 min | After hardware-sensitive changes |
| **Device soak** | `run_device_soak.sh` | Flake detection (N cycles) | ~30 min | Regression hunting, pre-release |
| **Real firmware soak** | `run_real_fw_soak.sh` | Production stability + SLO scoring | 5–15 min | Pre-release, performance validation |
| **Drive log scoring** | `score_perf_csv.py` | Field performance vs. SLOs | Seconds | After every real drive |
| **Iron gate** | `iron-gate.sh` | Ship-readiness | ~10 min | Release candidate |

### Minimum Standard: Every Commit

```bash
./scripts/ci-test.sh
```

This runs contract guards + native tests + web build + firmware build. If it passes, the commit is safe to push.

### Recommended: Pre-Release

```bash
./scripts/iron-gate.sh
./scripts/run_real_fw_soak.sh --duration-seconds 900 \
  --metrics-url http://192.168.35.5/api/debug/metrics
python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_<id>.csv \
  --profile drive_wifi_off
```

---

## Related Documentation

- [PERF_SLOS.md](PERF_SLOS.md) — Performance SLO definitions and scoring rules
- [DEVELOPER.md](DEVELOPER.md) — Developer guide, critical rules, testing checklist
- [ARCHITECTURE.md](ARCHITECTURE.md) — System architecture and priority contract
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — Common issues and debugging
- [test/README.md](../test/README.md) — Test suite structure and baselines
- [test/device/README.md](../test/device/README.md) — Device test suite details

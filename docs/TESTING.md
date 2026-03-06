# Testing Suite Documentation

> Status: Active
> Last validated against scripts: March 6, 2026

This document is the source of truth for how we test this repo today.

If scripts change, this file must change in the same commit.

## Table of Contents

1. [Testing Principles](#testing-principles)
2. [Canonical Workflows](#canonical-workflows)
3. [Hardware Cycle Workflow (Authoritative)](#hardware-cycle-workflow-authoritative)
4. [What `device-test.sh` Actually Runs](#what-device-testsh-actually-runs)
5. [Real Firmware Soak (`run_real_fw_soak.sh`)](#real-firmware-soak-run_real_fw_soaksh)
6. [Drive Log Analysis](#drive-log-analysis)
7. [Artifacts and Reports](#artifacts-and-reports)
8. [Release Gate](#release-gate)
9. [Change Control (Keep This Consistent)](#change-control-keep-this-consistent)

## Testing Principles

- Use script-driven testing, not ad-hoc commands, so results are reproducible.
- Treat hardware cycle testing as the behavioral truth for stability/performance regressions.
- Prefer incremental changes with a full hardware verification run after each change set.
- Keep the process documented as it evolves.

## Canonical Workflows

### 1. Pre-push code validation (required)

```bash
./scripts/ci-test.sh
```

This runs contract guards, native tests, web build checks, and firmware build checks.

### 2. Hardware cycle validation (required for stability/perf work)

When firmware/runtime code changed:

```bash
./build.sh --clean --upload --upload-fs
./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct 200
```

When no code changed and device is already uploaded:

```bash
./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct 200
```

### 3. Device suite wrappers (optional, separate from cycle gate)

```bash
./scripts/run_device_tests.sh
./scripts/run_device_tests.sh --quick
./scripts/run_device_tests.sh --full
./scripts/run_device_soak.sh --cycles 20
```

These are still useful, but the stability/performance cycle gate is `device-test.sh`.

## Hardware Cycle Workflow (Authoritative)

This is the process that must remain consistent over time.

### Standard cycle run

```bash
./build.sh --clean --upload --upload-fs
./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct 200
```

### Notes

- `device-test.sh` defaults to `--skip-flash`; cycle workflow relies on explicit upload via `build.sh`.
- Use `--duration-seconds` and `--rad-duration-scale-pct` as primary knobs for longer/shorter qualification windows.
- On code changes, always re-upload before running `device-test.sh`.

### Current cycle baseline knobs

`device-test.sh` currently applies:

- suite profile: `device_v1`
- soak profile: `drive_wifi_ap`
- robust latency mode: `hybrid` (`minSamples=8`, `maxExceedPct=5`, `wifiSkipFirst=2`)
- metrics endpoint retry: `attempts=3`, `delay=1s`
- minima tail exclusion for DMA floors: `3` samples
- transition qualification enabled:
  - flap cycles: `3`
  - interval: `15s`
  - max proxy-off recovery: `30000ms`
  - max samples-to-stable: `6`

### Reference qualification runs (March 6, 2026)

- Cycle 4 pass: [summary.md](/Users/ajmedford/v1g2_simple/.artifacts/test_reports/device_test_20260305_193153/summary.md)
- Cycle 5 pass: [summary.md](/Users/ajmedford/v1g2_simple/.artifacts/test_reports/device_test_20260305_194900/summary.md)

## What `device-test.sh` Actually Runs

Current sequence:

1. Metrics endpoint sanity check (`/api/debug/metrics?soak=1`)
2. Short radar scenario (`RAD-03` by default)
3. Real firmware soak: display stress (`soak_display`)
4. Real firmware soak: core (`soak_core`)
5. Real firmware soak: transition qualification (`soak_transition`)
6. Cross-item uptime continuity check (detect reboot via `uptimeMs` regression)

Outputs include per-item PASS/FAIL plus metrics, then an overall suite result.

## Real Firmware Soak (`run_real_fw_soak.sh`)

Use directly when you need focused soak tests outside `device-test.sh`.

```bash
./scripts/run_real_fw_soak.sh \
  --duration-seconds 900 \
  --metrics-url http://<device-ip>/api/debug/metrics \
  --require-metrics
```

Examples:

```bash
# Display stress
./scripts/run_real_fw_soak.sh --skip-flash \
  --duration-seconds 900 \
  --metrics-url http://<device-ip>/api/debug/metrics \
  --drive-display-preview --display-drive-interval-seconds 6

# Transition flap qualification
./scripts/run_real_fw_soak.sh --skip-flash \
  --duration-seconds 900 \
  --metrics-url http://<device-ip>/api/debug/metrics \
  --drive-transition-flaps --transition-flap-cycles 3
```

Important: the flag is `--drive-transition-flaps` (plural).

## Drive Log Analysis

Drive perf CSVs are produced on SD and can be scored with:

```bash
python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_<id>.csv --profile drive_wifi_ap
python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_<id>.csv --profile drive_wifi_off
```

Teardown caveat: end-of-run WiFi activity (for log pulling) can depress DMA minima. The cycle flow addresses this with tail exclusion for DMA floor gating.

## Artifacts and Reports

Primary output root:

```text
.artifacts/test_reports/
```

Cycle run outputs:

```text
.artifacts/test_reports/device_test_<timestamp>/
  results.tsv
  summary.md
  rad_short_*.log
  soak_display_artifacts/
  soak_core_artifacts/
  soak_transition_artifacts/
```

Soak-only outputs:

```text
.artifacts/test_reports/real_fw_soak_<timestamp>/
```

Device-suite wrapper outputs:

```text
.artifacts/test_reports/device_<timestamp>/
.artifacts/test_reports/device_soak_<timestamp>/
```

## Release Gate

```bash
./scripts/iron-gate.sh
```

Current iron gate points:

1. firmware build
2. SD lock contract
3. parser native smoke
4. device suite (`scripts/device-test.sh`)

## Change Control (Keep This Consistent)

When any of these change, update this document in the same commit:

- script names or command order in the cycle process
- default thresholds/gates in `device-test.sh` or `run_real_fw_soak.sh`
- artifact paths or report filenames
- pass/fail interpretation rules

### Required update checklist

1. Update this file (`docs/TESTING.md`).
2. If behavior contracts changed, update snapshots in `test/contracts/`.
3. Include at least one fresh hardware cycle artifact path in PR/commit notes.
4. Keep examples executable with current script flags.

## Related Docs

- [PERF_SLOS.md](PERF_SLOS.md)
- [DEVELOPER.md](DEVELOPER.md)
- [ARCHITECTURE.md](ARCHITECTURE.md)
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md)
- [test/README.md](../test/README.md)
- [test/device/README.md](../test/device/README.md)

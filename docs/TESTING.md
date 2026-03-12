# Testing

> Status: active
> Last validated against scripts: March 11, 2026

This file documents only the tests and commands that are still trusted today.
If a script changes, this file must change in the same commit.

## What Is Real Today

### Local repo gate

```bash
./scripts/ci-test.sh
```

This is the trusted local/code gate. It runs repo contracts, native tests,
frontend checks, and firmware build checks.

### Hardware qualification

If firmware changed:

```bash
./build.sh --clean --upload --upload-fs
./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct 200
./scripts/iron-gate.sh --skip-flash
```

If firmware did not change and the device already has the intended build:

```bash
./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct 200
./scripts/iron-gate.sh --skip-flash
```

### Drive log scoring

```bash
python3 tools/score_perf_csv.py /path/to/perf.csv --profile drive_wifi_ap
python3 tools/score_perf_csv.py /path/to/perf.csv --profile drive_wifi_off
```

Default bench latency gating now uses the same strict pass/fail model as the
drive scorer. See [PERF_SLOS.md](/Users/ajmedford/v1g2_simple/docs/PERF_SLOS.md)
for the numeric thresholds.

## What `device-test.sh` Really Does

Default qualification runs these items only:

1. `metrics_endpoint`
2. `rad_short`
3. `soak_display`
4. `soak_core`
5. `soak_transition` (unless `--no-transition-soak` is passed)

Default qualification does **not** run:

- `camera_smoke`
- `camera_radar_overlap`

Those paths are not part of release qualification today.

### Qualification behavior

- Metrics preflight must succeed before the suite continues.
- AP unreachable or invalid metrics payload aborts the suite as `INVALID`.
- Default soak latency gating is `strict`.
- Real firmware soaks keep raw peak values in artifacts and summaries.
- Inherited stale panic state is reported, but it is not treated as a new crash
  unless the panic/reset state changes during the soak.

### Trusted qualification command

```bash
./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct 200
```

Typical artifact location:

```text
.artifacts/test_reports/device_test_<timestamp>/
```

Important outputs:

- `summary.md`
- `results.tsv`
- per-item logs and soak artifact directories

## Manual And Exploratory Only

These tools still exist, but they are not qualification today:

- `python3 ./scripts/camera_device_smoke.py`
  - host/browser dependent
  - mutates camera settings during the run
- `./scripts/run_real_fw_soak.sh --latency-gate-mode hybrid`
- `./scripts/run_real_fw_soak.sh --latency-gate-mode robust`

Use them only for manual debugging or exploratory work. Do not treat them as
release evidence.

## What Is Not Covered

- Real bench camera-overlap coverage through the actual camera runtime path
- A trustworthy top-end camera+radar combined qualification gate
- Any claim that headless browser camera smoke proves runtime camera behavior

If one of those areas matters for a release, new real coverage must be added
before claiming it is tested.

## Known Gaps

- Real bench camera-overlap coverage does not currently exist.
- Exploratory stress runs are manual and non-blocking unless explicitly called
  out in the summary for a specific campaign.

## Current Rules

- Reduced but truthful coverage is better than broad fake coverage.
- If a gate cannot be made honest quickly, remove it from qualification instead
  of softening it.
- No default qualification step may depend on host Chrome or mutate persistent
  camera settings.

# Testing

> Status: active
> Last validated against scripts: March 12, 2026

This file documents only the tests the repo currently treats as real.

## What Is Real Today

### Local repo gate

```bash
./scripts/ci-test.sh
```

This is the trusted local/code gate. It runs repo contracts, native tests,
frontend checks, and firmware build checks.

### Drive log scoring

```bash
python3 tools/score_perf_csv.py /path/to/perf.csv --profile drive_wifi_ap
python3 tools/score_perf_csv.py /path/to/perf.csv --profile drive_wifi_off
```

See [PERF_SLOS.md](/Users/ajmedford/v1g2_simple/docs/PERF_SLOS.md) for the
numeric thresholds.

## What Is Not Real Today

There is no trusted hardware qualification command in this repo today.

These tools still exist, but they are exploratory/manual only and are not
release evidence:

- `./scripts/device-test.sh`
- `./scripts/run_real_fw_soak.sh`
- `./scripts/iron-gate.sh`

## Known Gaps

- No trusted bench qualification path
- No trusted bench camera-overlap coverage through the real camera runtime path
- No trusted transition stress gate

## Current Rules

- Reduced but truthful coverage is better than broad fake coverage.
- If a hardware script is exploratory, document it as exploratory.
- Do not claim bench qualification until a real, repeatable hardware gate exists.

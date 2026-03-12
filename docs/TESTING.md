# Testing

> Status: active
> Last validated against scripts: March 12, 2026

This file documents the current release-evidence model.

## Authoritative Gates

### Code gate

```bash
./scripts/ci-test.sh
```

This is the trusted local/code gate. It explicitly runs:

- semantic/unit/integration tests
- the tracked critical mutation catalog
- deterministic perf scorer regression tests
- compatibility guards
- docs hygiene checks
- frontend/build verification

### Hardware gate

```bash
./scripts/qualify_hardware.sh
```

This is the trusted single-board hardware qualification command.

## Authoritative Scoring

Use `tools/score_perf_csv.py` as the only authoritative perf scorer.

When a capture has multiple sessions, pass an explicit session selector for reproducibility:

```bash
python3 tools/score_perf_csv.py /path/to/perf.csv --profile drive_wifi_ap --session longest-connected
python3 tools/score_perf_csv.py /path/to/perf.csv --profile drive_wifi_off --session 1
```

See [PERF_SLOS.md](/Users/ajmedford/v1g2_simple/docs/PERF_SLOS.md) for the
numeric thresholds.

`tools/scorecard.py` remains available as a debug/analysis utility, but it is
not release authority.

## Non-Authoritative Tools

These tools still exist, but they are exploratory/manual only when run directly:

- `./scripts/device-test.sh`
- `./scripts/run_real_fw_soak.sh`
- `./scripts/iron-gate.sh`

## Known Gaps

- No trusted bench camera-overlap coverage through the real camera runtime path
- No trusted transition stress gate

## Current Rules

- Reduced but truthful coverage is better than broad fake coverage.
- If a hardware script is exploratory, document it as exploratory.
- Do not treat reference docs as release authority; use [README.md](/Users/ajmedford/v1g2_simple/docs/README.md) for the authority map.

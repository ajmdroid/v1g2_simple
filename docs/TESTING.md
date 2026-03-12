# Testing

> Status: active
> Last validated against scripts: March 12, 2026

This file is the current testing and validation policy authority for the repo.

## Evidence Lanes

| Lane | Script / Workflow | Budget | Purpose |
|------|-------------------|--------|---------|
| PR | `./scripts/ci-test.sh` | <= 8 min local / <= 12 min CI | Fast merge-safety gate |
| Nightly | `./scripts/ci-nightly.sh` | <= 60 min | Replay, sanitizer, expanded mutation, soak |
| Pre-release | `./scripts/ci-pre-release.sh` | <= 90 min | Full evidence + hardware qualification |
| Trend | `.github/workflows/stability-trend.yml` | N/A | Non-blocking stability analytics |

## Authoritative Gates

### Code gate (PR lane)

```bash
./scripts/ci-test.sh
```

This is the trusted local/code gate. It explicitly runs:

- semantic/unit/integration tests
- the tracked critical mutation catalog
- 4 golden captured-log replay scenarios
- deterministic perf scorer regression tests
- compatibility guards
- docs hygiene checks
- frontend/build verification

### Nightly gate

```bash
./scripts/ci-nightly.sh
```

Runs the PR gate plus:
- full replay corpus
- sanitizer lane (ASan + UBSan) for parser, replay, lockout, camera, volume_fade
- expanded mutation catalog with tier thresholds
- device soak (if hardware available)

### Pre-release gate

```bash
./scripts/ci-pre-release.sh
```

Runs the nightly gate plus:
- hardware qualification on the release board
- replay with perf evidence extraction
- validation manifest generation

### Hardware gate

```bash
./scripts/qualify_hardware.sh
./scripts/qualify_hardware.sh --board-id release
./scripts/qualify_hardware_matrix.sh
```

Single-board or multi-board hardware qualification.

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
- `tools/scorecard.py` — stability trend analysis (nightly only, non-blocking)

## Known Gaps

- No trusted bench camera-overlap coverage through the real camera runtime path
- Replay fixtures are Phase 1 scaffolds — real captured data needed
- No trusted transition stress gate

## Current Rules

- Reduced but truthful coverage is better than broad fake coverage.
- If a hardware script is exploratory, document it as exploratory.
- Do not treat reference docs as release authority; use [README.md](/Users/ajmedford/v1g2_simple/docs/README.md) as the doc index only.

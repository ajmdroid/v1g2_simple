# Observability & Test Evidence Authority

> Status: authoritative  
> Last updated: April 4, 2026

This document is the single authority for runtime observability surfaces, metric
naming, offline derivation, and how test evidence should be interpreted.

## Rule

**measure once → snapshot once → export intentionally → derive once**

## Canonical Runtime Source

The canonical runtime metric store lives in:

- `src/perf_metrics.h`
- `src/perf_metrics.cpp`

Runtime exporters must serialize from the shared runtime metrics snapshot built
by the perf layer. Exporters must not resample the same concept independently.

## Canonical Naming

Internal/canonical metric names use semantic camelCase.

Examples:
- `rxPackets`
- `parseSuccesses`
- `loopMaxUs`
- `dispPipeMaxUs`
- `heapDmaMin`
- `heapDmaLargestMin`

Compatibility aliases are allowed only at the final export boundary.

## Export Surfaces

### Human debug surface
- `GET /api/debug/metrics`
- Context-rich, nested where useful
- Must not duplicate the same concept in both flat and nested form

### Machine soak/debug surface
- `GET /api/debug/metrics?soak=1`
- Flat keys only
- Only fields used for machine parsing, triage, or scoring

### Persisted machine artifact
- `/perf/*.csv`
- Header/row serialization must come from the same shared runtime snapshot
- Legacy CSV header names may remain as compatibility aliases only

## Offline Derivation Authority

Shared offline derivation/schema logic lives in:

- `tools/metric_schema.py`
- `tools/metric_derivation.py`

These modules own:
- canonical derived metric names
- units
- source capability rules
- CSV/source field mappings
- percentile/window helper logic reused by tooling

Consumers include:
- `tools/import_perf_csv.py`
- `tools/import_drive_log.py`
- `tools/soak_parse_metrics.py`
- `scripts/run_real_fw_soak.sh`

## Threshold / Policy Authority

Thresholds and scoring policy live in:

- `tools/hardware_metric_catalog.json`
- `tools/perf_slo_thresholds.json`

Those files define policy. They are not alternate runtime schemas.

## How To Interpret Test Outputs

### `./scripts/ci-test.sh`
- Repo merge-safety / contract / build gate
- Not a substitute for raw runtime evidence by itself

### `./scripts/hardware/test.sh`
- Hardware orchestration wrapper
- Aggregates device suites and real-firmware lanes
- Uses the canonical exported artifacts; it does not define metrics itself

### Raw evidence hierarchy
When outputs disagree, trust in this order:
1. raw serial crash/reset evidence
2. raw on-device perf CSV / emitted soak metrics artifacts
3. visible device behavior
4. summarized comparisons / wrappers

## Logging Policy

Observability is Tier 4.

Keep serial logging only when it provides unique value:
- panic/crash breadcrumbs
- fatal boot/storage failures
- one-shot operator summaries
- explicit admin/manual action results

Do not mirror machine metrics repeatedly on serial when the same truth already
exists in the canonical snapshot or perf CSV.

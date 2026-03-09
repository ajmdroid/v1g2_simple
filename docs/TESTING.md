# Testing Suite Documentation

> Status: Active
> Last validated against scripts: March 9, 2026

This document is the source of truth for how we test this repo today.

If scripts change, this file must change in the same commit.

For a current inventory of failure points covered by the suite, see
[`docs/TEST_FAILURE_POINTS.md`](TEST_FAILURE_POINTS.md).

## Table of Contents

1. [Testing Principles](#testing-principles)
2. [Canonical Workflows](#canonical-workflows)
3. [Hardware Cycle Workflow (Authoritative)](#hardware-cycle-workflow-authoritative)
4. [Bench Stress Ladder (Authoritative)](#bench-stress-ladder-authoritative)
5. [What `device-test.sh` Actually Runs](#what-device-testsh-actually-runs)
6. [Real Firmware Soak (`run_real_fw_soak.sh`)](#real-firmware-soak-run_real_fw_soaksh)
7. [Drive Log Analysis](#drive-log-analysis)
8. [Artifacts and Reports](#artifacts-and-reports)
9. [Release Gate](#release-gate)
10. [Change Control (Keep This Consistent)](#change-control-keep-this-consistent)

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

This runs contract guards, native tests, frontend lint/type checks, frontend
unit tests with coverage, web build checks, and firmware build checks.

When only the Svelte interface changed and you want the narrower frontend gate
before the full repo pass:

```bash
cd interface
npm run lint
npm run test:unit
npm run test:coverage
```

### 2. Hardware cycle validation (required for stability/perf work)

When firmware/runtime code changed:

```bash
./build.sh --clean --upload --upload-fs
./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct 200
./scripts/iron-gate.sh --skip-flash
```

When no code changed and device is already uploaded:

```bash
./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct 200
./scripts/iron-gate.sh --skip-flash
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
./scripts/iron-gate.sh --skip-flash
```

### Notes

- `device-test.sh` defaults to `--skip-flash`; cycle workflow relies on explicit upload via `build.sh`.
- The validated final release gate after a successful upload cycle is `./scripts/iron-gate.sh --skip-flash`.
- Use `./scripts/iron-gate.sh` without `--skip-flash` only when you intentionally want the gate to include an additional flash/bootstrap step.
- Use `--duration-seconds` and `--rad-duration-scale-pct` as primary knobs for longer/shorter qualification windows.
- On code changes, always re-upload before running `device-test.sh`.

### Current cycle baseline knobs

`device-test.sh` currently applies:

- suite profile: `device_v2`
- soak profile: `drive_wifi_ap`
- camera smoke item: settings API round-trip, `/cameras` page render, and debug camera draw on hardware
- robust latency mode: `hybrid` (`minSamples=8`, `maxExceedPct=5`, `wifiSkipFirst=2`)
- metrics endpoint retry: `attempts=3`, `delay=1s`
- harness preflight fail-fast: classed preflight failure (`HARNESS_PRECHECK_*`) aborts remaining items with deterministic suite exit reason
- minima tail exclusion for DMA floors: `3` samples
- transition qualification enabled: flap cycles `3`, interval `15s`, max proxy-off recovery `30000ms`, max samples-to-stable `6`

Camera smoke requires a local Chrome/Chromium binary because the suite verifies
the rendered `/cameras` page with headless DOM capture.

### Reference qualification runs (March 6, 2026)

- Cycle 4 pass: [summary.md](/Users/ajmedford/v1g2_simple/.artifacts/test_reports/device_test_20260305_193153/summary.md)
- Cycle 5 pass: [summary.md](/Users/ajmedford/v1g2_simple/.artifacts/test_reports/device_test_20260305_194900/summary.md)
- Camera runtime cycle pass: [summary.md](/Users/ajmedford/v1g2_simple/.artifacts/test_reports/device_test_20260306_195806/summary.md)
- Camera release gate pass (`--skip-flash`): [summary.md](/Users/ajmedford/v1g2_simple/.artifacts/iron_gate/iron_20260306_201454/summary.md)

## Bench Stress Ladder (Authoritative)

This is the canonical bench stress escalation process for stability/performance qualification.

### Required bench profile (true-to-conditions)

Use this locked profile for every ladder level:

- WiFi AP active (`drive_wifi_ap` context)
- display drive active
- transition flaps active

Use the same physical setup across all levels:

- same power source and cable path
- same device placement/orientation
- same AP client behavior and traffic context

Run invariants:

- no code changes during a ladder campaign
- if code changes: re-upload and restart at `L0`
- fixed soak duration per run: `--duration-seconds 240`

### Stress ramp axes (locked)

Ramp two dimensions together at each level:

- RAD load: `--rad-duration-scale-pct`
- transition stress density: `--transition-drive-interval-seconds` (smaller is harsher)

### Fixed ladder levels

| Level | RAD % | Transition interval (s) |
|---|---:|---:|
| L0 | 200 | 15 |
| L1 | 250 | 12 |
| L2 | 300 | 10 |
| L3 | 350 | 8 |
| L4 | 400 | 7 |
| L5 | 500 | 6 |
| L6 | 650 | 5 |
| L7 | 800 | 4 |
| L8 | 1000 | 3 |

Per-level command template:

```bash
./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct <rad> --transition-drive-interval-seconds <interval>
```

Authoritative automation (recommended for release qualification):

```bash
./scripts/run_bench_ladder.sh --start-level L0 --end-level L8
```

This runner executes the fixed ladder levels, classifies each failure (`A/B/C`)
using `tools/classify_device_test_failure.py`, and enforces the stop policy and
acceptance decision automatically.

Run count rule:

- run each level **2 times**
- level is cleared only if both runs pass (`2/2`)

### Failure classification and action policy (severity-based)

Class A (reliability-critical):

- reboot/panic/reset evidence
- parser/integrity hard failures
- unrecoverable transition stability failures

Action:

- stop ladder immediately
- mark result **not acceptable**

Class B (performance/stability limit):

- latency/churn/recovery gate failures without crash

Action:

- rerun once at same level
- if second fail: stop ladder and record that level as first sustained limit

Class C (harness transient):

- isolated endpoint/no-response transient with immediate clean rerun

Action:

- rerun once
- if rerun passes: continue and log transient
- if rerun fails: reclassify as Class B

### Acceptance decision rules

- minimum acceptable bar: **L6 must be cleared (`2/2`)**
- if first sustained Class B failure is at `L7+` and no Class A before L6: acceptable (optional optimization backlog)
- any Class A before or at L6: not acceptable (reliability improvement required)

### Campaign reporting requirements

For each ladder campaign, record:

- highest cleared level
- first failing level
- failure class (A/B/C and final classification if reclassified)
- artifact paths (run summaries/results)

Append the latest campaign summary path in this doc:

- Latest bench stress ladder campaign summary: [`ladder_summary_validated.md`](/Users/ajmedford/v1g2_simple/.artifacts/test_reports/bench_ladder_20260305_202319/ladder_summary_validated.md)

### Maintenance validation checklist (when this section changes)

Run these checks before declaring the doc update complete:

1. Confirm documented flags still exist:
   - `./scripts/device-test.sh --help` includes `--rad-duration-scale-pct`, `--transition-drive-interval-seconds`, `--duration-seconds`
   - `./build.sh --help` includes `--upload-fs`
2. Dry-run sanity commands:
   - `./scripts/device-test.sh --dry-run --duration-seconds 240 --rad-duration-scale-pct 200 --transition-drive-interval-seconds 15`
   - `./scripts/run_real_fw_soak.sh --dry-run --duration-seconds 240 --transition-drive-interval-seconds 15`
3. Artifact naming/path alignment:
   - verify outputs continue under `.artifacts/test_reports/`
   - verify cycle artifacts still include `device_test_<timestamp>/summary.md` and soak artifact subdirectories documented below

## What `device-test.sh` Actually Runs

Current sequence:

1. Metrics endpoint sanity check (`/api/debug/metrics?soak=1`)
   - if preflight fails (AP unreachable/invalid payload), suite aborts here as a harness-classed failure
2. Camera smoke (`scripts/camera_device_smoke.py`)
3. Short radar scenario (`RAD-03` by default)
4. Real firmware soak: display stress (`soak_display`)
5. Real firmware soak: core (`soak_core`)
6. Real firmware soak: transition qualification (`soak_transition`)
7. Cross-item uptime continuity check (detect reboot via `uptimeMs` regression)

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

### DMA Fragmentation Triage (Heap DMA Floors)

As of March 9, 2026, soak summaries include non-gating DMA fragmentation
diagnostics to make `heapDmaLargestMin` floor failures actionable.

These lines are emitted in `run_real_fw_soak.sh` summaries:

- `DMA largest current below-floor samples/total: <n>/<total> (pct <x>%, longest streak <s>)`
- `DMA largest/free pct min/p05/p50: <min> / <p05> / <p50>`
- `DMA fragmentation pct p50/p95/max: <p50> / <p95> / <max>`
- DMA floor gate failures append an inline triage block:
  - `[DMA triage: currentBelowFloor=<n>/<total> belowFloorPct=<x>% longestStreak=<s> largestToFreePct(p05/p50)=... fragmentationPct(p50/p95)=...]`

`device-test.sh` also surfaces these in per-item metrics:

- `dmaBelowFloor=<n>/<total>`
- `dmaLargestToFreeP50=<value>`
- `dmaFragP95=<value>`

Policy:

- pass/fail thresholds are unchanged (the existing DMA floors still gate)
- diagnostics above are classification aids only

Interpretation guide:

- one-off dip: low `dmaBelowFloor` percent and `longest streak=1` usually indicates transient pressure
- sustained fragmentation: repeated below-floor samples with multi-sample streaks indicate persistent contiguous-block loss
- fragmentation shape: lower `largest/free p50` plus higher `frag p95` indicates the allocator retains free DMA bytes but loses large contiguous blocks

Triage workflow (when DMA floor fails repeatedly):

1. Freeze code and bench setup (no changes between runs).
2. Run repeated L2 stress (example):
   - `./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct 300 --transition-drive-interval-seconds 10`
3. Collect run summaries and compare:
   - `dmaBelowFloor`
   - `longest streak`
   - `dmaLargestToFreeP50`
   - `dmaFragP95`
4. Decide outcome:
   - mostly transient one-off dips: treat as noise/harness sensitivity and continue investigation
   - sustained streaked dips: treat as real fragmentation limit/bug and prioritize memory-layout/runtime mitigation

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

Frontend coverage outputs:

```text
interface/coverage/
  index.html
  lcov.info
```

## Release Gate

```bash
./scripts/iron-gate.sh --skip-flash
```

Current iron gate points:

1. firmware build
2. SD lock contract
3. parser native smoke
4. device suite (`scripts/device-test.sh`)

Notes:

- `iron-gate.sh` now defaults the embedded device suite to the current qualification profile:
  - `--duration-seconds 240`
  - `--rad-duration-scale-pct 200`
- Preferred release usage after `./build.sh --clean --upload --upload-fs` is `--skip-flash`, because that measures runtime stability on the image already loaded on the device.
- Running `iron-gate.sh` without `--skip-flash` is still valid for standalone bootstrap verification, but it intentionally includes a fresh upload inside the device suite.

## Change Control (Keep This Consistent)

When any of these change, update this document in the same commit:

- script names or command order in the cycle process
- default thresholds/gates in `device-test.sh` or `run_real_fw_soak.sh`
- ladder policy or classification behavior in `run_bench_ladder.sh`
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

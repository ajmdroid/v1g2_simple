# Validation Matrix

> Status: active
> Last updated: March 12, 2026

This document maps subsystems to failure modes, required evidence, and the gate
lane that enforces each check.

## Evidence Lanes

| Lane | Script | Budget | Purpose |
|------|--------|--------|---------|
| **PR** | `./scripts/ci-test.sh` | ≤ 8 min local / ≤ 12 min CI | Fast merge safety |
| **Nightly** | `./scripts/ci-nightly.sh` | ≤ 60 min | Replay, sanitizer, expanded mutation, soak |
| **Pre-release** | `./scripts/ci-pre-release.sh` | ≤ 90 min | Full evidence + hardware qualification |
| **Trend** | `.github/workflows/stability-trend.yml` | N/A | Non-blocking stability analytics |

## Subsystem → Evidence Map

### Packet Parser

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Corrupt frame accepted | `test_packet_parser` unit tests | PR | parser |
| Undersized payload passes | Critical mutation `critical-003` | PR | parser |
| Memory safety violation | `native-sanitized` parser suite | Nightly | parser |
| Real-world corrupt input | Replay: `corrupt_packet_rejection` | PR | parser |
| Property: no undersized accept | Property test (10 seeds × 100 cases) | PR | parser |

### Lockout Index

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Boundary alert leaks | Critical mutation `critical-001` | PR | lockout |
| Confidence underflow | Critical mutation `critical-006` | PR | lockout |
| Boundary suppression | Replay: `lockout_boundary_suppression` | PR | lockout |
| Clean-pass demotion | Replay: `lockout_clean_pass_demotion` | PR | lockout |
| Property: confidence never underflows | Property test (10 seeds × 100 cases) | PR | lockout |

### Lockout Learner

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Promotion threshold off-by-one | Critical mutation `critical-004` | PR | lockout |
| Learner promotion flow | Replay: Phase 2 nightly scenario | Nightly | lockout |

### Battery Manager

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Percentage math sign error | Critical mutation `critical-002` | PR | power |

### GPS Lockout Safety

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Guard trips too early | Critical mutation `critical-009` | PR | gps |
| GPS degradation/recovery | Replay: Phase 2 nightly scenario | Nightly | gps |

### Camera Alert

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Forward corridor boundary error | Critical mutation `critical-007` | PR | camera |
| Forward corridor behavior | Replay: `camera_forward_corridor` | PR | camera |
| Memory safety under sanitizer | `native-sanitized` camera suite | Nightly | camera |

### Volume Fade

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Fade delay boundary error | Critical mutation `critical-008` | PR | audio |
| Memory safety under sanitizer | `native-sanitized` volume_fade suite | Nightly | audio |

### WiFi Boot Policy

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| BLE settle boundary slip | Critical mutation `critical-005` | PR | wifi |
| Wi-Fi/runtime coexistence | Replay: Phase 2 nightly scenario | Nightly | wifi |

### Lockout Pre-Quiet

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Entry debounce bypass | Critical mutation `critical-010` | PR | lockout |

### Perf Scoring

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Scorer regression | `test_perf_scoring.py` fixture tests | PR | perf |
| Perf SLO doc/json drift | `check_perf_slo_contract.py` | PR | perf |
| Replay-derived perf evidence | Replay perf extraction | Pre-release | perf |
| Property: pinned session invariance | Property test (10 seeds × 100 cases) | PR | perf |

### Integration Pipeline

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Synthetic drive scenarios | `test_drive_scenario` | PR | integration |
| Captured-log replay (golden) | 4 golden replay scenarios | PR | integration |
| Captured-log replay (full) | 8-12 replay scenarios | Nightly | integration |
| Functional scenarios | `run_functional_tests.sh` | PR | integration |

### Hardware

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Release board qualification | `qualify_hardware.sh --board-id release` | Pre-release | hardware |
| Radio board regression | `run_device_tests.sh --full` | Pre-release | hardware |
| Stress board soak | `run_device_soak.sh --cycles 20` | Pre-release | hardware |
| Device soak stability | `run_device_soak.sh` | Nightly | hardware |

### Firmware Build

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Build failure | `pio run -e waveshare-349` | PR | build |
| Static analysis | `pio-check.sh` | PR | build |
| Flash package size | `report_flash_package_size.py` | PR | build |
| LittleFS build | `pio run -t buildfs` | PR | build |

### Frontend

| Failure Mode | Evidence | Lane | Owner |
|---|---|---|---|
| Lint / type errors | `npm run lint` | PR | frontend |
| Unit test failures | `npm run test:coverage` | PR | frontend |
| Build failure | `npm run build` | PR | frontend |
| Asset budget exceeded | `check_web_asset_budget.py` | PR | frontend |
| Audio manifest drift | `check_audio_asset_manifest.py` | PR | frontend |

## Mutation Testing Tiers

| Tier | Modules | PR Kill Rate | Nightly Kill Rate |
|------|---------|-------------|-------------------|
| **Tier 0** | packet_parser, battery_manager, lockout_index, gps_lockout_safety, camera_alert, volume_fade, wifi_boot_policy, lockout_pre_quiet | 100% (critical catalog) | ≥ 90% (full catalog) |
| **Tier 1** | Other behavior-critical modules | N/A | ≥ 85% (overall) |

## Replay Corpus Phases

| Phase | Scenarios | Lane | Status |
|-------|-----------|------|--------|
| **Phase 1** | 4 golden (lockout boundary, lockout demotion, corrupt packet, camera corridor) | PR | Scaffold committed |
| **Phase 2** | +4 (GPS degradation, WiFi coexistence, learner promotion, perf regression) | Nightly | Planned |
| **Phase 3** | +4 (camera/intersection ambiguity, mixed BLE/GPS edge cases) | Nightly | Planned |

## Waivers

Any surviving Tier 0 mutant in nightly requires either:
- A new test added before merge, or
- A written waiver in `test/mutations/waivers.json`

Approved waivers are included verbatim in the pre-release validation manifest.

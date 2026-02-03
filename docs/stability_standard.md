# V1G2 Simple — Stability Standard

## Overview

This document defines the **Base Stable** contract for the V1G2 Simple firmware. The goal is to ensure the firmware maintains consistent, predictable performance under normal operating conditions, with no unexplained stalls or data loss.

## Test Methodology

### Test Tool
```bash
python soak_test_v1.py --duration 5
```

The soak test runs for 5 minutes, capturing all debug log output including perf metrics emitted every 5 seconds.

### Steady-State Definition
A perf record is considered **steady-state** when:
```
millis >= 60000
```

This allows 60 seconds for boot, WiFi connection, NTP sync, and initial BLE connection to complete.

### Induced Reconnect
During the soak test, one induced reconnect (V1 power cycle or BLE disconnect) is expected. The system must recover gracefully within the reconnect window tolerance.

---

## Invariants (MUST PASS)

These conditions must hold for **all** steady-state perf records:

| Invariant | Requirement |
|-----------|-------------|
| **NDJSON Parse** | 100% of perf records must be valid JSON |
| **qDrop** | Must be 0 (no queue drops) |
| **parseFail** | Must be 0 (no parse failures) |
| **disc** | Must be 0 in steady-state (disconnects only during reconnect window) |

### Exception: `reconn`
- `reconn` may be non-zero if a reconnect was user-induced
- The reconnect should complete successfully within 30 seconds
- After reconnect, all other invariants must hold

---

## SLO Thresholds (Steady-State)

These are the maximum allowed values for performance metrics in steady-state:

| Metric | Threshold | Description |
|--------|-----------|-------------|
| `loopMax_us` | < 500,000 (500ms) | Main loop must not stall for more than 500ms |
| `bleDrainMax_us` | < 200,000 (200ms) | BLE queue drain must complete within 200ms |
| `wifiMax_us` | < 150,000 (150ms) | WiFi processing must complete within 150ms |
| `fsMax_us` | < 50,000 (50ms) | Filesystem operations must complete within 50ms |
| `sdMax_us` | < 50,000 (50ms) | SD card operations must complete within 50ms |

### Reconnect Window Exception
During a reconnect event (indicated by `reconn` incrementing), the following relaxed thresholds apply for up to 30 seconds:
- `loopMax_us` < 500,000 (500ms) — reconnect uses non-blocking step machine
- `bleConnMax_us` may be high but connection is async (does not block loop)
- `bleDiscMax_us` may be high if GATT cache miss (first connect or device change)
- `bleSubsMax_us` is step-limited to ~50ms per step via yield

**NEW (v3.0.8)**: The subscribe phase is now non-blocking with 5ms yields between steps. Discovery uses cached handles when available to skip the blocking `discoverAttributes()` call.

After reconnect completes, steady-state thresholds must be restored.

---

## No Mystery Stalls Requirement

If `loopMax_us` exceeds its threshold, at least one of the following attribution metrics must correlate (be within 90% of `loopMax_us`):

- `bleConnMax_us` — BLE connection duration
- `bleDiscMax_us` — Service discovery duration
- `bleSubsMax_us` — Characteristic subscription duration
- `bleDrainMax_us` — BLE queue drain duration
- `wifiMax_us` — WiFi processing duration
- `fsMax_us` — Filesystem serving duration
- `sdMax_us` — SD card I/O duration
- `flushMax_us` — Display flush duration

If no attribution metric explains the stall, this is a **mystery stall** and the build fails Base Stable.

---

## Scorecard Grades

The `tools/scorecard.py` tool evaluates logs against this standard:

| Grade | Criteria |
|-------|----------|
| **GREEN** | All invariants pass, all SLOs pass, no mystery stalls |
| **YELLOW** | All invariants pass, 1-2 SLO breaches OR reconnect window tolerance used |
| **RED** | Any invariant failure OR >2 SLO breaches OR mystery stalls |

---

## CI Enforcement

The GitHub Actions workflow runs `scorecard.py` against a committed fixture log (`test/fixtures/debug_base_stable.log`). The build fails if:

1. The scorecard grade is not GREEN
2. The scorecard tool itself errors (invalid log format, etc.)

---

## Validation Checklist

Before merging changes that affect performance-sensitive paths:

1. Run soak test for 5 minutes with one induced reconnect:
   ```bash
   python soak_test_v1.py --duration 5
   ```

2. Run scorecard on the resulting log:
   ```bash
   python tools/scorecard.py debug.log
   ```

3. Verify:
   - [ ] Grade is GREEN (or YELLOW with documented reason)
   - [ ] `qDrop=0`, `parseFail=0` in all steady-state records
   - [ ] Reconnect succeeds and subscriptions are active after
   - [ ] No mystery stalls (all `loopMax_us` spikes are attributed)

---

## Troubleshooting

### If scorecard shows RED:

1. **Check invariant failures first** — `qDrop` or `parseFail` non-zero indicates a data path problem
2. **Find the worst perf records** — scorecard shows top 5 worst samples
3. **Check attribution** — which subsystem metric matches `loopMax_us`?
4. **Check for reconnect** — was there a reconnect during the test? Did it complete?

### Common issues:

| Symptom | Likely Cause |
|---------|--------------|
| `loopMax_us` high, `bleConnMax_us` high | BLE connection blocking main loop |
| `loopMax_us` high, `sdMax_us` high | SD card write blocking main loop |
| `qDrop > 0` | BLE packet queue overflow |
| `parseFail > 0` | V1 protocol desync or corruption |

---

## Version History

| Date | Change |
|------|--------|
| 2026-02-03 | Initial stability standard defined |

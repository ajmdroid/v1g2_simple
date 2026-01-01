# V1G2 Simple Latency A/B Test Procedure

## Problem Statement
ENQ→DEQ queue-wait spikes of 84-113ms observed during alert transitions.
SLOW_LOOP events at 85/125/204ms suggest main loop blocking.
DEQ→DRAWDONE is fast (~55us avg), so the issue is before dequeue.

## Test Infrastructure Added
- `include/perf_test_flags.h` - Compile-time flags to disable subsystems
- Percentile histograms (p50/p95/p99/max) for precise latency analysis
- Results printed every 30 seconds

## Test Flags
| Flag | Effect |
|------|--------|
| `PERF_TEST_DISABLE_WIFI` | Skip `wifiManager.process()` |
| `PERF_TEST_DISABLE_TOUCH` | Skip `touchHandler.getTouchPoint()` |
| `PERF_TEST_DISABLE_BATTERY` | Skip `batteryManager.update()` |
| `PERF_TEST_EARLY_DRAIN` | Move `processBLEData()` to top of loop |
| `PERF_TEST_DISABLE_PROXY` | Skip `bleClient.forwardToProxy()` |
| `PERF_TEST_DISABLE_THROTTLE` | Always draw (no DISPLAY_SKIP) |

## Test Procedure

### For Each Test Configuration:
1. Edit `include/perf_test_flags.h` - uncomment ONE flag
2. Build: `pio run`
3. Flash: `pio run -t upload`
4. Monitor: `pio device monitor -b 115200`
5. Run test scenario for 2 minutes:
   - Ensure V1 is connected
   - Generate alerts (drive past radar or use test mode)
   - Wait for alert transitions (alert start → end)
6. Record results from `PERF TEST RESULTS` output

### Results Template
Copy the output block:
```
========== PERF TEST RESULTS ==========
Config: <CONFIG_NAME>
Duration: XX.X sec | Draws: XXX | Skips: XXX | Skip%: XX.X%
ENQ→DEQ (queue_wait): p50=X.XX p95=X.XX p99=X.XX max=X.XX ms
DEQ→DRAWDONE (draw): p50=X.XX p95=X.XX p99=X.XX max=X.XX ms
BLE→DRAWDONE (e2e):  p50=X.XX p95=X.XX p99=X.XX max=X.XX ms
========================================
```

## Results Table

| Config | ENQ→DEQ p95 | ENQ→DEQ max | E2E p99 | E2E max | Skip% |
|--------|-------------|-------------|---------|---------|-------|
| BASELINE | | | | | |
| no_wifi | | | | | |
| no_touch | | | | | |
| no_battery | | | | | |
| early_drain | | | | | |
| no_proxy | | | | | |
| no_throttle | | | | | |

## Analysis Guide

### If `no_wifi` shows major improvement:
→ `server.handleClient()` is blocking (even when Setup Mode inactive?)
→ Fix: Disable WiFi stack when not in Setup Mode

### If `no_touch` shows major improvement:
→ I2C touch polling is blocking
→ Fix: Reduce touch poll frequency or use interrupt-driven touch

### If `early_drain` shows major improvement:
→ Tasks before `processBLEData()` are blocking
→ Fix: Reorder loop or add intermediate queue drains

### If `no_battery` shows major improvement:
→ ADC/I2C battery reads are slow
→ Fix: Reduce battery poll frequency

### If `no_proxy` shows major improvement:
→ BLE proxy forwarding is slow
→ Fix: Move proxy to background task

### If `no_throttle` shows major improvement:
→ Throttle logic itself is problematic
→ Fix: Decouple dequeue from draw cadence

## Recommended Fix Priority
Based on typical ESP32 blocking patterns:
1. WiFi/WebServer - most likely (50-200ms blocks)
2. Touch I2C - second most likely (delays in driver)
3. Early drain - structural fix for any blocker

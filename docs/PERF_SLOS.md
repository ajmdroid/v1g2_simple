# Perf SLOs (CSV Scorecard)

> Status: Active  
> Date: February 15, 2026  
> Source metrics: `/perf/perf_boot_<id>.csv` (schema v6)

## Goal

Define concrete, repeatable SLO gates for runtime health that can be scored directly from perf CSV captures.

## Run Profiles

- `drive_wifi_off`: normal driving test with setup AP not started (default BOOT behavior).
- `drive_wifi_ap`: driving test while setup AP is intentionally active.

## Hard SLOs (Must Pass)

These are release gates unless a test is explicitly exploratory.

| Metric | Rule | `drive_wifi_off` | `drive_wifi_ap` | Why |
|---|---|---:|---:|---|
| `qDrop` | final == | `0` | `0` | Core BLE queue integrity |
| `parseFail` | final == | `0` | `0` | Parser integrity |
| `oversizeDrops` | final == | `0` | `0` | Packet framing safety |
| `bleMutexTimeout` | final == | `0` | `0` | BLE lockup guard |
| `loopMax_us` | max <= | `250000` | `250000` | Loop stall ceiling |
| `bleDrainMax_us` | max <= | `10000` | `10000` | Main-loop BLE drain target (`<10ms`) |
| `bleProcessMax_us` | max <= | `120000` | `120000` | BLE process budget |
| `dispPipeMax_us` | max <= | `120000` | `120000` | Display pipeline budget |
| `flushMax_us` | max <= | `100000` | `100000` | Storage flush budget |
| `sdMax_us` | max <= | `50000` | `50000` | SD write chunk budget |
| `fsMax_us` | max <= | `50000` | `50000` | FS serve budget |
| `queueHighWater` | final <= | `12` | `12` | Queue occupancy margin (`48` depth default) |
| `dmaLargestMin` | min >= | `10000` | `10000` | DMA contiguous block floor |
| `dmaFreeMin` | min >= | `20000` | `20000` | DMA free-memory floor |
| `cameraLoadFailures` | final == | `0` | `0` | Camera loader reliability |
| `cameraBudgetExceeded` | final == | `0` | `0` | Camera scan-cap breach |
| `cameraIndexSwapFailures` | final == | `0` | `0` | Camera index swap safety |
| `cameraMaxTick_us` | max <= | `800` | `800` | Camera tick cap (from camera plan) |
| `cameraMaxWindowHz` | computed <= | `5.05` | `5.05` | Camera cadence cap (5 Hz + epsilon) |
| `wifiConnectDeferred` | final == | `0` | `5` | NVS/WiFi transition pressure |
| `wifiMax_us` | max <= | `1000` | `5000` | WiFi work budget by profile |

## Advisory SLOs (Track/Trend)

These do not fail the run by themselves but should be monitored for regression.

| Metric | Rule | Limit | Why |
|---|---|---:|---|
| `cmdPaceNotYetPerMin` | computed <= | `25` | BLE command pacing pressure |
| `displaySkipPct` | computed <= | `20` | Display throttle ratio (skips / (updates + skips)) |
| `displaySkipsPerMin` | computed <= | `120` | UI draw-throttle pressure |
| `gpsObsDropsPerMin` | computed <= | `200` | GPS observation consumer lag |
| `audioPlayBusyPerMin` | computed <= | `2` | Audio contention signal |
| `reconn` | final <= | `2` | Connection stability trend |
| `disc` | final <= | `2` | Disconnect trend |
| `cameraSkipNonCorePct` | computed <= | `98` | Camera starvation trend |

## Scoring Tool

Use:

```bash
python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1.csv --profile drive_wifi_off
```

JSON output:

```bash
python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1.csv --profile drive_wifi_off --json
```

Exit codes:

- `0`: all hard SLOs pass (advisories pass or warn)
- `1`: hard SLOs pass, one or more advisories fail
- `2`: one or more hard SLOs fail
- `3`: input/tool error

## Notes

- SLOs are intentionally defined against fields already present in schema v6 so scoring is deterministic.
- If queue depth, WiFi runtime model, or camera cadence policy changes, update this file and `tools/score_perf_csv.py` in the same change.

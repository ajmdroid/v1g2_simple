# Boot Sequencing Plan

> Status: Draft  
> Date: February 2026  
> Source data: `perf_boot_2.csv` scored against `drive_wifi_off` profile

## Problem

Five hard SLO failures traced to the first two perf windows (0–12 s):

| Metric | Measured | SLO | Root Cause |
|---|---:|---:|---|
| `loopMax_us` | 369,733 | ≤ 250,000 | BLE connect + discovery cost lands in a single loop iteration |
| `bleProcessMax_us` | 364,288 | ≤ 120,000 | Same — `process()` runs the whole state machine in one call |
| `dmaFreeMin` | 19,692 | ≥ 20,000 | WiFi AP late in session (marginal, but still a fail) |
| `wifiConnectDeferred` | 4 | == 0 | WiFi client connect attempts while drive_wifi_off |
| `wifiMax_us` | 14,904 | ≤ 1,000 | WiFi work happening in a wifi-off profile |

The first two are clearly boot-sequencing issues. The last three are WiFi discipline issues.

## Current Boot Timeline (perf_boot_2.csv)

```
Row 0  (0–7.2 s)  bleConnectMax = 3,004 ms   obdState = SCANNING   loopMax = 370 ms
Row 1  (7.2–12.2) bleDiscovery  = 1,818 ms   bleSub = 376 ms       77 rx, 68 parseOK
Row 2  (12.2–17)  bleProcess    = 116 µs      obdState = DISCONNECTED   stable
Row 3+ (17 s+)    fully settled  ~28 ms loops  obdState = DISCONNECTED   steady state
```

BLE blocks the main loop for **5.2 s cumulative** (connect 3.0 + discovery 1.8 + subscribe 0.4).

## Current setup() Order

| # | Stage | Code | Notes |
|---|---|---|---|
| 1 | Serial + panic dump | `Serial.begin`, `logPanicBreadcrumbs` | |
| 2 | NVS health | `nvsHealthCheck()` | |
| 3 | PSRAM check | `psramFound()` | |
| 4 | Battery latch | `batteryManager.begin()` | **Must stay early — prevents shutdown** |
| 5 | Display init | `display.begin()` | First visual feedback |
| 6 | Settings + time | `settingsManager.begin()`, `timeService.begin()` | Needed before styled screens |
| 7 | Power module | `powerModule.begin()` | |
| 8 | Boot splash | `display.showBootSplash()` | ~100 ms hold |
| 9 | Display preview | `displayPreviewModule.begin()` | |
| 10 | Storage (SD/FS) | `storageManager.begin()`, profiles, audio, settings restore | **Slow — SD mount + JSON** |
| 11 | Perf loggers | `perfSdLogger.begin()`, signal logger | |
| 12 | Lockout zones | JSON load from SD (up to 64 KB) | **Slow — JSON deserialization** |
| 13 | **BLE init** | `bleClient.initBLE()` | NimBLE stack startup |
| 14 | **BLE scan start** | `bleClient.begin()`, callbacks | Scan starts immediately (10 s window) |
| 15 | Auto-push, Touch UI | Module `begin()` calls | |
| 16 | Touch handler | `touchHandler.begin()` | I2C init |
| 17 | Brightness / volume | Apply saved settings | |
| 18 | Core pipeline | Alert, voice, display, BLE queue, OBD, GPS, lockout | All module `begin()` |
| 19 | **Boot ready gate** | `bootReady = true`, `bleClient.setBootReady(true)` | Unblocks BLE state machine |
| 20 | WiFi (if enabled) | `getWifiOrchestrator().startWifi()` | Only if `enableWifiAtBoot` |

### Key Observations

1. **BLE scan starts at step 14** but the state machine is gated on `bootReady` (step 19). Scan runs during steps 15–18, so V1 is often found before the gate opens. Good — discovery overlaps with module init.

2. **OBD `begin()` at step 18** creates its task and sets state to `IDLE`. The actual auto-connect fires 1.5 s after `onV1Connected` in `loop()` — that's correct, but the scan (step 14) already overlaps with it in Row 0.

3. **WiFi at step 20** is conditional, but the perf CSV shows `wifiConnectDeferred = 4` and `wifiMax_us = 14,904` even in a `drive_wifi_off` run. This suggests WiFi work leaks in from somewhere (likely startup mode check or AP reconnect logic).

4. **All BLE blocking happens in `loop()`** via `bleClient.process()`. The connect phase is async (NimBLE callback), discovery runs on a FreeRTOS task, and subscribe uses a yield machine — but the *polling* in `process()` still clips the caller's loop iteration.

## Proposed Boot Sequence

### Design Principles

1. **Priority stack**: V1 connectivity > BLE ingest > Display > Audio > Metrics > WiFi
2. **Never block the main loop for > 120 ms** (bleProcessMax SLO)
3. **Settle before escalate**: each radio consumer waits for the prior one to stabilize
4. **WiFi is opt-in and deferred**: if enabled, it waits for V1 + OBD to settle

### Phase 0 — Hardware Foundation (unchanged)

Steps 1–8 of current boot. No changes needed.

- Serial, panic breadcrumbs, NVS health
- Battery latch (must stay first — prevents power-off)
- Display init + boot splash
- Settings + time + power module

These are fast (~200 ms total) and order-constrained. Battery latch MUST precede any long init.

### Phase 1 — Storage & Config Load

Steps 9–12 of current boot. No changes needed to order.

- SD mount, profiles, audio init, settings restore
- Perf/signal loggers
- Lockout zone load (JSON)

These run before BLE and are purely local I/O. Total cost: ~300–600 ms depending on SD and zone count.

**Possible optimization** (not required for SLO compliance): Defer lockout zone load to a background task after Phase 2 starts. Zones are only needed when GPS fix + lockout enforcer runs, which is 30+ seconds into a drive. This could shave 100–300 ms off time-to-BLE-scan.

### Phase 2 — BLE Start + Scan

Steps 13–14 of current boot. **Move earlier if possible.**

- `initBLE()` + register callbacks + `begin()` (starts scan)
- Scan runs in background (10 s window, stops early when V1 found)

**Change: Move BLE init before storage** (swap Phase 1 and Phase 2). The NimBLE stack needs only NVS (already healthy from step 2) and settings (already loaded in step 6). Storage/SD mount and lockout JSON are not BLE dependencies.

This buys ~300–600 ms earlier scan start, which means the V1 is more likely to be found by the time the boot-ready gate opens.

### Phase 3 — Module Init (Pipeline Setup)

Steps 15–18. These all run after scan has started and take ~50–100 ms total.

- Auto-push, touch UI, tap gesture
- Touch handler (I2C)
- Brightness, volume apply
- Alert persistence, voice, display pipeline, BLE queue, connection state
- OBD handler `begin()` (creates task, loads remembered devices, state = IDLE)
- GPS, lockout enforcer/learner

No changes needed. These are fast and don't touch the radio.

### Phase 4 — Boot-Ready Gate

Step 19. `bootReady = true`, `bleClient.setBootReady(true)`.

**This is the critical moment.** When the gate opens, `bleClient.process()` will start executing the BLE state machine. If the V1 was found during the scan overlap (Phase 2–3), it will immediately begin:

1. `SCAN_STOPPING` → settle (200 ms on first boot)
2. `CONNECTING` → async connect (callback-driven, ~1–3 s)
3. `CONNECTING_WAIT` → poll for callback
4. `DISCOVERING` → spawns FreeRTOS task (~1.8 s)
5. `SUBSCRIBING` → step machine with 5 ms yields
6. `CONNECTED` → packets flowing

### Phase 5 — V1 Settle Window (NEW)

**New concept: V1 settle gate.**

After BLE reaches `CONNECTED`, the `onV1Connected` callback fires. Current code sets `obdAutoConnectAtMs = millis() + 1500` — a 1.5 s timer before OBD auto-connect. This is already the right idea, but the settle window should also gate WiFi.

**Proposed OBD gate (already exists):**
- OBD `tryAutoConnect()` fires 1.5 s after `onV1Connected`
- `tryAutoConnect()` checks `isLinkReady()` — V1 must be connected
- If V1 disconnects during the window, OBD is canceled

**Proposed WiFi gate (new):**
- WiFi auto-start (if `enableWifiAtBoot`) should wait until **after** V1 is connected AND a configurable settle timer expires (e.g., 5 s post V1-connect)
- If V1 never connects (30 s timeout), WiFi can start anyway (user may want web UI to diagnose)
- This prevents WiFi from competing with BLE during the critical connection window

### Phase 6 — WiFi (Deferred, Conditional)

Only if `enableWifiAtBoot` is true AND settle conditions are met.

**Current issue:** WiFi starts in `setup()` immediately if `enableWifiAtBoot` is set. This means WiFi AP comes up while BLE is still connecting/discovering. Even though `WiFi.setTxPower(WIFI_POWER_5dBm)` mitigates RF contention, the NVS/ESP-IDF WiFi init itself consumes DMA heap and CPU.

**Change:** Move WiFi auto-start out of `setup()` into `loop()` with a gate:

```
WiFi starts when ALL of:
  1. enableWifiAtBoot == true
  2. BLE is CONNECTED (or 30 s timeout)
  3. At least 3 s have elapsed since V1 connected (or timeout)
  4. canStartSetupMode() passes (DMA heap check)
```

## What Changes vs. What Stays

### Changes Required

| Change | Where | Risk | Impact |
|---|---|---|---|
| Move BLE init before SD/storage | `setup()` reorder | Low | Scan starts 300–600 ms earlier |
| WiFi auto-start moves to `loop()` | `setup()` + `loop()` | Low | WiFi no longer competes with BLE connect |
| WiFi gated on V1 settle | New logic in `loop()` | Low | Prevents WiFi SLO failures in wifi-off profile |
| WiFi stray work audit | `wifi_manager.cpp` | Medium | Must find why `wifiMax_us = 14,904` in wifi-off |

### No Changes Required

| Component | Why |
|---|---|
| Battery latch order | Already first — correct |
| Display init order | Must provide visual feedback early — correct |
| BLE state machine | Already async (connect via callback, discovery via task, subscribe via yield) |
| OBD auto-connect timing | 1.5 s post-V1-connect is appropriate |
| OBD `isLinkReady()` guard | Already prevents OBD from racing V1 |
| `bootReady` gate | Already prevents state machine from running during setup |
| Module `begin()` ordering | No inter-dependencies that conflict |

## SLO Impact Analysis

### `loopMax_us` (369,733 → target ≤ 250,000)

**Root cause**: BLE connect/discovery cost in `process()` exceeding 250 ms in a single call.

**Boot sequencing alone won't fix this.** The blocking is inherent to the BLE state machine polling model. The connect phase is already async, but `processConnectingWait()` and `processDiscovering()` poll on each `loop()` iteration. The actual BLE work happens in NimBLE callbacks and a FreeRTOS task — the main loop just checks flags.

The 370 ms `loopMax` in Row 0 includes *all* loop work in that 5 s window, not just BLE. The perf CSV `bleProcessMax_us` column captures the `process()` call duration, and at 364 ms, it's the dominant cost.

**Root fix needed**: The perf window (5 s) captures the max across all iterations. The BLE connect callback fires asynchronously — the main loop doesn't actually block for 3 s. But the *first loop iteration* after boot-ready gate opens may overlap with the scan-stop settle + connect initiation, which can be expensive.

**Sequencing mitigation**: Moving BLE scan earlier means the V1 is likely already found when the gate opens. The scan-stop + settle occurs on the first `process()` call, which currently takes ~200 ms (first boot settle). If we can ensure the scan has already stopped and settled by the time the gate opens, the first `process()` call goes straight to `connectToServer()` which is cheap (just initiates async connect).

**Recommendation**: After opening the boot-ready gate, call `process()` once in setup before entering `loop()`. This absorbs the scan-stop + settle cost in setup time rather than the first loop iteration. Since discovery runs on a task and connect is async, subsequent `process()` calls will be microsecond-cheap polls.

### `bleProcessMax_us` (364,288 → target ≤ 120,000)

Same root cause as `loopMax_us`. Same fix path.

### `dmaFreeMin` (19,692 → target ≥ 20,000)

This fails at Row 384 (1,930 s into session), not at boot. The 308-byte breach is marginal and not boot-related. Likely caused by WiFi or a long-lived allocation late in the session.

**Boot sequencing impact**: None directly, but deferring WiFi startup reduces peak DMA pressure during the critical boot window.

### `wifiConnectDeferred` (4 → target == 0 in wifi-off)

WiFi client connect attempts are happening even though this is a `drive_wifi_off` run. Something is triggering `connectToNetwork()` calls.

**Boot sequencing fix**: Ensure WiFi code paths are completely gated when WiFi is not enabled. Audit `wifi_manager.cpp` `process()` loop for stray work when `!isSetupModeActive()`.

### `wifiMax_us` (14,904 → target ≤ 1,000 in wifi-off)

Same root cause as `wifiConnectDeferred`. WiFi work shouldn't be happening at all.

**Boot sequencing fix**: Audit `wifiManager.process()` early return when WiFi is off. The call in `loop()` is already gated on `!skipNonCoreThisLoop`, but there's no gate on WiFi being disabled/off.

## Implementation Plan

### Step 1: Reorder BLE init before storage (low risk)

Move steps 13–14 (BLE initBLE + begin) to before step 10 (storageManager.begin). Verify BLE has no dependencies on SD/storage.

BLE dependencies:
- NVS (ready from step 2) ✓
- Settings (ready from step 6) ✓
- No storage/SD dependency ✓

### Step 2: WiFi auto-start deferral (low risk)

Remove the `if (enableWifiAtBoot) startWifi()` block from end of `setup()`. Add a WiFi auto-start gate in `loop()`:

```cpp
// WiFi auto-start gate (only when enableWifiAtBoot is set)
static bool wifiAutoStartDone = false;
if (!wifiAutoStartDone && settingsManager.get().enableWifiAtBoot) {
    bool bleSettled = bleClient.isConnected() && (now - onV1ConnectedTimestamp) >= 3000;
    bool timeout = now >= 30000;
    if (bleSettled || timeout) {
        getWifiOrchestrator().startWifi();
        wifiAutoStartDone = true;
    }
}
```

### Step 3: WiFi stray work audit (medium risk)

Investigate why `wifiMax_us = 14,904` and `wifiConnectDeferred = 4` occur in a wifi-off profile. Likely candidates:
- `wifiManager.process()` doing work even when AP is off
- Stale WiFi client reconnect logic firing from a previous session
- WiFi scanning / network discovery that runs unconditionally

### Step 4: Validate with perf CSV capture

Re-run `score_perf_csv.py --profile drive_wifi_off` and verify all 5 failures are resolved.

## Sequencing Constraints (Do Not Violate)

These are invariants discovered during analysis. Future changes must preserve them:

1. **Battery latch must be the first hardware init** — prevents power-off during boot on battery.
2. **Display init must precede any delay > 100 ms** — user needs visual feedback that boot is happening.
3. **Settings must load before BLE init** — NimBLE needs proxy name and enabled flag.
4. **NVS health check must precede any NVS consumer** — BLE, settings, OBD all use NVS.
5. **BLE scan must start before module init** — overlapping scan with setup is a free optimization.
6. **Boot-ready gate must open after all `begin()` calls** — modules must be initialized before they receive events.
7. **OBD must not touch BLE radio until V1 is connected** — `isLinkReady()` guard.
8. **WiFi must not start until V1 is connected or timeout** — RF coexistence.
9. **Never delete active NimBLE clients at runtime** — disconnect and reuse (from CLAUDE.md).
10. **Keep BLE callbacks lightweight** — queue/copy only, no SPI or heavy work (from CLAUDE.md).

## Expected Outcome

| SLO | Before | After (Expected) | How |
|---|---:|---:|---|
| `loopMax_us` | 369,733 | < 250,000 | BLE scan settle absorbed in setup; first `process()` in loop is a cheap poll |
| `bleProcessMax_us` | 364,288 | < 120,000 | Same mechanism |
| `dmaFreeMin` | 19,692 | ≥ 20,000 | WiFi deferral reduces peak DMA; marginal fix |
| `wifiConnectDeferred` | 4 | 0 | WiFi stray work audit eliminates phantom connects |
| `wifiMax_us` | 14,904 | < 1,000 | Same audit |

## Open Questions

1. **Can we call `process()` in setup after opening the boot-ready gate?** This would absorb the scan-stop + settle cost. Risk: if the scan hasn't found V1 yet, `process()` will just start a new scan and return quickly (microseconds). Benefit: if V1 was found, the settle happens in setup rather than the first loop iteration. **Verdict: safe and beneficial.**

2. **Should lockout zone loading be deferred?** Currently takes ~100–300 ms for large zone files. Zones aren't needed until GPS fix + lockout enforcer runs (~30 s into drive). Deferring would let BLE scan start even earlier. **Verdict: nice-to-have, not needed for SLO compliance.**

3. **What causes wifiConnectDeferred in wifi-off mode?** Need to trace the call path. Possibly a saved network credential triggering reconnect logic in `wifi_manager.cpp` `processClientState()`. **Verdict: must investigate before Step 3 implementation.**

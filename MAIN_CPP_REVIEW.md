# main.cpp Optimization Review

**Review Date:** January 29, 2026  
**Scope:** Main application flow, module interactions, and potential optimizations  
**Status:** Analysis only - no code changes

---

## 1. Executive Summary

The `main.cpp` file (616 lines) orchestrates 18+ modules through `setup()` and `loop()`. The modular architecture is well-designed, but several optimization opportunities exist around:

- **Loop execution order** - some modules called out of logical sequence
- **Redundant state checks** - multiple modules checking same conditions
- **Unused variables** - legacy globals that could be removed
- **Conditional processing** - modules running when feature is disabled
- **Display update throttling** - split across multiple locations

---

## 2. Architecture Overview

### 2.1 Module Instantiation Order (Globals)

```
bleClient, parser, display, touchHandler, gpsHandler, lockouts, autoLockouts
alertPersistenceModule, voiceModule, displayPreviewModule
volumeFadeModule, speedVolumeModule, cameraAlertModule
autoPushModule, touchUiModule, tapGestureModule, powerModule
perfReporterModule, bleQueueModule, connectionStateModule
displayPipelineModule, cameraLoadCoordinator, obdAutoConnector
autoLockoutMaintenance, displayRestoreModule
```

### 2.2 Loop Call Order (Annotated)

| Line | Call | Category | Notes |
|------|------|----------|-------|
| 510 | `alertPersistenceModule.update()` | Display | **Empty impl** - no-op |
| 511 | `perfReporterModule.process(now)` | Diagnostics | Rate-limited (60s) |
| 516 | `display.setBLEProxyStatus(...)` | Display | **Called every loop** |
| 519 | `audio_process_amp_timeout()` | Audio | Lightweight |
| 522-527 | `displayPreviewModule` | Display | Conditional |
| 532 | `powerModule.process(now)` | Power | Rate-limited internally |
| 533-535 | `touchUiModule.process(now, ...)` | Input | Returns early if active |
| 538 | `tapGestureModule.process(now)` | Input | |
| 541-543 | `bleClient.process()` | BLE | Critical path |
| 546 | `bleQueueModule.process()` | BLE | Critical path |
| 549 | `autoPushModule.process()` | BLE | State machine |
| 552 | `wifiManager.process()` | WiFi | |
| 555-563 | `gpsHandler.update()` | GPS | Conditional on enabled |
| 566 | `cameraAlertModule.process()` | Camera | Heavy when active |
| 569 | `obdAutoConnector.process(now)` | OBD | Lightweight |
| 572 | `cameraLoadCoordinator.process(...)` | Camera | One-shot |
| 575-578 | `cameraAlertModule.updateMainDisplay(...)` | Camera | |
| 581 | `speedVolumeModule.process(now)` | Audio | Rate-limited |
| 584 | `autoLockoutMaintenance.process(now)` | Lockout | Rate-limited (30s) |
| 590-594 | `connectionStateModule.process(now)` | BLE/Display | **Inside 50ms gate** |
| 598-606 | Serial status print | Debug | DEBUG_LOGS guard |
| 609 | `debugLogger.update()` | Diagnostics | Buffer flush |
| 612 | `vTaskDelay(pdMS_TO_TICKS(1))` | System | Yield |

---

## 3. Optimization Opportunities

### 3.1 HIGH PRIORITY - Structural Issues

#### 3.1.1 `alertPersistenceModule.update()` is a No-Op
**Location:** [main.cpp](main.cpp#L510)  
**Issue:** Called every loop but implementation is empty:
```cpp
void AlertPersistenceModule::update() {
    if (!initialized) return;
    // Future: could handle periodic tasks here
}
```
**Recommendation:** Remove the call until the module actually needs periodic processing.

#### 3.1.2 `display.setBLEProxyStatus()` Called Every Loop
**Location:** [main.cpp](main.cpp#L514-L517)  
**Issue:** This call happens unconditionally every loop iteration, even when nothing changed:
```cpp
unsigned long lastRx = bleQueueModule.getLastRxMillis();
bool bleReceiving = (now - lastRx) < 2000;
display.setBLEProxyStatus(bleClient.isConnected(), bleClient.isProxyClientConnected(), bleReceiving);
```
**Recommendation:** Add change detection - only call when values differ from last call.

#### 3.1.3 `connectionStateModule.process()` Inside 50ms Gate
**Location:** [main.cpp](main.cpp#L587-L594)  
**Issue:** Connection state handling is inside the `DISPLAY_UPDATE_MS` (50ms) gate:
```cpp
if (now - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
    lastDisplayUpdate = now;
    if (!displayPreviewModule.isRunning()) {
        connectionStateModule.process(now);
    }
}
```
**Impact:** On V1 disconnect, the state transition could be delayed up to 50ms. The module handles:
- Power module connection change notification
- Display state reset (scanning/resting)
- Parser state reset
- Stale data re-request logic

**Recommendation:** Move `connectionStateModule.process()` outside the time gate for immediate state handling, or at minimum, check connection transitions separately.

#### 3.1.4 Redundant `displayPreviewModule.isRunning()` Checks
**Location:** [main.cpp](main.cpp#L522), [main.cpp](main.cpp#L575), [main.cpp](main.cpp#L591)  
**Issue:** `displayPreviewModule.isRunning()` is checked 3 times in the loop:
1. Line 522: To decide whether to update preview
2. Line 575: To decide whether to update camera main display
3. Line 591: To decide whether to process connection state

**Recommendation:** Check once at loop start and store in local variable.

### 3.2 MEDIUM PRIORITY - Efficiency Improvements

#### 3.2.1 Loop Order Could Be Optimized
**Current order has some inefficiencies:**

1. `alertPersistenceModule.update()` runs before BLE queue processing, but persistence depends on parsed data
2. `speedVolumeModule.process()` runs after camera processing, but volume decisions could inform earlier code
3. `connectionStateModule.process()` runs last (inside gate) but handles critical connect/disconnect transitions

**Recommended order for critical path:**
```
1. bleClient.process()         // Handle BLE events
2. bleQueueModule.process()    // Parse incoming data
3. connectionStateModule.process()  // Handle state transitions
4. displayPipelineModule      // (called from bleQueueModule)
5. ... other modules ...
```

#### 3.2.2 GPS Module Detection Runs Every Loop
**Location:** [main.cpp](main.cpp#L555-L563)  
**Issue:** When GPS is enabled, detection check runs every iteration:
```cpp
if (gpsHandler.isEnabled()) {
    gpsHandler.update();
    if (gpsHandler.isDetectionComplete() && !gpsHandler.isModuleDetected()) {
        // disable GPS
    }
}
```
**Recommendation:** Once detection is complete (either way), stop checking. The `isDetectionComplete()` check continues running even after the module is confirmed working.

#### 3.2.3 Camera Alert Module Called Even When Disabled
**Location:** [main.cpp](main.cpp#L566)  
**Issue:** `cameraAlertModule.process()` is called unconditionally. The module internally checks if feature is disabled, but there's function call overhead.
**Recommendation:** Gate with `settingsManager.get().cameraAlertsEnabled` in main.cpp.

### 3.3 LOW PRIORITY - Code Cleanup

#### 3.3.1 Unused Global Variables
**Location:** [main.cpp](main.cpp#L89-L91)
```cpp
unsigned long lastDisplayUpdate = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastLvTick = 0;
```
- `lastDisplayUpdate` - Used ✓
- `lastStatusUpdate` - Used ✓  
- `lastLvTick` - **Never used after initial assignment** at line 278

**Recommendation:** Remove `lastLvTick`.

#### 3.3.2 Legacy Comment References
**Location:** [main.cpp](main.cpp#L137-L145)
```cpp
// Smart threat escalation tracking moved to VoiceModule
// Helper moved to VoiceModule: getAlertBars(), isBandEnabledForSecondary(), speed helpers
// WiFi manual startup - user must long-press BOOT to start AP
// Alert persistence handled by AlertPersistenceModule
```
**Recommendation:** These migration comments could be removed now that the codebase is stable.

#### 3.3.3 Inline Static Orchestrator
**Location:** [main.cpp](main.cpp#L153-L168)  
The `getWifiOrchestrator()` function creates a static WifiOrchestrator with a lambda. This is fine but could be simplified to a global like other modules.

### 3.4 TIMING CONSIDERATIONS

#### 3.4.1 Multiple Rate-Limited Modules
Several modules have internal rate limiting:
- `perfReporterModule.process()` - 60 second interval
- `autoLockoutMaintenance.process()` - 30 second update, 5 minute save
- `connectionStateModule` - Uses `DATA_STALE_MS` (2s), `DATA_REQUEST_INTERVAL_MS` (3s)
- `display_pipeline_module` - 50ms `DISPLAY_DRAW_MIN_MS`
- `cameraAlertModule` - 300ms `CAMERA_CHECK_INTERVAL_MS`

**Observation:** Rate limiting is scattered across modules. Consider a unified timing system or document the timing hierarchy.

#### 3.4.2 FreeRTOS Yield Strategy
**Location:** [main.cpp](main.cpp#L612)
```cpp
vTaskDelay(pdMS_TO_TICKS(1));
```
**Analysis:** 1ms delay gives ~1000 Hz max loop rate. This is appropriate for the application but worth documenting. The BLE queue processes packets at the rate they arrive, and display updates are throttled to 20 Hz (50ms).

---

## 4. Module Dependency Analysis

### 4.1 Initialization Dependencies (setup)
```
Storage → Settings → ProfileManager → AutoPush
Storage → Lockouts, AutoLockouts
Settings → GPS → CameraManager
Settings → OBD
Settings → Display (brightness)
Touch → TouchUiModule, TapGestureModule
BLE → bleClient.onDataReceived(bleQueueModule.onNotify)
BLE → bleClient.onV1Connected(onV1Connected)
```

### 4.2 Runtime Dependencies (loop)

```
BleQueueModule.process() → DisplayPipelineModule.handleParsed()
DisplayPipelineModule → VoiceModule, VolumeFadeModule, SpeedVolumeModule
ConnectionStateModule → PowerModule.onV1ConnectionChange()
CameraAlertModule → GpsHandler, CameraManager
SpeedVolumeModule → VoiceModule.getCurrentSpeedMph()
```

### 4.3 Circular Dependency Concern
`SpeedVolumeModule` depends on `VoiceModule.getCurrentSpeedMph()`, while `VoiceModule` is called from `DisplayPipelineModule`. This creates a logical timing dependency where speed must be current before voice decisions.

---

## 5. Memory Considerations

### 5.1 Stack Usage
The loop function has relatively few local variables:
- `now` (unsigned long)
- `lastRx` (unsigned long) 
- `bleReceiving` (bool)
- `previewActive` (bool)
- `v1HasActiveAlerts` (bool)

Most heavy data is in module member variables or parser static storage.

### 5.2 Heap Fragmentation
Global instantiation of all modules avoids heap fragmentation. Only `WifiOrchestrator` uses function-local static, which is still safe.

---

## 6. Recommended Changes Summary

### Immediate (Low Risk)
1. Remove `alertPersistenceModule.update()` call (no-op)
2. Remove unused `lastLvTick` variable
3. Cache `displayPreviewModule.isRunning()` in loop-local variable

### Short-Term (Medium Risk)
4. Add change detection to `display.setBLEProxyStatus()` 
5. Move `connectionStateModule.process()` outside 50ms gate
6. Gate `cameraAlertModule.process()` with settings check

### Consider (Architectural)
7. Reorder loop for optimal critical path
8. Add one-shot flag for GPS detection complete
9. Document timing hierarchy across modules

---

## 7. Testing Recommendations

Before implementing any changes:
1. **Baseline performance** - Log loop iteration times
2. **BLE latency** - Measure packet-to-display time
3. **Connection state** - Test V1 disconnect/reconnect behavior
4. **Camera alerts** - Verify no missed detections with gating

---

## 8. Appendix: Module Process Function Summary

| Module | Internal Rate Limit | Early Exit Conditions |
|--------|---------------------|----------------------|
| alertPersistenceModule | None | `!initialized` |
| perfReporterModule | 60s | `!logPerfMetrics` |
| powerModule | Internal auto-off timer | Platform check |
| touchUiModule | TAP_DEBOUNCE_MS | Returns true = skip loop |
| tapGestureModule | TAP_DEBOUNCE_MS, TAP_WINDOW_MS | Missing deps |
| bleQueueModule | Queue-driven | Empty queue |
| autoPushModule | State machine | `Step::Idle` |
| wifiManager | AsyncWebServer | None visible |
| gpsHandler | Serial parsing | `!enabled` |
| cameraAlertModule | 300ms | `!cameraAlertsEnabled` |
| obdAutoConnector | Timer-based | `!obd` |
| cameraLoadCoordinator | One-shot | `!pending \|\| complete` |
| speedVolumeModule | Internal | `!enabled \|\| !bleConnected` |
| autoLockoutMaintenance | 30s update, 5min save | `!autoLockouts` |
| connectionStateModule | 2s stale, 3s request | `!ble \|\| !parser` |

---

*Review conducted by analyzing main.cpp and all 18+ module implementations.*

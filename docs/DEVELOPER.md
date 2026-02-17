# V1-Simple Developer Guide

Technical documentation for developers working on the V1-Simple codebase.

---

## Table of Contents

1. [Critical Rules](#critical-rules)
2. [Common Bugs & Prevention](#common-bugs--prevention)
3. [Display System](#display-system)
4. [Camera Module](#camera-module)
5. [BLE Architecture](#ble-architecture)
6. [Testing Checklist](#testing-checklist)

---

## Critical Rules

These rules are **non-negotiable**. Violating them causes crashes or data corruption.

### 1. Never Delete BLE Clients at Runtime

```cpp
// ❌ WRONG - Causes heap corruption
if (pClient) {
    NimBLEDevice::deleteClient(pClient);  // CRASH!
    pClient = nullptr;
}

// ✅ CORRECT - Reuse the client
if (pClient) {
    if (pClient->isConnected()) {
        pClient->disconnect();
    }
    pClient->connect(address);  // Reuse existing client
}
```

**Why**: NimBLE maintains internal state. Deleting clients at runtime corrupts the heap.

### 2. Single-Threaded Display

```cpp
// ❌ WRONG - Called from BLE callback
void notifyCallback(NimBLECharacteristic* pChar) {
    display.updateAlert(data);  // CRASH - wrong thread!
}

// ✅ CORRECT - Queue and process in main loop
void notifyCallback(NimBLECharacteristic* pChar) {
    xQueueSend(bleDataQueue, &data, 0);  // Queue only
}

void loop() {
    if (xQueueReceive(bleDataQueue, &data, 0)) {
        display.updateAlert(data);  // Safe - main thread
    }
}
```

**Why**: Arduino_GFX is not thread-safe. Display calls from callbacks cause corruption.

### 3. Battery Latch Timing

```cpp
void setup() {
    // batteryManager.begin() MUST be called within ~300ms of power-on
    // or the device will shut down when user releases power button
    batteryManager.begin();  // First thing after Serial.begin()
    
    // ... rest of setup ...
}
```

### 4. Radio Contention

The ESP32 has a single shared radio for WiFi and BLE. Rules:

- BLE scan duty cycle is currently tuned to 75% (`interval=160`, `window=120`) for reliable V1 discovery on ESP32-S3
- OBD auto-connect is deferred by 1.5s after V1 connect to avoid competing with initial V1 stabilization
- WiFi scans are non-blocking but still contend with BLE airtime

---

## Runtime Priority Contract

Main-loop execution follows this priority order:

1. V1 connectivity
2. BLE ingest/drain
3. Display updates
4. Audio alerts (best-effort)
5. Metrics collection
6. WiFi / Web UI (off by default)
7. Logging / persistence (best-effort)

Implementation notes:

- Core BLE + display paths are kept non-blocking in steady state.
- Tier-7 persistence in `loop()` is intentionally bounded by dirty flags and coarse rate limits.
- SD writes use `SDTryLock` on Core 1 (skip/defer on contention); LittleFS fallback writes are synchronous but bounded and infrequent.
- BLE recovery paths include short settle delays in failure handling (`hardResetBLEClient`) due to NimBLE state-transition constraints.

---

## Logging Strategy

The codebase uses two active logging mechanisms:

### Conventions (Apply Incrementally)

For new or touched code:
- Use stable uppercase prefixes: `[AREA]` (example: `[BLE]`, `[WIFI]`, `[PERF]`).
- Include level keywords (`ERROR`, `WARN`) for failure paths when applicable.
- Keep subsystem prefix spellings consistent; do not introduce alternates.
- For compatibility routes/paths, log when legacy behavior is exercised.
- In hot paths, keep logs debug-gated; keep actionable failures always visible.

### Serial/SerialLog (Direct Output)
Use for:
- **Critical errors** that must always be visible
- **Startup messages** and initialization status
- **Connection state changes** (BLE connect/disconnect)
- **Runtime traces** while reproducing issues over USB

```cpp
Serial.println("[BLE] Connected!");
Serial.printf("[OBD] Speed: %d km/h\n", speed);
```

### Perf CSV Logger (`/perf/perf_boot_<id>.csv`)
Use for:
- **Post-mortem analysis** across long drives
- **Counter/timing correlation** with `/api/debug/metrics`
- **Field diagnostics** without serial attached

```cpp
perfMetricsCheckReport();          // periodic snapshot enqueue
perfMetricsEnqueueSnapshotNow();   // immediate snapshot on critical paths
```

### Legacy `debugLogger`
`debugLogger` remains as a compatibility stub so existing call sites compile, but SD-backed `debug.log` capture is removed.

---

## Testing Without Hardware (REPLAY_MODE)

For UI/display testing without a physical V1 device, use REPLAY_MODE:

### Enabling REPLAY_MODE

1. Add to `platformio.ini` build flags:
   ```ini
   build_flags = 
       ...existing flags...
       -D REPLAY_MODE
   ```

2. Build and flash:
   ```bash
   pio run -e waveshare-349 -t upload
   ```

### What REPLAY_MODE Does
- **Disables BLE scanning** - no V1 connection attempts
- **Generates synthetic alert packets** - simulates V1 data for UI testing
- **Enables display testing** - verify layouts, colors, animations
- **No hardware required** - useful for CI/CD display validation

### Disabling REPLAY_MODE
Remove the `-D REPLAY_MODE` flag and rebuild for normal operation.

---

## Common Bugs & Prevention

### Display Flashing / Constant Redraws

**Symptom**: Display elements flash or flicker constantly.

**Root Cause**: Something called every frame unconditionally triggers a redraw.

**The Pattern That Breaks**:
```cpp
// ❌ WRONG - In main loop, called every iteration
if (!hasCameraAlerts) {
    display.clearCameraAlerts();  // Triggers redraw EVERY FRAME
}
```

**The Fix**:
```cpp
// ✅ CORRECT - Track state, only call on change
static bool lastHadCameras = false;
if (hasCameraAlerts != lastHadCameras) {
    if (!hasCameraAlerts) {
        display.clearCameraAlerts();
    }
    lastHadCameras = hasCameraAlerts;
}
```

**Prevention Rules**:

1. **Never set `forceCardRedraw = true` unconditionally** in any per-frame function
2. **Always add change detection** before triggering redraws
3. **Add early exits** in display functions when state unchanged:
   ```cpp
   void updateCameraAlerts(...) {
       bool stateChanged = (active != lastCameraState) || ...;
       if (!stateChanged && !active) {
           return;  // No cameras now, no cameras before - nothing to do
       }
   }
   ```

**Debugging**:
1. Check `forceCardRedraw = true` - should never be unconditional
2. Check camera/alert update loops in main.cpp
3. Look for functions called every loop() that don't have change detection

### Frequency Jitter Causing Redraws

**Symptom**: Frequency display redraws constantly even with stable alert.

**Root Cause**: V1 frequency can jitter ±1-5 MHz between packets.

```cpp
// ❌ WRONG - Exact comparison triggers constant redraws
if (priority.frequency != lastPriority.frequency) {
    needsRedraw = true;
}

// ✅ CORRECT - Use tolerance
if (abs(priority.frequency - lastPriority.frequency) > 5) {
    needsRedraw = true;
}
```

### Memory Exhaustion from Unbounded Strings

**Symptom**: Device crashes after receiving malformed input.

**Root Cause**: String inputs without length limits.

```cpp
// ❌ WRONG - No limit
String proxyName = server.arg("proxy_name");
settingsManager.setProxyName(proxyName);

// ✅ CORRECT - Truncate to limit
String proxyName = server.arg("proxy_name");
if (proxyName.length() > 32) {
    proxyName = proxyName.substring(0, 32);
}
settingsManager.setProxyName(proxyName);
```

### Settings Not Persisting

**Symptom**: New setting reverts after reboot.

**Root Cause**: Forgot to update one of the 5 required places.

**Checklist for new settings**:
1. [ ] Add to `V1Settings` struct in `settings.h`
2. [ ] Add to `load()` in `settings.cpp` with bounds validation
3. [ ] Add to `save()` in `settings.cpp`
4. [ ] Add handler in `wifi_manager.cpp`
5. [ ] Add UI in Svelte component

---

## Display System

### Architecture

```
Draw Functions → RAM Canvas → flush() → QSPI → AMOLED Panel
```

- All draws write to RAM canvas (not directly to panel)
- `flush()` transfers canvas to panel (~26ms)
- Throttled to 40fps max (25ms minimum between frames)

### Caching System

Each draw function maintains static cache variables:

```cpp
void drawBandIndicators(...) {
    static uint8_t lastEffectiveMask = 0xFF;
    static bool lastMuted = false;
    static bool cacheValid = false;
    
    // Skip redraw if nothing changed
    if (cacheValid && 
        mask == lastEffectiveMask && 
        muted == lastMuted &&
        !s_forceBandRedraw) {
        return;
    }
    
    // ... actual drawing ...
    
    lastEffectiveMask = mask;
    lastMuted = muted;
    cacheValid = true;
    s_forceBandRedraw = false;
}
```

### Force Redraw Flags

Global flags set by `drawBaseFrame()` on full screen clear:

- `s_forceFrequencyRedraw`
- `s_forceBatteryRedraw`
- `s_forceBandRedraw`
- `s_forceSignalBarsRedraw`
- `s_forceArrowRedraw`
- `s_forceStatusBarRedraw`
- `s_forceMuteIconRedraw`
- `s_forceTopCounterRedraw`

**Rule**: Only `drawBaseFrame()` should set these to `true`.

---

## Display Ownership Map

Single owner per frame prevents redraw conflicts and flicker. Ownership is decided in `main.cpp` and validated by the display test suite (`test/test_display/`).

- **Main display**: `displayPreviewModule` owns while preview is active; otherwise `displayPipelineModule.handleParsed()` owns rendering. Live/persisted V1 states take priority, and camera banners render only via `display.updateCameraAlert(...)` when camera runtime reports an active alert and no V1 alert path is active.
- **Camera cards**: No standalone camera card writer is active; camera UX currently uses the existing primary frequency/arrow region only.
- **Voice**: `voiceModule.process()` drives V1 speech actions. Camera voice is a one-shot call to `play_camera_ahead_voice(...)` from `displayPipelineModule` when a new camera lifecycle start is observed and audio is not muted.
- **Flush discipline**: Modules never call `display.flush()` directly; `display.update()` and preview paths flush once per frame. Tests enforce ≤1 flush per frame.
- **End flags**: Preview end flags (`displayPreviewModule.consumeEnded()`) are consumed once then cleared; callers force a redraw with current state after consumption.
- **Change detection**: All display writers must early-exit when unchanged and avoid setting any `force*Redraw` flag per frame (except `drawBaseFrame()`).

Keep new display features aligned with this map and add ownership tests when introducing new display writers.

---

## Camera Module

**Location**: `src/modules/camera/`

- **Runtime entrypoint**: `camera_runtime_module.{h,cpp}` exposes `begin(...)`, `setEnabled(...)`, `process(...)`, and `snapshot()` for main-loop integration.
- **Index/data path**: `camera_index.{h,cpp}` holds immutable camera records + spans; `camera_data_loader.{h,cpp}` performs FreeRTOS-task loading/build/swap.
- **Event log**: `camera_event_log.{h,cpp}` provides bounded diagnostics snapshots used by `/api/cameras/events`.
- **Display/audio integration**: Camera modules do not draw directly; display/audio consume `cameraRuntimeModule.snapshot()` in `display_pipeline_module.cpp`.
- **Dependencies**: Matching is gated by GPS runtime snapshot validity, loop overload guards, and signal-priority preemption.

## BLE Architecture

### Client Reuse Pattern

```cpp
class BLEClient {
    NimBLEClient* pClient = nullptr;
    
    bool connect(const NimBLEAddress& addr) {
        if (!pClient) {
            pClient = NimBLEDevice::createClient();  // Create once
        }
        
        if (pClient->isConnected()) {
            pClient->disconnect();
        }
        
        return pClient->connect(addr);  // Reuse
    }
    
    ~BLEClient() {
        // Only delete in destructor
        if (pClient) {
            NimBLEDevice::deleteClient(pClient);
        }
    }
};
```

### Notify Callback Rules

```cpp
void notifyCallback(NimBLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
    // ✅ OK: memcpy to queue
    memcpy(packet.data, pData, length);
    xQueueSend(bleDataQueue, &packet, 0);
    
    // ❌ FORBIDDEN:
    // - Serial.print() - too slow
    // - new/malloc - heap allocation
    // - display calls - wrong thread
    // - packet parsing - too slow
}
```

---

## Testing Checklist

### Before Committing Display Changes

**Critical: Display changes MUST be tested with all these scenarios:**

- [ ] Test with active V1 alerts (flashing bugs only appear with live data)
- [ ] Test with NO V1 alerts (idle/resting state)
- [ ] Test transitions: alerts → no alerts → alerts
- [ ] Test camera alerts on/off transitions
- [ ] Test mute/unmute transitions
- [ ] Test volume changes
- [ ] Test brightness changes
- [ ] Let device run 5+ minutes in each state (flashing may be subtle)
- [ ] **Verify no flashing/flickering in any state**

**Display Change Review Checklist:**

1. [ ] Does any new display function have change detection / early exit?
2. [ ] Are any `force*Redraw` flags set unconditionally?
3. [ ] Is the function called from main loop without state guards?
4. [ ] Does the function use frequency comparison with tolerance (±5 MHz)?
5. [ ] Does the function flush() - if so, is it guarded?

**Common Patterns That Cause Flashing:**

```cpp
// ❌ BAD - Called every loop without change detection
void loop() {
    if (someCondition) {
        display.updateSomething();  // Called every frame!
    }
}

// ✅ GOOD - Only call on state change
static bool lastCondition = false;
void loop() {
    if (someCondition != lastCondition) {
        display.updateSomething();
        lastCondition = someCondition;
    }
}
```

```cpp
// ❌ BAD - Display function without early exit
void V1Display::updateSomething(bool active) {
    FILL_RECT(...);  // Clears every frame
    if (active) { 
        drawText(...);
    }
}

// ✅ GOOD - Display function with change detection
void V1Display::updateSomething(bool active) {
    static bool lastActive = false;
    if (active == lastActive) return;  // Early exit
    
    FILL_RECT(...);
    if (active) { 
        drawText(...);
    }
    lastActive = active;
}
```

### Before Committing BLE Changes

- [ ] Connect/disconnect V1 10 times
- [ ] Let run for 30+ minutes without disconnect
- [ ] Test OBD connection with V1 connected
- [ ] Test reconnection after power cycle

### Before Committing Settings Changes

- [ ] Verify setting persists after reboot
- [ ] Verify bounds validation (try invalid values)
- [ ] Verify UI shows correct value
- [ ] Verify API returns correct value

### Unit Tests

```bash
# Run all tests
pio test -e native

# Run specific test suite
pio test -e native -f test_display
pio test -e native -f test_packet_parser
```

---

## Error Handling

The API uses standard HTTP status codes (200, 400, 404, 500, 503) with
freeform `"error":"..."` JSON bodies. See [docs/API.md](API.md) for the
full status-code table and error response format.

---

*Last updated: February 2026*

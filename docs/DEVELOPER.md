# V1-Simple Developer Guide

Technical documentation for developers working on the V1-Simple codebase.

---

## Table of Contents

1. [Critical Rules](#critical-rules)
2. [Common Bugs & Prevention](#common-bugs--prevention)
3. [Display System](#display-system)
4. [BLE Architecture](#ble-architecture)
5. [Testing Checklist](#testing-checklist)

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

- BLE scan duty cycle ≤ 50%
- OBD connection delayed 12s after V1 connects
- WiFi scans are non-blocking but interfere with BLE

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

**Root Cause**: Forgot to update one of the 4 required places.

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
- Throttled to 20fps (50ms minimum between frames)

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

- [ ] Test with active V1 alerts (flashing bugs only appear with live data)
- [ ] Test camera alerts on/off transitions
- [ ] Test mute/unmute transitions
- [ ] Test volume changes
- [ ] Verify no flashing/flickering in any state

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
pio test -e native -f test_haversine
pio test -e native -f test_packet_parser
```

---

## Error Codes

See [docs/API.md](API.md#error-codes-reference) for the complete error code reference.

Quick reference:
- 100-199: BLE errors
- 200-299: GPS errors
- 300-399: Storage errors
- 400-499: WiFi errors
- 500-599: V1 protocol errors
- 900-999: System errors

---

*Last updated: January 2025*

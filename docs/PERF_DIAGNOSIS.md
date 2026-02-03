# Performance Diagnosis Report

**Date:** 2024-01-XX
**Evidence:** debug-15.log (soak test ~4h)
**Goal:** Minimize latency and stalls between incoming BLE data and display updates

---

## Executive Summary

Analysis of debug-15.log reveals **four distinct performance issues**:

| Issue | Severity | Root Cause | Impact |
|-------|----------|------------|--------|
| Async Queue Failure | Medium | `xQueueCreate()` fails - memory pressure at startup | Falls back to sync writes, potential main loop blocking |
| WiFi Stalls | High | 512ms spike at startup (line 3), ongoing ~1.6ms | Initial 540ms loop stall |
| BLE Drain Stalls | **Critical** | Up to 797ms post-connect (line 456) | Display freezes during alert processing |
| Unknown 3.52s Stall | **Critical** | BLE connect/discovery path (NOT instrumented) | Complete UI freeze during reconnection |

---

## Issue A: Async Logger Queue Creation Failure

### Evidence
```
Line 1: {"category":"system","message":"[Logger] ERROR: Failed to create async write queue"}
```

### Root Cause Analysis
**File:** [debug_logger.cpp](../src/debug_logger.cpp#L692-L725)

```cpp
void DebugLogger::setAsyncMode(bool async) {
    if (async) {
        if (!writeQueue) {
            writeQueue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(LogQueueItem));  // Line 698
            if (!writeQueue) {
                log(DebugLogCategory::System, "[Logger] ERROR: Failed to create async write queue");
                return;
            }
        }
        ...
    }
}
```

**Queue Memory Calculation:**
- `LOG_QUEUE_DEPTH = 32` ([debug_logger.h#L46](../src/debug_logger.h#L46))
- `sizeof(LogQueueItem) = 520 bytes` (512 line + 8 overhead)
- **Total allocation: ~16.6 KB** (plus FreeRTOS overhead)

**Likely Cause:** Memory fragmentation during early boot. At line 1 (90.8s uptime), heap fragmentation from BLE stack, WiFi init, and camera DB loading may prevent finding a contiguous 16KB block.

### Current Workaround
System falls back to synchronous writes, which block the main loop during SD I/O (up to 6-18ms per `sdMax_us`).

### Recommendation
1. Pre-allocate queue earlier in boot (before heavy allocations)
2. Reduce queue depth or use statically allocated ring buffer
3. Add heap stats logging on failure

---

## Issue B: WiFi/FS Blocking Stalls

### Evidence
```
Line 3: wifiMax_us=511972 (~512ms), loopMax_us=540639 (~540ms)
```
Correlation: `loopMax_us ≈ wifiMax_us + 30ms` → WiFi dominates this stall.

Steady-state: `wifiMax_us=1600-1700us` (~1.6ms) - acceptable.

### Root Cause Analysis
**File:** [wifi_manager.cpp](../src/wifi_manager.cpp)

The 512ms spike occurs at startup during:
1. WiFi AP/STA initialization (`WiFi.mode()`, `WiFi.softAP()`)
2. WebServer initialization (`server.begin()`)
3. First client request processing

**File Serving Path:**
```cpp
// wifi_manager.cpp - serveLittleFSFileHelper()
void WifiManager::serveLittleFSFileHelper(const char* path, const char* contentType, bool gzip) {
    File file = LittleFS.open(path, "r");
    // ... streaming with yield() calls
}
```

The `yield()` calls help, but first-file-open on cold LittleFS may still block.

### Recommendation
1. Add `perfRecordWifiSetupUs()` timer around WiFi init
2. Consider deferring full WebServer start until after BLE connect
3. Pre-warm LittleFS cache during boot idle time

---

## Issue C: BLE Drain Stalls (Post-Connect Buffering)

### Evidence
```
Line 50 (connection):  bleDrainMax_us=973        (0.97ms - reasonable)
Line 223 (5s later):   bleDrainMax_us=112679     (112ms)
Line 351 (10s later):  bleDrainMax_us=129589     (129ms)
Line 456 (15s later):  bleDrainMax_us=797396     (797ms!) ← CRITICAL
Line 549 (20s later):  bleDrainMax_us=70330      (70ms)
Line 735 (30s later):  bleDrainMax_us=28311      (28ms) ← stabilizing
```

### Root Cause Analysis
**File:** [ble_queue_module.cpp](../src/modules/ble/ble_queue_module.cpp#L141-L175)

```cpp
void BleQueueModule::process() {
    // 1. Drain all queued BLE packets into rxBuffer
    while (queueHandle && xQueueReceive(queueHandle, &pkt, 0) == pdTRUE) {
        rxBuffer.insert(rxBuffer.end(), pkt.data, pkt.data + pkt.length);
    }
    
    // 2. Process proxy queue (BLE stack interaction)
    if (ble) {
        ble->processProxyQueue();  // <-- Potential blocking call
    }
    
    // 3. Parse all complete packets and update display
    while (true) {
        // Frame parsing, checksum validation
        // ...
        if (displayPipeline) {
            displayPipeline->handleParsed(nowMs);  // <-- Display update
        }
    }
}
```

**Issue Breakdown:**
- `bleDrainMax_us` includes: queue drain + proxy queue + parsing + display updates
- Post-connect: ~100 packets buffered during 3.52s stall, then processed in burst
- `displayPipeline->handleParsed()` triggers SPI display writes (~10-30ms each)
- Processing 10+ packets × 30ms = 300ms+ in one `process()` call

### Recommendation
1. Limit packets processed per `process()` call (e.g., max 5)
2. Separate "drain" timing from "process" timing for attribution
3. Add `PERF_MAX(blePacketsPerProcess, count)` metric
4. Consider processing display updates at lower rate (30 FPS throttle exists but may not be honored during burst)

---

## Issue D: Unknown 3.52s Stall (BLE Reconnection)

### Evidence
```
Line 50:  loopMax_us=3520057 (3.52s), reconn=1, heapMin=26676
Line 52:  "investigation_triggered loopMax_us=3520057 qDropDelta=0"
```

**Critical Observation:**
- `wifiMax_us=1671` (only 1.6ms) → WiFi is NOT the cause
- `bleDrainMax_us=973` (only 0.97ms) → BLE drain is NOT the cause
- `reconn=1` → A reconnection just completed

**The 3.52s is spent OUTSIDE the instrumented code paths!**

### Root Cause Analysis
**File:** [ble_client.cpp](../src/ble_client.cpp#L698-L870)

```cpp
bool V1BLEClient::connectToServer() {
    // ... guards and setup (~100ms)
    
    // SYNCHRONOUS connect with retries (up to 2 attempts)
    for (int attempt = 1; attempt <= attempts && !connectedOk; ++attempt) {
        connectedOk = pClient->connect(targetAddress, true);  // <-- BLOCKING!
        if (!connectedOk) {
            lastErr = pClient->getLastError();
            if (lastErr == 13 && attempt < attempts) {
                vTaskDelay(pdMS_TO_TICKS(2000));  // <-- 2s delay on EBUSY
            } else if (attempt < attempts) {
                vTaskDelay(pdMS_TO_TICKS(750));   // <-- 750ms delay
            }
        }
    }
    
    return finishConnection();
}

bool V1BLEClient::finishConnection() {
    // ... logging
    
    // Service discovery - BLOCKING!
    int maxRetries = 3;
    for (int retry = 0; retry < maxRetries; retry++) {
        if (pClient->discoverAttributes()) {  // <-- BLOCKING! (1-3s typical)
            break;
        }
        delay(50);
    }
    
    bool ok = setupCharacteristics();  // Subscribe to notifications (~500ms)
    // ...
}
```

**Time Attribution (estimated):**
| Phase | Duration | Notes |
|-------|----------|-------|
| `pClient->connect()` | 1-2s | BLE link establishment |
| `discoverAttributes()` | 1-2s | GATT service discovery |
| `setupCharacteristics()` | 0.5s | Subscribe to notifications |
| **Total** | **2.5-4.5s** | Matches observed 3.52s |

### Why It's Not Captured
The `perfRecordBleDrainUs()` timer wraps `bleQueueModule.process()`, but the BLE connection happens in `bleClient.process()` which is **not timed**:

```cpp
// main.cpp loop()
bleClient.process();  // <-- NOT TIMED - contains blocking connect!

uint32_t bleDrainStartUs = PERF_TIMESTAMP_US();
bleQueueModule.process();
perfRecordBleDrainUs(PERF_TIMESTAMP_US() - bleDrainStartUs);  // <-- Only times queue drain
```

### Recommendation
1. Add `perfRecordBleConnectUs()` around `connectToServer()` + `finishConnection()`
2. Add `bleConnMax_us` to perf metrics for reporting
3. Consider async connect pattern (non-blocking with state machine)
4. Split `bleClient.process()` into timed phases

---

## Metric Gaps Summary

| What We Measure | What We DON'T Measure |
|-----------------|----------------------|
| `wifiMax_us` - WiFi/WebServer process | WiFi init (one-time) |
| `bleDrainMax_us` - Queue drain + parse + display | BLE connect/discovery/subscribe |
| `loopMax_us` - Total loop time | Per-subsystem breakdown |
| `sdMax_us` - SD write | SD mount/init |
| `flushMax_us` - Buffer flush | - |

---

## Appendix: Key Files

| File | Role | Lines of Interest |
|------|------|-------------------|
| [debug_logger.cpp](../src/debug_logger.cpp) | Async logging | L692-760 (setAsyncMode) |
| [ble_client.cpp](../src/ble_client.cpp) | BLE connection | L698-870 (connectToServer) |
| [ble_queue_module.cpp](../src/modules/ble/ble_queue_module.cpp) | Packet processing | L141-292 (process) |
| [wifi_manager.cpp](../src/wifi_manager.cpp) | WebServer | L1-400 (file serving) |
| [main.cpp](../src/main.cpp) | Main loop | L650-750 (loop timing) |
| [perf_metrics.h](../src/perf_metrics.h) | Metrics defs | L1-150 |

---

## Next Steps

See [PERF_PATCH_PLAN.md](PERF_PATCH_PLAN.md) for implementation details.

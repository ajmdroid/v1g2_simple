# Performance Optimization Patch Plan

**Reference:** [PERF_DIAGNOSIS.md](PERF_DIAGNOSIS.md)
**Priority:** Minimize latency between BLE data arrival and display update

---

## Commit Strategy

Small, incremental commits. Each commit should:
1. Pass compilation
2. Not break existing behavior
3. Be independently revertable
4. Include relevant tests where applicable

---

## Phase 1: Instrumentation (Objective D) - Do This FIRST

**Rationale:** Before fixing anything, we need to see where time is spent. The 3.52s stall is currently invisible to metrics.

### Commit 1.1: Add BLE Connection Timing to perf_metrics

**Files:** `src/perf_metrics.h`, `src/perf_metrics.cpp`

```cpp
// In PerfExtendedMetrics struct (perf_metrics.h ~line 130)
uint32_t bleConnMaxUs = 0;     // BLE connect() call duration
uint32_t bleDiscMaxUs = 0;     // Service discovery duration
uint32_t bleSubsMaxUs = 0;     // Characteristic subscribe duration

// In reset() (perf_metrics.cpp)
bleConnMaxUs = 0;
bleDiscMaxUs = 0;
bleSubsMaxUs = 0;

// Add recording functions (perf_metrics.h)
void perfRecordBleConnectUs(uint32_t us);
void perfRecordBleDiscoveryUs(uint32_t us);
void perfRecordBleSubscribeUs(uint32_t us);
```

### Commit 1.2: Instrument BLE Connection Path

**File:** `src/ble_client.cpp`

Wrap timing around blocking calls in `connectToServer()` and `finishConnection()`:

```cpp
bool V1BLEClient::connectToServer() {
    // ... guards ...
    
    uint32_t connStartUs = micros();
    
    for (int attempt = 1; attempt <= attempts && !connectedOk; ++attempt) {
        connectedOk = pClient->connect(targetAddress, true);
        // ...
    }
    
    perfRecordBleConnectUs(micros() - connStartUs);
    
    if (!connectedOk) {
        // ... failure handling ...
    }
    
    return finishConnection();
}

bool V1BLEClient::finishConnection() {
    // ...
    
    uint32_t discStartUs = micros();
    for (int retry = 0; retry < maxRetries; retry++) {
        if (pClient->discoverAttributes()) {
            break;
        }
        delay(50);
    }
    perfRecordBleDiscoveryUs(micros() - discStartUs);
    
    uint32_t subsStartUs = micros();
    bool ok = setupCharacteristics();
    perfRecordBleSubscribeUs(micros() - subsStartUs);
    
    // ...
}
```

### Commit 1.3: Add New Metrics to Perf Report

**Files:** `src/modules/reporting/perf_reporter_module.cpp`

Add `bleConnMax_us`, `bleDiscMax_us`, `bleSubsMax_us` to the JSON report.

### Commit 1.4: Add bleClient.process() Timing

**File:** `src/main.cpp`

```cpp
// Around line 700 in loop()
uint32_t bleProcessStartUs = PERF_TIMESTAMP_US();
bleClient.process();
perfRecordBleProcessUs(PERF_TIMESTAMP_US() - bleProcessStartUs);
```

This captures any blocking that happens inside `bleClient.process()` including reconnection.

---

## Phase 2: Fix Async Logging (Objective A)

### Commit 2.1: Pre-allocate Async Queue at Boot

**File:** `src/debug_logger.cpp`, `src/debug_logger.h`

Move queue creation to `begin()` instead of `setAsyncMode()`:

```cpp
// debug_logger.h - Add to class
private:
    bool queuePreallocated = false;

// debug_logger.cpp - In begin()
bool DebugLogger::begin(const char* filename, uint32_t maxSize, DebugLogConfig cfg) {
    // ... existing init ...
    
    // Pre-allocate queue early before heap fragmentation
    if (cfg.asyncWrites && !writeQueue) {
        writeQueue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(LogQueueItem));
        if (writeQueue) {
            queuePreallocated = true;
            Serial.println("[Logger] Queue pre-allocated for async mode");
        } else {
            Serial.printf("[Logger] WARNING: Queue pre-alloc failed (heap: %u, block: %u)\n",
                          ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        }
    }
    
    return true;
}
```

### Commit 2.2: Add Heap Stats on Queue Failure

**File:** `src/debug_logger.cpp`

Enhance error message with heap diagnostics:

```cpp
void DebugLogger::setAsyncMode(bool async) {
    if (async) {
        if (!writeQueue) {
            uint32_t freeHeap = ESP.getFreeHeap();
            uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
            size_t queueSize = LOG_QUEUE_DEPTH * sizeof(LogQueueItem);
            
            writeQueue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(LogQueueItem));
            if (!writeQueue) {
                Serial.printf("[Logger] ERROR: Queue alloc failed (need ~%u, have heap=%u block=%u)\n",
                              queueSize, freeHeap, largestBlock);
                if (enabled && categoryAllowed(DebugLogCategory::System)) {
                    logf(DebugLogCategory::System, 
                         "[Logger] ERROR: Queue alloc failed (need ~%u, heap=%u, block=%u)",
                         queueSize, freeHeap, largestBlock);
                }
                return;
            }
        }
        // ...
    }
}
```

### Commit 2.3: Reduce Queue Size as Fallback

**File:** `src/debug_logger.h`

Consider smaller queue as trade-off:

```cpp
// Original: 32 items × 520 bytes = 16.6 KB
// Reduced:  16 items × 520 bytes = 8.3 KB
inline constexpr size_t LOG_QUEUE_DEPTH = 16;  // Reduced from 32

// Or keep 32 but reduce line size
inline constexpr size_t LOG_QUEUE_LINE_SIZE = 384;  // Reduced from 512
// 32 × 392 = 12.5 KB
```

---

## Phase 3: Reduce BLE Drain Stalls (Objective C)

### Commit 3.1: Limit Packets Per Process Cycle

**File:** `src/modules/ble/ble_queue_module.cpp`

```cpp
void BleQueueModule::process() {
    static constexpr size_t MAX_PACKETS_PER_CYCLE = 5;  // Don't process more than 5
    size_t packetsProcessed = 0;
    
    // ... existing queue drain ...
    
    while (true) {
        if (packetsProcessed >= MAX_PACKETS_PER_CYCLE) {
            break;  // Let main loop breathe, continue next cycle
        }
        
        // ... existing packet parsing ...
        
        if (parseOk) {
            // ... display update ...
            packetsProcessed++;
        }
        
        // ... existing cleanup ...
    }
}
```

### Commit 3.2: Separate Drain Timing from Process Timing

**File:** `src/main.cpp`

```cpp
// Split timing for better attribution
uint32_t bleQueueStartUs = PERF_TIMESTAMP_US();
bleQueueModule.drainQueue();  // New: just pull from FreeRTOS queue
perfRecordBleQueueDrainUs(PERF_TIMESTAMP_US() - bleQueueStartUs);

uint32_t bleProcessStartUs = PERF_TIMESTAMP_US();
bleQueueModule.processBuffer();  // New: parse and display
perfRecordBleProcessUs(PERF_TIMESTAMP_US() - bleProcessStartUs);
```

**File:** `src/modules/ble/ble_queue_module.h`

```cpp
// Split process() into two phases
void drainQueue();      // ISR-safe: just move data from queue to buffer
void processBuffer();   // Parse packets, update display
```

### Commit 3.3: Add Packets-Per-Cycle Metric

**File:** `src/perf_metrics.h`

```cpp
// In PerfExtendedMetrics
uint32_t blePacketsPerCycleMax = 0;  // Max packets processed in one cycle

// In perf_metrics.cpp
void perfRecordBlePacketsPerCycle(uint32_t count) {
    PERF_TRACK_MAX(extMetrics.blePacketsPerCycleMax, count);
}
```

---

## Phase 4: Reduce WiFi/FS Stalls (Objective B)

### Commit 4.1: Add WiFi Init Timing

**File:** `src/wifi_manager.cpp`

```cpp
void WifiManager::begin() {
    uint32_t wifiInitStartUs = micros();
    
    // ... existing WiFi.mode(), WiFi.softAP(), etc ...
    
    uint32_t wifiInitUs = micros() - wifiInitStartUs;
    Serial.printf("[WiFi] Init took %lu us\n", wifiInitUs);
    
    // Log to debug logger if available
    if (debugLogger.isEnabledFor(DebugLogCategory::System)) {
        debugLogger.logf(DebugLogCategory::System, "[WiFi] Init took %lu us", wifiInitUs);
    }
}
```

### Commit 4.2: Pre-warm LittleFS Cache

**File:** `src/wifi_manager.cpp`

After WebServer starts, touch common files to pre-cache:

```cpp
void WifiManager::prewarmFilesystem() {
    // Touch common files to warm FS cache
    const char* commonFiles[] = {
        "/index.html", "/app.js", "/app.css", "/favicon.ico"
    };
    
    for (const char* path : commonFiles) {
        File f = LittleFS.open(path, "r");
        if (f) {
            f.read();  // Just read first byte to warm cache
            f.close();
        }
    }
}
```

---

## Phase 5: Validation

### Commit 5.1: Add Validation Assertions to Tests

**File:** `test/test_perf_metrics/test_perf_metrics.cpp` (new or existing)

```cpp
void test_ble_connect_timing_captured() {
    // Simulate a reconnect
    // Assert bleConnMaxUs > 0 after reconnect
}

void test_async_queue_allocation() {
    // Verify queue pre-allocation succeeds
    // Assert heap before/after delta is reasonable
}
```

---

## Validation Checklist

After implementing each phase, run soak test and verify:

### Phase 1 (Instrumentation) ✓
- [ ] `bleConnMax_us` appears in perf logs
- [ ] `bleDiscMax_us` appears in perf logs  
- [ ] `bleSubsMax_us` appears in perf logs
- [ ] `bleProcessMax_us` appears in perf logs (main loop wrapper)
- [ ] Sum of new metrics ≈ `loopMax_us` during reconnect

### Phase 2 (Async Logging) ✓
- [ ] No "[Logger] ERROR: Failed to create async write queue" message
- [ ] "[Logger] Queue pre-allocated" appears at boot
- [ ] Heap stats logged if allocation fails

### Phase 3 (BLE Drain) ✓
- [ ] `bleDrainMax_us` < 50ms steady-state (was 70-130ms)
- [ ] No display freezes during packet bursts
- [ ] `blePacketsPerCycleMax` visible in logs

### Phase 4 (WiFi) ✓
- [ ] `wifiMax_us` < 5ms steady-state (no change expected)
- [ ] WiFi init timing logged at boot
- [ ] First page load faster (FS prewarm)

### Overall
- [ ] `loopMax_us` < 100ms except during BLE reconnect
- [ ] During reconnect: `loopMax_us` explained by `bleConnMax_us` + `bleDiscMax_us`
- [ ] `qDrop = 0` throughout soak test
- [ ] Stable 4h+ soak with no unexpected spikes

---

## Risk Assessment

| Change | Risk | Mitigation |
|--------|------|------------|
| Pre-allocate queue | Low | Fallback to sync mode if fails |
| Limit packets/cycle | Medium | May increase overall latency slightly |
| Split drain/process | Medium | Interface change in ble_queue_module |
| FS prewarm | Low | Non-blocking, optional |
| New metrics | Very Low | Compile-time gated |

---

## Implementation Order

1. **Phase 1** - Must do first to validate subsequent changes
2. **Phase 2** - Quick win, low risk
3. **Phase 3** - Addresses critical 800ms stalls
4. **Phase 4** - Nice to have, lower priority
5. **Phase 5** - Ongoing validation

**Estimated LOC:** ~200-300 lines across all phases
**Estimated Time:** 4-6 hours including testing

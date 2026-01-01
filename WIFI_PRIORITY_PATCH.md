# WiFi Priority Mode - Complete Patch

## Summary
Makes Web UI fast by disabling SD logging and deprioritizing BLE during active web use.

---

## File 1: src/serial_logger.h

### Change: Add SD-only disable control
**Line ~115:**
```diff
-    // Enable/disable SD logging
-    void setEnabled(bool en) { enabled = en && (fs != nullptr); }
-    bool isEnabled() const { return enabled; }
+    // Enable/disable SD logging (runtime control for WiFi priority mode)
+    // When disabled, Serial output continues but SD writes are skipped
+    void setSDEnabled(bool en) { 
+        if (fs != nullptr) {
+            enabled = en;
+        }
+    }
+    bool isSDEnabled() const { return enabled; }
+    
+    // Legacy compatibility
+    void setEnabled(bool en) { setSDEnabled(en); }
+    bool isEnabled() const { return isSDEnabled(); }
```

---

## File 2: src/wifi_manager.h

### Change 1: Add web activity tracking methods
**Line ~68:**
```diff
     static void requestTempWiFiSession(unsigned long durationMs);
     static bool consumeTempWiFiSession(unsigned long& durationMs);
+    
+    // Web activity tracking (for WiFi priority mode)
+    void markUiActivity();  // Call on every HTTP request
+    bool isUiActive(unsigned long timeoutMs = 30000) const;
```

### Change 2: Add activity timestamp variable
**Line ~72:**
```diff
 private:
     WebServer server;
     SetupModeState setupModeState;
     unsigned long setupModeStartTime;
+    
+    // Web activity tracking for WiFi priority mode
+    unsigned long lastUiActivityMs = 0;
```

---

## File 3: src/wifi_manager.cpp

### Change 1: Implement tracking + integrate with checkRateLimit()
**Line ~175:**
```diff
 bool WiFiManager::checkRateLimit() {
     unsigned long now = millis();
+    
+    // Mark UI activity on every request
+    markUiActivity();
     
     // Reset window if expired
     if (now - rateLimitWindowStart > RATE_LIMIT_WINDOW_MS) {
         ...
     }
     
     return true;
 }
+
+// Web activity tracking for WiFi priority mode
+void WiFiManager::markUiActivity() {
+    lastUiActivityMs = millis();
+}
+
+bool WiFiManager::isUiActive(unsigned long timeoutMs) const {
+    if (lastUiActivityMs == 0) return false;
+    return (millis() - lastUiActivityMs) < timeoutMs;
+}
```

### Change 2: Add markUiActivity() to root handler
**Line ~302:**
```diff
 server.on("/", HTTP_GET, [this]() { 
+    markUiActivity();  // Track UI activity
     // Try Svelte index.html first
     if (serveLittleFSFile("/index.html", "text/html")) {
         ...
```

### Change 3: Add to onNotFound handler
**Line ~315:**
```diff
 server.onNotFound([this]() {
+    markUiActivity();  // Track UI activity
     String uri = server.uri();
     ...
```

### Change 4: Add to captive portal handlers
**Line ~393-413:**
```diff
 server.on("/ping", HTTP_GET, [this]() {
+    markUiActivity();
     SerialLog.println("[HTTP] GET /ping");
     server.send(200, "text/plain", "OK");
 });
 server.on("/generate_204", HTTP_GET, [this]() {
+    markUiActivity();
     SerialLog.println("[HTTP] GET /generate_204");
     ...
 });
 server.on("/gen_204", HTTP_GET, [this]() {
+    markUiActivity();
     ...
 });
 server.on("/hotspot-detect.html", HTTP_GET, [this]() {
+    markUiActivity();
     ...
 });
```

---

## File 4: src/ble_client.h

### Change 1: Add WiFi priority methods
**Line ~172:**
```diff
     void resetProxyMetrics() { proxyMetrics.reset(); }
+    
+    // WiFi priority mode - deprioritize BLE when web UI is active
+    void setWifiPriority(bool enabled);  // Enable = suppress BLE activity
+    bool isWifiPriority() const { return wifiPriorityMode; }
```

### Change 2: Add priority flag variable
**Line ~295:**
```diff
     ProxyWriteCallbacks* pProxyWriteCallbacks;
+    
+    // WiFi priority mode flag
+    bool wifiPriorityMode = false;
```

---

## File 5: src/ble_client.cpp

### Change 1: Suppress scanning in DISCONNECTED state
**Line ~1241:**
```diff
 switch (bleState) {
     case BLEState::DISCONNECTED: {
+        // Skip scanning if WiFi priority mode is active
+        if (wifiPriorityMode) {
+            return;
+        }
+        
         // Not connected - start scanning...
```

### Change 2: Implement setWifiPriority() method
**Line ~1397 (after disconnect() method):**
```diff
 void V1BLEClient::disconnect() {
     if (pClient && pClient->isConnected()) {
         pClient->disconnect();
     }
 }
+
+// ==================== WiFi Priority Mode ====================
+
+void V1BLEClient::setWifiPriority(bool enabled) {
+    if (wifiPriorityMode == enabled) return;  // No change
+    
+    wifiPriorityMode = enabled;
+    
+    if (enabled) {
+        Serial.println("[BLE] WiFi priority ENABLED - suppressing scans/reconnects/proxy");
+        
+        // Stop any active scan
+        NimBLEScan* pScan = NimBLEDevice::getScan();
+        if (pScan && pScan->isScanning()) {
+            Serial.println("[BLE] Stopping scan for WiFi priority mode");
+            pScan->stop();
+            pScan->clearResults();
+        }
+        
+        // Stop proxy advertising if running
+        if (proxyEnabled && NimBLEDevice::getAdvertising()->isAdvertising()) {
+            Serial.println("[BLE] Stopping proxy advertising for WiFi priority mode");
+            NimBLEDevice::stopAdvertising();
+        }
+        
+        // Cancel any pending deferred advertising start
+        proxyAdvertisingStartMs = 0;
+        
+    } else {
+        Serial.println("[BLE] WiFi priority DISABLED - resuming normal BLE operation");
+        
+        // Resume proxy advertising if connected
+        if (isConnected() && proxyEnabled && proxyServerInitialized) {
+            Serial.println("[BLE] Resuming proxy advertising after WiFi priority mode");
+            proxyAdvertisingStartMs = millis() + 500;
+        }
+        
+        // Resume scanning if disconnected
+        if (!isConnected() && bleState == BLEState::DISCONNECTED) {
+            Serial.println("[BLE] Resuming scan after WiFi priority mode");
+            startScanning();
+        }
+    }
+}
```

---

## File 6: src/main.cpp

### Change: Integrate WiFi priority in main loop
**Line ~1972 (replace `bleClient.process()` section):**
```diff
 #ifndef REPLAY_MODE
-    // Process BLE events
-    bleClient.process();
+    // ========== WiFi Priority Mode ==========
+    // Deprioritize BLE + disable SD logging during web UI use
+    static bool wifiPriorityActive = false;
+    bool wifiPriorityNow = wifiManager.isSetupModeActive() || 
+                           wifiManager.isUiActive(30000);
+    
+    // Transition logging
+    if (wifiPriorityNow != wifiPriorityActive) {
+        wifiPriorityActive = wifiPriorityNow;
+        if (wifiPriorityNow) {
+            SerialLog.println("[WiFiPriority] ENABLED (UI active or Setup Mode on)");
+            SerialLog.println("[SerialLog] SD logging disabled for WiFi priority");
+        } else {
+            SerialLog.println("[WiFiPriority] DISABLED (UI idle, resuming normal operation)");
+            SerialLog.println("[SerialLog] SD logging re-enabled");
+        }
+    }
+    
+    // Apply WiFi priority controls
+    SerialLog.setSDEnabled(!wifiPriorityNow);
+    bleClient.setWifiPriority(wifiPriorityNow);
+    
+    // Process BLE events (with priority mode suppression)
+    bleClient.process();
 #endif
```

---

## Summary of Changes

| File | Lines Added | Lines Changed | Purpose |
|------|-------------|---------------|---------|
| serial_logger.h | 8 | 2 | SD-only disable control |
| wifi_manager.h | 4 | 0 | Activity tracking API |
| wifi_manager.cpp | 13 | 6 | Activity tracking implementation |
| ble_client.h | 4 | 0 | WiFi priority API |
| ble_client.cpp | 42 | 3 | BLE suppression logic |
| main.cpp | 22 | 2 | Priority mode integration |
| **Total** | **93** | **13** | **~100 lines total** |

---

## Key Features

✅ **SD logging disabled during web UI use** (serial continues)  
✅ **BLE scans suppressed** when WiFi active  
✅ **Proxy advertising stopped** during web UI  
✅ **Existing V1 connection preserved** (doesn't break radar)  
✅ **Automatic 30-second timeout** after last HTTP request  
✅ **Setup Mode auto-triggers** priority mode  
✅ **Logging for all transitions** (ENABLED/DISABLED)  

---

## Testing Quick Reference

1. **Enter Setup Mode**: Hold BOOT+PWR → See `[WiFiPriority] ENABLED`
2. **Load page**: Visit http://192.168.35.5/ → Fast load, no SD stalls
3. **Wait 30s**: No clicks → See `[WiFiPriority] DISABLED`, BLE resumes
4. **Exit Setup Mode**: Click button → WiFi stops, BLE resumes
5. **V1 connection**: Stays connected throughout (if already connected)

---

## Build & Deploy

```bash
cd /Users/ajmedford/v1g2_simple
./build.sh --all
# Flash to device via PlatformIO
```

**Build status:** ✅ Success (Exit 0)

---

## Rollback

To remove WiFi priority mode:
1. Revert `setSDEnabled()` calls in main.cpp
2. Remove `setWifiPriority()` calls in main.cpp
3. Remove activity tracking from wifi_manager.cpp
4. Behavior returns to pre-patch state

All changes are isolated and reversible.

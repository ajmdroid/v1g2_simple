#include "wifi_manager.h"
#include "settings_manager.h"
#include "serial_log.h"
#include <LittleFS.h>

// Dump LittleFS root directory for diagnostics
static void dumpLittleFSRoot() {
    // LittleFS is mounted in setupWebServer(); do NOT format-on-fail here.
    // If mount failed earlier, this will just report an error.
    if (!LittleFS.begin(false)) {
        SerialLog.println("[SetupMode] ERROR: LittleFS mount failed (root dump)");
        return;
    }

    SerialLog.println("[SetupMode] Dumping LittleFS root...");
    SerialLog.println("[SetupMode] Files in LittleFS root:");

    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        SerialLog.println("[SetupMode] ERROR: Could not open root directory");
        if (root) root.close();
        return;
    }

    File file = root.openNextFile();
    bool hasFiles = false;
    while (file) {
        hasFiles = true;
        SerialLog.printf("[SetupMode]   %s (%u bytes)\n", file.name(), file.size());
        file = root.openNextFile();
    }

    if (!hasFiles) {
        SerialLog.println("[SetupMode]   (empty)");
    }

    root.close();
}

void WiFiManager::setupWebServer() {
    if (!LittleFS.begin(false)) {  // do NOT format-on-fail
        SerialLog.println("[SetupMode] ERROR: LittleFS mount failed!");
    } else {
        SerialLog.println("[SetupMode] LittleFS mounted");
        dumpLittleFSRoot();
    }

    // UI status endpoint (Svelte dashboard expects this JSON shape)
    server.on("/api/status", HTTP_GET, [this]() {
        if (!checkRateLimit()) return;
        handleStatus();
    });

    // Raw/diagnostic status (failsafe dashboard / debugging)
    server.on("/api/status/raw", HTTP_GET, [this]() {
        if (!checkRateLimit()) return;
        handleApiStatus();
    });

    // ... other server routes ...

    // Remove legacy duplicate /api/status handler here (deleted per instructions)

    // Legacy status endpoint with rate limiting
    server.on("/status", HTTP_GET, [this]() { if (!checkRateLimit()) return; handleStatus(); });

    // ... rest of setupWebServer() ...
}

void WiFiManager::handleStatus() {
    // JSON contract expected by the Svelte UI:
    // {
    //   wifi:{sta_connected,ap_active,sta_ip,ap_ip,ssid,rssi},
    //   device:{uptime,heap_free,hostname},
    //   v1_connected:bool,
    //   alert:{...}|null
    // }

    const V1Settings& settings = settingsManager.get();

    bool apOn = (setupModeState == SETUP_MODE_AP_ON);

    String json = "{";

    // WiFi block (AP-only for setup)
    json += "\"wifi\":{";
    json += "\"sta_connected\":false,";
    json += "\"ap_active\":" + String(apOn ? "true" : "false") + ",";
    json += "\"sta_ip\":\"\",";
    json += "\"ap_ip\":\"" + getAPIPAddress() + "\",";
    json += "\"ssid\":\"" + jsonEscape(settings.apSSID) + "\",";
    json += "\"rssi\":0";
    json += "},";

    // Device block
    json += "\"device\":{";
    json += "\"uptime\":" + String(millis() / 1000) + ",";
    json += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"hostname\":\"v1g2\"";
    json += "}";

    // v1_connected injected by callback
    if (getStatusJson) {
        json += "," + getStatusJson();
    } else {
        json += ",\"v1_connected\":false";
    }

    // alert injected by callback
    if (getAlertJson) {
        json += ",\"alert\":" + getAlertJson();
    } else {
        json += ",\"alert\":null";
    }

    json += "}";

    server.send(200, "application/json", json);
}

#include "ble_client.h"

void V1BLEClient::setWifiPriority(bool enabled) {
    wifiPriorityMode = enabled;

    if (enabled) {
        // In Setup/Web UI work, drop the V1 connection to stop BLE notifications
        // and free radio time for WiFi/AP responsiveness.
        if (isConnected()) {
            Serial.println("[BLE] Disconnecting from V1 for WiFi priority mode");
            disconnect();
        }
    } else {
        // Note: We keep existing V1 connection if already connected
        // to avoid disrupting active radar detection
    }
}

void V1BLEClient::process() {
    if (wifiPriorityMode) {
        // Web UI is active: suppress periodic BLE work.
        return;
    }

    // ... existing process() code ...
}

#include "main.h"
#include "wifi_manager.h"
#include "ble_client.h"
#include "serial_log.h"
#include "battery_manager.h"

void loop() {
#ifndef PERF_TEST_DISABLE_WIFI
    // In Setup Mode / active Web UI, prioritize HTTP handling to keep the UI responsive.
    // Process WiFi early so UI activity timestamps update before we decide priority.
    wifiManager.process();
#endif

#ifndef REPLAY_MODE
    bool wifiPriorityNow = wifiManager.isSetupModeActive() || wifiManager.isUiActive(30000);
    if (wifiPriorityNow) {
        // Disable SD writes (keep Serial) and suppress BLE activity aggressively
        SerialLog.setSDEnabled(false);
        bleClient.setWifiPriority(true);

#if defined(DISPLAY_WAVESHARE_349) && !defined(PERF_TEST_DISABLE_BATTERY)
        // Keep power button handling responsive while in setup mode
        batteryManager.update();
        batteryManager.processPowerButton();
#endif
        // Nothing else should run while actively using the web UI
        return;
    }
#endif

    // Normal driving mode: lowest-latency BLE processing first
    processBLEData();

    // ... rest of loop() ...

    // In the existing WiFi priority block, remove bleClient.process() call as instructed.
    // bleClient.setWifiPriority(...) call remains.
}

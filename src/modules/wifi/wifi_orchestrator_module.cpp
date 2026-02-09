#include "wifi_orchestrator_module.h"

WifiOrchestrator::WifiOrchestrator(WiFiManager& wifiManager,
                                   DebugLogger& debugLogger,
                                   V1BLEClient& bleClient,
                                   PacketParser& parser,
                                   SettingsManager& settingsManager,
                                   StorageManager& storageManager,
                                   GPSHandler& gpsHandler,
                                   AutoPushModule& autoPushModule,
                                   std::function<void(int)> profilePushFn)
    : wifiManager(wifiManager),
      debugLogger(debugLogger),
      bleClient(bleClient),
      parser(parser),
      settingsManager(settingsManager),
      storageManager(storageManager),
      gpsHandler(gpsHandler),
      autoPushModule(autoPushModule),
      profilePushFn(std::move(profilePushFn)) {}

void WifiOrchestrator::startWifi() {
    if (wifiManager.isSetupModeActive()) return;

    // Always ensure callbacks are bound exactly once, even if WiFi was started elsewhere
    if (!callbacksConfigured) {
        configureCallbacks();
        callbacksConfigured = true;
    }

    // Skip if WiFi already up to keep begin()/TX power idempotent
    if (WiFi.getMode() != WIFI_OFF || WiFi.isConnected()) {
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "startWifi() skipped (already running)");
        }
        return;
    }

    if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
        debugLogger.log(DebugLogCategory::Wifi, "startWifi() requested");
    }

    // Reset failure counter so user can retry after "gave up" state
    wifiManager.resetReconnectFailures();
    
    Serial.println("[WiFi] Starting WiFi (manual start)...");
    if (!wifiManager.begin()) {
        Serial.println("[WiFi] begin() failed (memory or radio)");
        return;
    }

    // Reduce WiFi TX power to minimize interference with BLE
    // 5dBm gives ~2-3m range, sufficient for in-car phone config
    WiFi.setTxPower(WIFI_POWER_5dBm);
    Serial.println("[WiFi] TX power 5dBm (low RF for BLE coex)");

    Serial.println("[WiFi] Initialized");
}

void WifiOrchestrator::configureCallbacks() {
    // V1 connection status
    wifiManager.setStatusCallback([this]() {
        JsonDocument doc;
        doc["v1_connected"] = bleClient.isConnected();
        String json;
        serializeJson(doc, json);
        return json;
    });

    // Current alert state
    wifiManager.setAlertCallback([this]() {
        JsonDocument doc;
        if (parser.hasAlerts()) {
            AlertData alert = parser.getPriorityAlert();
            doc["active"] = true;
            const char* bandStr = "None";
            if (alert.band == BAND_KA) bandStr = "Ka";
            else if (alert.band == BAND_K) bandStr = "K";
            else if (alert.band == BAND_X) bandStr = "X";
            else if (alert.band == BAND_LASER) bandStr = "LASER";
            doc["band"] = bandStr;
            doc["strength"] = alert.frontStrength;
            doc["frequency"] = alert.frequency;
            doc["direction"] = alert.direction;
        } else {
            doc["active"] = false;
        }
        String json;
        serializeJson(doc, json);
        return json;
    });

    // Command handling (dark mode / mute)
    wifiManager.setCommandCallback([this](const char* cmd, bool state) {
        if (strcmp(cmd, "display") == 0) {
            return bleClient.setDisplayOn(state);
        } else if (strcmp(cmd, "mute") == 0) {
            return bleClient.setMute(state);
        }
        return false;
    });

    // Filesystem for web APIs
    wifiManager.setFilesystemCallback([this]() -> fs::FS* {
        return storageManager.isReady() ? storageManager.getFilesystem() : nullptr;
    });

    // Manual profile push
    wifiManager.setProfilePushCallback([this]() {
        const V1Settings& s = settingsManager.get();
        if (profilePushFn) {
            profilePushFn(s.activeSlot);
            return true;
        }
        return false;
    });

    // Auto-push executor status
    wifiManager.setPushStatusCallback([this]() {
        return autoPushModule.getStatusJson();
    });

    // GPS status
    wifiManager.setGpsStatusCallback([this]() {
        JsonDocument doc;

        doc["enabled"] = gpsHandler.isEnabled();
        doc["moduleDetected"] = gpsHandler.isModuleDetected();
        doc["detectionComplete"] = gpsHandler.isDetectionComplete();
        doc["hasValidFix"] = gpsHandler.hasValidFix();

        if (gpsHandler.isEnabled() && gpsHandler.isModuleDetected()) {
            GPSFix fix = gpsHandler.getFix();
            doc["latitude"] = fix.latitude;
            doc["longitude"] = fix.longitude;
            doc["satellites"] = fix.satellites;
            doc["hdop"] = fix.hdop;
            doc["speed_mph"] = fix.speed_mps * 2.237f;
            doc["heading"] = fix.heading_deg;
            doc["fixValid"] = fix.valid;
            doc["fixStale"] = gpsHandler.isFixStale();
            doc["moving"] = gpsHandler.isMoving();

            if (fix.unixTime > 0) {
                doc["gpsTime"] = fix.unixTime;
                char timeStr[32];
                snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d UTC", fix.hour, fix.minute, fix.seconds);
                doc["gpsTimeStr"] = timeStr;
            }
        }

        String json;
        serializeJson(doc, json);
        return json;
    });

    // GPS reset
    wifiManager.setGpsResetCallback([this]() {
        gpsHandler.reset();
    });

    // V1 connection state (used to defer WiFi client operations until V1 is connected)
    wifiManager.setV1ConnectedCallback([this]() {
        return bleClient.isConnected();
    });
}

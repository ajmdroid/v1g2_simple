#include "wifi_orchestrator.h"

WifiOrchestrator::WifiOrchestrator(WiFiManager& wifiManager,
                                   DebugLogger& debugLogger,
                                   V1BLEClient& bleClient,
                                   PacketParser& parser,
                                   SettingsManager& settingsManager,
                                   StorageManager& storageManager,
                                   GPSHandler& gpsHandler,
                                   CameraManager& cameraManager,
                                                                     CameraAlertModule& cameraAlertModule,
                                                                     AutoPushModule& autoPushModule,
                                   std::function<void(int)> profilePushFn)
    : wifiManager(wifiManager),
      debugLogger(debugLogger),
      bleClient(bleClient),
      parser(parser),
      settingsManager(settingsManager),
      storageManager(storageManager),
      gpsHandler(gpsHandler),
      cameraManager(cameraManager),
      cameraAlertModule(cameraAlertModule),
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

    Serial.println("[WiFi] Starting WiFi (manual start)...");
    wifiManager.begin();

    // Reduce WiFi TX power to minimize interference with BLE
    WiFi.setTxPower(WIFI_POWER_11dBm);
    Serial.println("[WiFi] TX power reduced to 11dBm for BLE coexistence");

    Serial.println("[WiFi] Initialized");
}

void WifiOrchestrator::configureCallbacks() {
    // V1 connection status
    wifiManager.setStatusCallback([this]() {
        DynamicJsonDocument doc(64);
        doc["v1_connected"] = bleClient.isConnected();
        String json;
        serializeJson(doc, json);
        return json;
    });

    // Current alert state
    wifiManager.setAlertCallback([this]() {
        DynamicJsonDocument doc(192);
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
        DynamicJsonDocument doc(256);

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

    // Camera status
    wifiManager.setCameraStatusCallback([this]() {
        DynamicJsonDocument doc(256);

        doc["loaded"] = cameraManager.isLoaded();
        doc["count"] = cameraManager.getCameraCount();

        if (cameraManager.isLoaded()) {
            doc["name"] = cameraManager.getDatabaseName();
            doc["date"] = cameraManager.getDatabaseDate();
            doc["redLightCount"] = cameraManager.getRedLightCount();
            doc["speedCount"] = cameraManager.getSpeedCameraCount();
            doc["alprCount"] = cameraManager.getALPRCount();

            if (cameraManager.hasRegionalCache()) {
                JsonObject cache = doc["cache"].to<JsonObject>();
                float cacheLat, cacheLon;
                cameraManager.getCacheCenter(cacheLat, cacheLon);
                cache["count"] = cameraManager.getRegionalCacheCount();
                cache["centerLat"] = cacheLat;
                cache["centerLon"] = cacheLon;
                cache["radiusMiles"] = cameraManager.getCacheRadius();
            }
        }

        String json;
        serializeJson(doc, json);
        return json;
    });

    // Camera reload
    wifiManager.setCameraReloadCallback([this]() {
        if (!storageManager.isReady() || !storageManager.isSDCard()) {
            return false;
        }
        fs::FS* fs = storageManager.getFilesystem();
        if (!fs) return false;
        return cameraManager.begin(fs);
    });

    // Camera test
    wifiManager.setCameraTestCallback([this](int type) {
        cameraAlertModule.startTest(type);
    });
}

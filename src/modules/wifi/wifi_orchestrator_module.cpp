#include "wifi_orchestrator_module.h"

WifiOrchestrator::WifiOrchestrator(WiFiManager& wifiManager,
                                   V1BLEClient& bleClient,
                                   PacketParser& parser,
                                   SettingsManager& settingsManager,
                                   StorageManager& storageManager,
                                   AutoPushModule& autoPushModule,
                                   std::function<void(int)> profilePushFn)
    : wifiManager(wifiManager),
      bleClient(bleClient),
      parser(parser),
      settingsManager(settingsManager),
      storageManager(storageManager),
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
        return;
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

    // V1 connection state (used to defer WiFi client operations until V1 is connected)
    wifiManager.setV1ConnectedCallback([this]() {
        return bleClient.isConnected();
    });
}

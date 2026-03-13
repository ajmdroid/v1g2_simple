#include "obd_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>

#include "modules/obd/obd_runtime_module.h"
#include "modules/wifi/wifi_api_response.h"
#include "settings.h"

namespace ObdApiService {

void handleApiStatus(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) markUiActivity();
    ObdRuntimeStatus status = obdRuntime.snapshot(millis());
    JsonDocument doc;
    doc["enabled"] = status.enabled;
    doc["connected"] = status.connected;
    doc["speedValid"] = status.speedValid;
    doc["speedMph"] = status.speedMph;
    doc["speedAgeMs"] = status.speedAgeMs;
    doc["rssi"] = status.rssi;
    doc["scanInProgress"] = status.scanInProgress;
    doc["savedAddressValid"] = status.savedAddressValid;
    doc["connectAttempts"] = status.connectAttempts;
    doc["pollCount"] = status.pollCount;
    doc["pollErrors"] = status.pollErrors;
    doc["consecutiveErrors"] = status.consecutiveErrors;
    doc["state"] = static_cast<int>(status.state);
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiScan(WebServer& server,
                   ObdRuntimeModule& obdRuntime,
                   const std::function<bool()>& checkRateLimit,
                   const std::function<void()>& markUiActivity) {
    if (markUiActivity) markUiActivity();
    if (checkRateLimit && !checkRateLimit()) return;
    obdRuntime.startScan();
    JsonDocument doc;
    doc["ok"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiForget(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     SettingsManager& settingsManager,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) markUiActivity();
    if (checkRateLimit && !checkRateLimit()) return;
    obdRuntime.forgetDevice();
    V1Settings& settings = settingsManager.mutableSettings();
    settings.obdSavedAddress = "";
    settingsManager.save();
    JsonDocument doc;
    doc["ok"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiConfig(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     SettingsManager& settingsManager,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) markUiActivity();
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("plain") || server.arg("plain").length() == 0) {
        JsonDocument errDoc;
        WifiApiResponse::setErrorAndMessage(errDoc, "Missing JSON body");
        WifiApiResponse::sendJsonDocument(server, 400, errDoc);
        return;
    }

    JsonDocument body;
    DeserializationError err = deserializeJson(body, server.arg("plain"));
    if (err) {
        JsonDocument errDoc;
        WifiApiResponse::setErrorAndMessage(errDoc, "Invalid JSON");
        WifiApiResponse::sendJsonDocument(server, 400, errDoc);
        return;
    }

    V1Settings& settings = settingsManager.mutableSettings();
    bool changed = false;

    if (body["enabled"].is<bool>()) {
        bool enabled = body["enabled"].as<bool>();
        settings.obdEnabled = enabled;
        obdRuntime.setEnabled(enabled);
        changed = true;
    }
    if (!body["minRssi"].isNull()) {
        int rssi = body["minRssi"].as<int>();
        rssi = std::max(-90, std::min(rssi, -40));
        settings.obdMinRssi = static_cast<int8_t>(rssi);
        changed = true;
    }

    if (changed) {
        settingsManager.save();
    }

    JsonDocument doc;
    doc["ok"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

}  // namespace ObdApiService

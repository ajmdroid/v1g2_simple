#include "obd_api_service.h"

#include <ArduinoJson.h>

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
    if (checkRateLimit && checkRateLimit()) return;
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
    if (checkRateLimit && checkRateLimit()) return;
    obdRuntime.forgetDevice();
    V1Settings& settings = settingsManager.mutableSettings();
    settings.obdSavedAddress = "";
    settingsManager.save();
    JsonDocument doc;
    doc["ok"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

}  // namespace ObdApiService

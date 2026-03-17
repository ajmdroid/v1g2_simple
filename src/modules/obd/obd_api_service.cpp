#include "obd_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>

#include "modules/obd/obd_runtime_module.h"
#ifndef UNIT_TEST
#include "modules/speed/speed_source_selector.h"
#endif
#include "modules/wifi/wifi_api_response.h"
#include "settings.h"
#include "settings_runtime_sync.h"

namespace ObdApiService {

namespace {

const char* vehicleFamilyName(ObdVehicleFamily family) {
    switch (family) {
        case ObdVehicleFamily::FORD: return "ford";
        case ObdVehicleFamily::FCA: return "fca";
        case ObdVehicleFamily::VW_AUDI_PORSCHE: return "vw";
        case ObdVehicleFamily::UNKNOWN:
        default:
            return "unknown";
    }
}

const char* commandKindName(ObdCommandKind kind) {
    switch (kind) {
        case ObdCommandKind::AT_INIT: return "at_init";
        case ObdCommandKind::SANITY: return "sanity";
        case ObdCommandKind::SPEED: return "speed";
        case ObdCommandKind::VIN: return "vin";
        case ObdCommandKind::EOT_PROBE: return "eot_probe";
        case ObdCommandKind::EOT_POLL: return "eot_poll";
        case ObdCommandKind::NONE:
        default:
            return "none";
    }
}

}  // namespace

void handleApiConfigGet(WebServer& server,
                        SettingsManager& settingsManager,
                        const std::function<void()>& markUiActivity) {
    if (markUiActivity) markUiActivity();
    const V1Settings& settings = settingsManager.get();
    JsonDocument doc;
    doc["enabled"] = settings.obdEnabled;
    doc["minRssi"] = settings.obdMinRssi;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiStatus(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) markUiActivity();
    ObdRuntimeStatus status = obdRuntime.snapshot(millis());
    JsonDocument doc;
    doc["enabled"] = status.enabled;
    doc["connected"] = status.connected;
    doc["securityReady"] = status.securityReady;
    doc["encrypted"] = status.encrypted;
    doc["bonded"] = status.bonded;
    doc["speedValid"] = status.speedValid;
    doc["speedMph"] = status.speedMph;
    doc["speedAgeMs"] = status.speedAgeMs;
    doc["rssi"] = status.rssi;
    doc["scanInProgress"] = status.scanInProgress;
    doc["savedAddressValid"] = status.savedAddressValid;
    doc["connectAttempts"] = status.connectAttempts;
    doc["connectSuccesses"] = status.connectSuccesses;
    doc["connectFailures"] = status.connectFailures;
    doc["securityRepairs"] = status.securityRepairs;
    doc["initRetries"] = status.initRetries;
    doc["pollCount"] = status.pollCount;
    doc["pollErrors"] = status.pollErrors;
    doc["staleSpeedCount"] = status.staleSpeedCount;
    doc["consecutiveErrors"] = status.consecutiveErrors;
    doc["bufferOverflows"] = status.bufferOverflows;
    doc["commandInFlight"] = commandKindName(status.commandInFlight);
    doc["commandInFlightRaw"] = static_cast<int>(status.commandInFlight);
    doc["lastConnectStartMs"] = status.lastConnectStartMs;
    doc["lastConnectSuccessMs"] = status.lastConnectSuccessMs;
    doc["lastFailureMs"] = status.lastFailureMs;
    doc["lastBleError"] = status.lastBleError;
    doc["lastSecurityError"] = status.lastSecurityError;
    doc["lastFailureRaw"] = static_cast<int>(status.lastFailure);
    doc["vinDetected"] = status.vinDetected;
    doc["vin"] = status.vinDetected ? status.vin : "";
    doc["vehicleFamily"] = vehicleFamilyName(status.vehicleFamily);
    doc["vehicleFamilyRaw"] = static_cast<int>(status.vehicleFamily);
    doc["eotValid"] = status.eotValid;
    doc["eotC_x10"] = status.eotValid ? status.eotC_x10 : 0;
    if (status.eotValid) {
        doc["eotAgeMs"] = status.eotAgeMs;
    } else {
        doc["eotAgeMs"] = nullptr;
    }
    doc["eotProfileId"] = static_cast<int>(status.eotProfileId);
    doc["eotProbeFailures"] = status.eotProbeFailures;
    doc["cachedProfileActive"] = status.cachedProfileActive;
    doc["state"] = static_cast<int>(status.state);
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiScan(WebServer& server,
                   ObdRuntimeModule& obdRuntime,
                   const std::function<bool()>& checkRateLimit,
                   const std::function<void()>& markUiActivity) {
    if (markUiActivity) markUiActivity();
    if (checkRateLimit && !checkRateLimit()) return;
    if (!obdRuntime.isEnabled()) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["message"] = "OBD is disabled";
        WifiApiResponse::sendJsonDocument(server, 409, doc);
        return;
    }
    if (!obdRuntime.startScan()) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["message"] = "OBD scan already requested or in progress";
        WifiApiResponse::sendJsonDocument(server, 409, doc);
        return;
    }

    const ObdRuntimeStatus status = obdRuntime.snapshot(millis());
    JsonDocument doc;
    doc["ok"] = true;
    doc["requested"] = true;
    doc["scanInProgress"] = status.scanInProgress;
    doc["message"] = status.scanInProgress ? "OBD scan already running" : "OBD scan requested";
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
    settings.obdSavedAddrType = 0;
    settings.obdCachedVinPrefix11 = "";
    settings.obdCachedEotProfileId = 0;
    settingsManager.save();
    JsonDocument doc;
    doc["ok"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiConfig(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     SettingsManager& settingsManager,
                     SpeedSourceSelector& speedSourceSelector,
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

    const String requestBody = server.arg("plain");
    JsonDocument body;
    DeserializationError err = deserializeJson(body, requestBody);
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
        changed = true;
    }
    if (!body["minRssi"].isNull()) {
        int rssi = body["minRssi"].as<int>();
        rssi = std::max(-90, std::min(rssi, -40));
        settings.obdMinRssi = static_cast<int8_t>(rssi);
        changed = true;
    }

    if (changed) {
        SettingsRuntimeSync::syncObdVehicleRuntimeSettings(settings, obdRuntime, speedSourceSelector);
        settingsManager.save();
    }

    JsonDocument doc;
    doc["ok"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

}  // namespace ObdApiService

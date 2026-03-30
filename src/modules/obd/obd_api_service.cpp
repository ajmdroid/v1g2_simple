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

constexpr size_t MAX_OBD_DEVICE_NAME_LEN = 32;

bool isHex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

String normalizeObdDeviceAddress(const String& rawAddress) {
    String value = rawAddress;
    value.trim();
    value.replace("-", ":");
    value.toUpperCase();

    if (value.length() != 17) {
        return "";
    }

    for (int i = 0; i < 17; ++i) {
        const char c = value[i];
        if ((i + 1) % 3 == 0) {
            if (c != ':') {
                return "";
            }
            continue;
        }
        if (!isHex(c)) {
            return "";
        }
    }

    return value;
}

String sanitizeObdDeviceName(const String& raw) {
    String value = raw;
    value.trim();
    if (value.length() > MAX_OBD_DEVICE_NAME_LEN) {
        value = value.substring(0, MAX_OBD_DEVICE_NAME_LEN);
        value.trim();
    }
    return value;
}

}  // namespace

void handleApiConfigGet(WebServer& server,
                        SettingsManager& settingsManager,
                        void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (markUiActivity) markUiActivity(uiActivityCtx);
    const V1Settings& settings = settingsManager.get();
    JsonDocument doc;
    doc["enabled"] = settings.obdEnabled;
    doc["minRssi"] = settings.obdMinRssi;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiStatus(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (markUiActivity) markUiActivity(uiActivityCtx);
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
    doc["manualScanPending"] = status.manualScanPending;
    doc["savedAddressValid"] = status.savedAddressValid;
    doc["savedAddress"] = status.savedAddressValid ? String(obdRuntime.getSavedAddress()) : "";
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

void handleApiDevicesList(WebServer& server,
                          ObdRuntimeModule& obdRuntime,
                          SettingsManager& settingsManager,
                          void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (markUiActivity) markUiActivity(uiActivityCtx);

    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();

    const V1Settings& settings = settingsManager.get();
    const String address = normalizeObdDeviceAddress(settings.obdSavedAddress);
    if (address.length() > 0) {
        JsonObject obj = arr.add<JsonObject>();
        obj["address"] = address;
        obj["name"] = settings.obdSavedName;
        obj["connected"] = obdRuntime.snapshot(millis()).connected;
        obj["active"] = true;
    }

    doc["count"] = arr.size();
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiDeviceNameSave(WebServer& server,
                             SettingsManager& settingsManager,
                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                             void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (markUiActivity) markUiActivity(uiActivityCtx);
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;

    if (!server.hasArg("address")) {
        server.send(400, "application/json", "{\"error\":\"Missing address\"}");
        return;
    }

    const String requestedAddress = normalizeObdDeviceAddress(server.arg("address"));
    const String savedAddress = normalizeObdDeviceAddress(settingsManager.get().obdSavedAddress);
    if (requestedAddress.length() == 0 || savedAddress.length() == 0 || !requestedAddress.equalsIgnoreCase(savedAddress)) {
        server.send(404, "application/json", "{\"error\":\"Saved OBD device not found\"}");
        return;
    }

    ObdSettingsUpdate update;
    update.hasSavedName = true;
    update.savedName = sanitizeObdDeviceName(server.hasArg("name") ? server.arg("name") : "");
    settingsManager.applyObdSettingsUpdate(update, SettingsPersistMode::Deferred);

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiScan(WebServer& server,
                   ObdRuntimeModule& obdRuntime,
                   bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                   void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (markUiActivity) markUiActivity(uiActivityCtx);
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (!obdRuntime.isEnabled()) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["message"] = "OBD is disabled";
        WifiApiResponse::sendJsonDocument(server, 409, doc);
        return;
    }
    if (!obdRuntime.requestManualPairScan(millis())) {
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
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (markUiActivity) markUiActivity(uiActivityCtx);
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    obdRuntime.forgetDevice();
    ObdSettingsUpdate update;
    update.hasSavedAddress = true;
    update.savedAddress = "";
    update.hasSavedName = true;
    update.savedName = "";
    update.hasSavedAddrType = true;
    update.savedAddrType = 0;
    update.hasCachedVinPrefix11 = true;
    update.cachedVinPrefix11 = "";
    update.hasCachedEotProfileId = true;
    update.cachedEotProfileId = 0;
    settingsManager.applyObdSettingsUpdate(update, SettingsPersistMode::Deferred);
    JsonDocument doc;
    doc["ok"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiConfig(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     SettingsManager& settingsManager,
                     SpeedSourceSelector& speedSourceSelector,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (markUiActivity) markUiActivity(uiActivityCtx);
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;

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

    ObdSettingsUpdate update;

    if (body["enabled"].is<bool>()) {
        update.hasEnabled = true;
        update.enabled = body["enabled"].as<bool>();
    }
    if (!body["minRssi"].isNull()) {
        int rssi = body["minRssi"].as<int>();
        rssi = std::max(-90, std::min(rssi, -40));
        update.hasMinRssi = true;
        update.minRssi = static_cast<int8_t>(rssi);
    }

    const bool changed =
        settingsManager.applyObdSettingsUpdate(update, SettingsPersistMode::Deferred);
    if (changed) {
        SettingsRuntimeSync::syncObdVehicleRuntimeSettings(settingsManager.get(), obdRuntime, speedSourceSelector);
    }

    JsonDocument doc;
    doc["ok"] = true;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

}  // namespace ObdApiService

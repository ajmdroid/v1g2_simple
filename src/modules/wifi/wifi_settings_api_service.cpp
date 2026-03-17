#include "wifi_settings_api_service.h"

#include <ArduinoJson.h>

#include <algorithm>

#include "../../settings_sanitize.h"
#include "../gps/gps_lockout_safety.h"
#include "../../../include/clamp_utils.h"
#include "wifi_api_response.h"

namespace WifiSettingsApiService {

namespace {

bool argIsTrue(const String& value) {
    return value == "true" || value == "1";
}

void sendSettingsUnavailable(WebServer& server) {
    server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
}

}  // namespace

void handleApiDeviceSettingsGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        sendSettingsUnavailable(server);
        return;
    }

    const V1Settings& settings = runtime.getSettings();

    JsonDocument doc;
    doc["ap_ssid"] = settings.apSSID;
    doc["ap_password"] = "********";  // Don't send actual password
    doc["isDefaultPassword"] = (settings.apPassword == "setupv1g2");
    doc["proxy_ble"] = settings.proxyBLE;
    doc["proxy_name"] = settings.proxyName;
    doc["autoPowerOffMinutes"] = settings.autoPowerOffMinutes;
    doc["apTimeoutMinutes"] = settings.apTimeoutMinutes;
    doc["enableWifiAtBoot"] = settings.enableWifiAtBoot;
    doc["enableSignalTraceLogging"] = settings.enableSignalTraceLogging;

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiDeviceSettingsSave(WebServer& server,
                                 const Runtime& runtime,
                                 const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!runtime.getMutableSettings) {
        sendSettingsUnavailable(server);
        return;
    }

    V1Settings& mutableSettings = runtime.getMutableSettings();
    const V1Settings& currentSettings = mutableSettings;

    if (server.hasArg("ap_ssid")) {
        String apSsid = clampStringLength(server.arg("ap_ssid"), MAX_WIFI_SSID_LEN);
        String apPass = server.arg("ap_password");
        if (apPass.length() > MAX_AP_PASSWORD_LEN && apPass != "********") {
            apPass = apPass.substring(0, MAX_AP_PASSWORD_LEN);
        }

        // If password is placeholder, keep existing password
        if (apPass == "********") {
            apPass = currentSettings.apPassword;
        }

        if (apSsid.length() == 0 || apPass.length() < 8) {
            server.send(400, "application/json", "{\"error\":\"AP SSID required and password must be at least 8 characters\"}");
            return;
        }

        if (runtime.updateAPCredentials) {
            runtime.updateAPCredentials(apSsid, apPass);
        }
    }

    if (server.hasArg("proxy_ble")) {
        bool proxyEnabled = argIsTrue(server.arg("proxy_ble"));
        mutableSettings.proxyBLE = proxyEnabled;
    }
    if (server.hasArg("proxy_name")) {
        mutableSettings.proxyName = sanitizeProxyNameValue(server.arg("proxy_name"));
    }
    if (server.hasArg("autoPowerOffMinutes")) {
        int minutes = server.arg("autoPowerOffMinutes").toInt();
        minutes = std::max(0, std::min(minutes, 60));
        mutableSettings.autoPowerOffMinutes = static_cast<uint8_t>(minutes);
    }
    if (server.hasArg("apTimeoutMinutes")) {
        int minutes = server.arg("apTimeoutMinutes").toInt();
        if (minutes != 0) {
            minutes = std::max(5, std::min(minutes, 60));
        }
        mutableSettings.apTimeoutMinutes = static_cast<uint8_t>(minutes);
    }
    if (server.hasArg("enableWifiAtBoot")) {
        mutableSettings.enableWifiAtBoot = argIsTrue(server.arg("enableWifiAtBoot"));
    }
    if (server.hasArg("enableSignalTraceLogging")) {
        mutableSettings.enableSignalTraceLogging =
            argIsTrue(server.arg("enableSignalTraceLogging"));
    }

    if (runtime.save) {
        runtime.save();
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiSettingsSave(WebServer& server,
                           const Runtime& runtime,
                           const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!runtime.getMutableSettings) {
        sendSettingsUnavailable(server);
        return;
    }

    Serial.println("=== handleSettingsSave() called ===");

    V1Settings& mutableSettings = runtime.getMutableSettings();

    if (server.hasArg("gpsEnabled")) {
        mutableSettings.gpsEnabled = argIsTrue(server.arg("gpsEnabled"));
        if (runtime.setGpsRuntimeEnabled) {
            runtime.setGpsRuntimeEnabled(mutableSettings.gpsEnabled);
        }
        if (runtime.setSpeedSourceGpsEnabled) {
            runtime.setSpeedSourceGpsEnabled(mutableSettings.gpsEnabled);
        }
    }
    if (server.hasArg("gpsLockoutMode")) {
        mutableSettings.gpsLockoutMode = gpsLockoutParseRuntimeModeArg(server.arg("gpsLockoutMode"),
                                                                       mutableSettings.gpsLockoutMode);
    }
    if (server.hasArg("gpsLockoutCoreGuardEnabled")) {
        mutableSettings.gpsLockoutCoreGuardEnabled =
            (server.arg("gpsLockoutCoreGuardEnabled") == "true" ||
             server.arg("gpsLockoutCoreGuardEnabled") == "1");
    }
    if (server.hasArg("gpsLockoutMaxQueueDrops")) {
        mutableSettings.gpsLockoutMaxQueueDrops =
            clamp_utils::clampU16Value(server.arg("gpsLockoutMaxQueueDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutMaxPerfDrops")) {
        mutableSettings.gpsLockoutMaxPerfDrops =
            clamp_utils::clampU16Value(server.arg("gpsLockoutMaxPerfDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutMaxEventBusDrops")) {
        mutableSettings.gpsLockoutMaxEventBusDrops =
            clamp_utils::clampU16Value(server.arg("gpsLockoutMaxEventBusDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutKaLearningEnabled")) {
        mutableSettings.gpsLockoutKaLearningEnabled =
            argIsTrue(server.arg("gpsLockoutKaLearningEnabled"));
        if (runtime.setLockoutKaLearningEnabled) {
            runtime.setLockoutKaLearningEnabled(mutableSettings.gpsLockoutKaLearningEnabled);
        }
    }
    if (server.hasArg("gpsLockoutKLearningEnabled")) {
        mutableSettings.gpsLockoutKLearningEnabled =
            argIsTrue(server.arg("gpsLockoutKLearningEnabled"));
        if (runtime.setLockoutKLearningEnabled) {
            runtime.setLockoutKLearningEnabled(mutableSettings.gpsLockoutKLearningEnabled);
        }
    }
    if (server.hasArg("gpsLockoutXLearningEnabled")) {
        mutableSettings.gpsLockoutXLearningEnabled =
            argIsTrue(server.arg("gpsLockoutXLearningEnabled"));
        if (runtime.setLockoutXLearningEnabled) {
            runtime.setLockoutXLearningEnabled(mutableSettings.gpsLockoutXLearningEnabled);
        }
    }
    if (server.hasArg("gpsLockoutPreQuiet")) {
        mutableSettings.gpsLockoutPreQuiet = argIsTrue(server.arg("gpsLockoutPreQuiet"));
    }
    if (server.hasArg("gpsLockoutPreQuietBufferE5")) {
        mutableSettings.gpsLockoutPreQuietBufferE5 = clampLockoutPreQuietBufferE5Value(
            server.arg("gpsLockoutPreQuietBufferE5").toInt());
    }

    // All changes are queued in the settingsManager instance. Now, save them all at once.
    Serial.println("--- Calling settingsManager.save() ---");
    if (runtime.save) {
        runtime.save();
    }

    server.send(200, "application/json", "{\"success\":true}");
}

}  // namespace WifiSettingsApiService

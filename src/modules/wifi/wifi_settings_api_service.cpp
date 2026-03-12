#include "wifi_settings_api_service.h"

#include <ArduinoJson.h>

#include <algorithm>

#include "../../settings_sanitize.h"
#include "../gps/gps_lockout_safety.h"
#include "../../../include/clamp_utils.h"
#include "wifi_api_response.h"

namespace WifiSettingsApiService {

void handleApiSettingsGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    const V1Settings& settings = runtime.getSettings();

    JsonDocument doc;
    doc["ap_ssid"] = settings.apSSID;
    doc["ap_password"] = "********";  // Don't send actual password
    doc["isDefaultPassword"] = (settings.apPassword == "setupv1g2");  // Security warning flag
    doc["proxy_ble"] = settings.proxyBLE;
    doc["proxy_name"] = settings.proxyName;
    doc["gpsEnabled"] = settings.gpsEnabled;
    doc["gpsLockoutMode"] = static_cast<int>(settings.gpsLockoutMode);
    doc["gpsLockoutModeName"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    doc["gpsLockoutCoreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    doc["gpsLockoutMaxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    doc["gpsLockoutMaxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    doc["gpsLockoutMaxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    doc["gpsLockoutKaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    doc["gpsLockoutPreQuiet"] = settings.gpsLockoutPreQuiet;
    doc["gpsLockoutPreQuietBufferE5"] = settings.gpsLockoutPreQuietBufferE5;
    doc["displayStyle"] = static_cast<int>(settings.displayStyle);
    doc["autoPowerOffMinutes"] = settings.autoPowerOffMinutes;
    doc["apTimeoutMinutes"] = settings.apTimeoutMinutes;

    // Development settings
    doc["enableWifiAtBoot"] = settings.enableWifiAtBoot;
    doc["enableSignalTraceLogging"] = settings.enableSignalTraceLogging;

    // OBD settings
    doc["obdEnabled"] = settings.obdEnabled;
    doc["obdMinRssi"] = settings.obdMinRssi;

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiSettingsSave(WebServer& server,
                           const Runtime& runtime,
                           const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!runtime.getMutableSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    Serial.println("=== handleSettingsSave() called ===");

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

    if (server.hasArg("brightness")) {
        int brightness = server.arg("brightness").toInt();
        brightness = std::max(0, std::min(brightness, 255));
        if (runtime.updateBrightness) {
            runtime.updateBrightness(static_cast<uint8_t>(brightness));
        } else {
            mutableSettings.brightness = static_cast<uint8_t>(brightness);
        }
    }

    // BLE proxy settings
    if (server.hasArg("proxy_ble")) {
        bool proxyEnabled = server.arg("proxy_ble") == "true" || server.arg("proxy_ble") == "1";
        mutableSettings.proxyBLE = proxyEnabled;
    }
    if (server.hasArg("proxy_name")) {
        mutableSettings.proxyName = sanitizeProxyNameValue(server.arg("proxy_name"));
    }
    if (server.hasArg("gpsEnabled")) {
        mutableSettings.gpsEnabled =
            (server.arg("gpsEnabled") == "true" || server.arg("gpsEnabled") == "1");
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
            (server.arg("gpsLockoutKaLearningEnabled") == "true" ||
             server.arg("gpsLockoutKaLearningEnabled") == "1");
        if (runtime.setLockoutKaLearningEnabled) {
            runtime.setLockoutKaLearningEnabled(mutableSettings.gpsLockoutKaLearningEnabled);
        }
    }
    if (server.hasArg("gpsLockoutKLearningEnabled")) {
        mutableSettings.gpsLockoutKLearningEnabled =
            (server.arg("gpsLockoutKLearningEnabled") == "true" ||
             server.arg("gpsLockoutKLearningEnabled") == "1");
        if (runtime.setLockoutKLearningEnabled) {
            runtime.setLockoutKLearningEnabled(mutableSettings.gpsLockoutKLearningEnabled);
        }
    }
    if (server.hasArg("gpsLockoutXLearningEnabled")) {
        mutableSettings.gpsLockoutXLearningEnabled =
            (server.arg("gpsLockoutXLearningEnabled") == "true" ||
             server.arg("gpsLockoutXLearningEnabled") == "1");
        if (runtime.setLockoutXLearningEnabled) {
            runtime.setLockoutXLearningEnabled(mutableSettings.gpsLockoutXLearningEnabled);
        }
    }
    if (server.hasArg("gpsLockoutPreQuiet")) {
        mutableSettings.gpsLockoutPreQuiet =
            (server.arg("gpsLockoutPreQuiet") == "true" ||
             server.arg("gpsLockoutPreQuiet") == "1");
    }
    if (server.hasArg("gpsLockoutPreQuietBufferE5")) {
        mutableSettings.gpsLockoutPreQuietBufferE5 = clampLockoutPreQuietBufferE5Value(
            server.arg("gpsLockoutPreQuietBufferE5").toInt());
    }
    if (server.hasArg("autoPowerOffMinutes")) {
        int minutes = server.arg("autoPowerOffMinutes").toInt();
        minutes = std::max(0, std::min(minutes, 60));  // Clamp 0-60 minutes
        mutableSettings.autoPowerOffMinutes = static_cast<uint8_t>(minutes);
    }
    if (server.hasArg("apTimeoutMinutes")) {
        int minutes = server.arg("apTimeoutMinutes").toInt();
        // Clamp: 0=always on, or 5-60 minutes
        if (minutes != 0) {
            minutes = std::max(5, std::min(minutes, 60));
        }
        mutableSettings.apTimeoutMinutes = static_cast<uint8_t>(minutes);
    }
    if (server.hasArg("enableWifiAtBoot")) {
        mutableSettings.enableWifiAtBoot =
            (server.arg("enableWifiAtBoot") == "true" || server.arg("enableWifiAtBoot") == "1");
    }
    if (server.hasArg("enableSignalTraceLogging")) {
        mutableSettings.enableSignalTraceLogging =
            (server.arg("enableSignalTraceLogging") == "true" ||
             server.arg("enableSignalTraceLogging") == "1");
    }

    // OBD settings
    if (server.hasArg("obdEnabled")) {
        mutableSettings.obdEnabled =
            (server.arg("obdEnabled") == "true" || server.arg("obdEnabled") == "1");
        if (runtime.setObdRuntimeEnabled) {
            runtime.setObdRuntimeEnabled(mutableSettings.obdEnabled);
        }
        if (runtime.setSpeedSourceObdEnabled) {
            runtime.setSpeedSourceObdEnabled(mutableSettings.obdEnabled);
        }
    }
    if (server.hasArg("obdMinRssi")) {
        int rssi = server.arg("obdMinRssi").toInt();
        rssi = std::max(-90, std::min(rssi, -40));
        mutableSettings.obdMinRssi = static_cast<int8_t>(rssi);
    }

    // Display style setting
    if (server.hasArg("displayStyle")) {
        DisplayStyle style = normalizeDisplayStyle(server.arg("displayStyle").toInt());
        if (runtime.updateDisplayStyle) {
            runtime.updateDisplayStyle(style);
        } else {
            mutableSettings.displayStyle = style;
        }
        if (runtime.forceDisplayRedraw) {
            runtime.forceDisplayRedraw();
        }
    }

    // All changes are queued in the settingsManager instance. Now, save them all at once.
    Serial.println("--- Calling settingsManager.save() ---");
    if (runtime.save) {
        runtime.save();
    }

    server.send(200, "application/json", "{\"success\":true}");
}

}  // namespace WifiSettingsApiService

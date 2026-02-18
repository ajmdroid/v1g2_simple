#include "wifi_settings_api_service.h"

#include <ArduinoJson.h>

#include <algorithm>

#include "../../settings_sanitize.h"
#include "../gps/gps_lockout_safety.h"

#ifdef UNIT_TEST
#include <string>
#endif

namespace WifiSettingsApiService {

namespace {

void sendJsonDocument(WebServer& server, int statusCode, const JsonDocument& doc) {
#ifdef UNIT_TEST
    std::string response;
    serializeJson(doc, response);
    server.send(statusCode, "application/json", response.c_str());
#else
    String response;
    serializeJson(doc, response);
    server.send(statusCode, "application/json", response);
#endif
}

uint16_t clampU16Value(int value, int minVal, int maxVal) {
    return static_cast<uint16_t>(std::max(minVal, std::min(value, maxVal)));
}

bool computeCameraRuntimeEnabled(const V1Settings& settings) {
    return settings.cameraEnabled;
}

}  // namespace

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
    doc["obdEnabled"] = settings.obdEnabled;
    doc["obdVwDataEnabled"] = settings.obdVwDataEnabled;
    doc["gpsEnabled"] = settings.gpsEnabled;
    doc["cameraEnabled"] = settings.cameraEnabled;
    doc["gpsLockoutMode"] = static_cast<int>(settings.gpsLockoutMode);
    doc["gpsLockoutModeName"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    doc["gpsLockoutCoreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    doc["gpsLockoutMaxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    doc["gpsLockoutMaxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    doc["gpsLockoutMaxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    doc["gpsLockoutKaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    doc["displayStyle"] = static_cast<int>(settings.displayStyle);
    doc["autoPowerOffMinutes"] = settings.autoPowerOffMinutes;
    doc["apTimeoutMinutes"] = settings.apTimeoutMinutes;

    // Development settings
    doc["enableWifiAtBoot"] = settings.enableWifiAtBoot;
    doc["enableSignalTraceLogging"] = settings.enableSignalTraceLogging;

    sendJsonDocument(server, 200, doc);
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
    if (server.hasArg("obdVwDataEnabled")) {
        mutableSettings.obdVwDataEnabled =
            (server.arg("obdVwDataEnabled") == "true" || server.arg("obdVwDataEnabled") == "1");
        if (runtime.setObdVwDataEnabled) {
            runtime.setObdVwDataEnabled(mutableSettings.obdVwDataEnabled);
        }
    }
    if (server.hasArg("obdEnabled")) {
        mutableSettings.obdEnabled =
            (server.arg("obdEnabled") == "true" || server.arg("obdEnabled") == "1");
        if (!mutableSettings.obdEnabled) {
            if (runtime.stopObdScan) {
                runtime.stopObdScan();
            }
            if (runtime.disconnectObd) {
                runtime.disconnectObd();
            }
        }
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
    if (server.hasArg("cameraEnabled")) {
        mutableSettings.cameraEnabled =
            (server.arg("cameraEnabled") == "true" || server.arg("cameraEnabled") == "1");
    }
    if (server.hasArg("gpsEnabled") || server.hasArg("cameraEnabled")) {
        if (runtime.setCameraRuntimeEnabled) {
            runtime.setCameraRuntimeEnabled(computeCameraRuntimeEnabled(mutableSettings));
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
            clampU16Value(server.arg("gpsLockoutMaxQueueDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutMaxPerfDrops")) {
        mutableSettings.gpsLockoutMaxPerfDrops =
            clampU16Value(server.arg("gpsLockoutMaxPerfDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutMaxEventBusDrops")) {
        mutableSettings.gpsLockoutMaxEventBusDrops =
            clampU16Value(server.arg("gpsLockoutMaxEventBusDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutKaLearningEnabled")) {
        mutableSettings.gpsLockoutKaLearningEnabled =
            (server.arg("gpsLockoutKaLearningEnabled") == "true" ||
             server.arg("gpsLockoutKaLearningEnabled") == "1");
        if (runtime.setLockoutKaLearningEnabled) {
            runtime.setLockoutKaLearningEnabled(mutableSettings.gpsLockoutKaLearningEnabled);
        }
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

void handleApiLegacySettingsSave(WebServer& server,
                                 const Runtime& runtime,
                                 const std::function<bool()>& checkRateLimit,
                                 const std::function<void()>& sendDeprecatedHeader,
                                 const std::function<void()>& logLegacyUsage) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (logLegacyUsage) {
        logLegacyUsage();
    }
    if (sendDeprecatedHeader) {
        sendDeprecatedHeader();
    }

    // Preserve legacy behavior: route-level guard + delegate-level guard.
    handleApiSettingsSave(server, runtime, checkRateLimit);
}

}  // namespace WifiSettingsApiService

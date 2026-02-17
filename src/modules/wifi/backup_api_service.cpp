#include "backup_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>

#include "../../settings.h"
#include "../../settings_sanitize.h"
#include "../../v1_profiles.h"
#include "../../obd_handler.h"
#include "../gps/gps_runtime_module.h"
#include "../gps/gps_lockout_safety.h"
#include "../lockout/lockout_band_policy.h"
#include "../speed/speed_source_selector.h"
#include "../camera/camera_runtime_module.h"

namespace {

uint8_t clampU8Value(int value, int minVal, int maxVal) {
    if (value < minVal) return static_cast<uint8_t>(minVal);
    if (value > maxVal) return static_cast<uint8_t>(maxVal);
    return static_cast<uint8_t>(value);
}

uint16_t clampU16Value(int value, int minVal, int maxVal) {
    if (value < minVal) return static_cast<uint16_t>(minVal);
    if (value > maxVal) return static_cast<uint16_t>(maxVal);
    return static_cast<uint16_t>(value);
}

bool computeCameraRuntimeEnabled(const V1Settings& settings) {
    return settings.gpsEnabled && settings.cameraEnabled;
}

}  // anonymous namespace

namespace BackupApiService {

static void sendBackup(WebServer& server) {
    Serial.println("[HTTP] GET /api/settings/backup");
    
    const V1Settings& s = settingsManager.get();
    JsonDocument doc;
    
    // Metadata
    doc["_version"] = 5;  // Backup format version
    doc["_type"] = "v1simple_backup";
    doc["_timestamp"] = millis();
    
    // WiFi settings (exclude password for security)
    doc["apSSID"] = s.apSSID;
    // Note: password not included in backup for security
    
    // BLE settings
    doc["proxyBLE"] = s.proxyBLE;
    doc["proxyName"] = s.proxyName;
    doc["obdEnabled"] = s.obdEnabled;
    doc["obdVwDataEnabled"] = s.obdVwDataEnabled;
    doc["gpsEnabled"] = s.gpsEnabled;
    doc["cameraEnabled"] = s.cameraEnabled;
    doc["gpsLockoutMode"] = static_cast<int>(s.gpsLockoutMode);
    doc["gpsLockoutCoreGuardEnabled"] = s.gpsLockoutCoreGuardEnabled;
    doc["gpsLockoutMaxQueueDrops"] = s.gpsLockoutMaxQueueDrops;
    doc["gpsLockoutMaxPerfDrops"] = s.gpsLockoutMaxPerfDrops;
    doc["gpsLockoutMaxEventBusDrops"] = s.gpsLockoutMaxEventBusDrops;
    
    // Display settings
    doc["brightness"] = s.brightness;
    doc["displayStyle"] = (int)s.displayStyle;
    doc["turnOffDisplay"] = s.turnOffDisplay;
    
    // All colors (RGB565)
    doc["colorBogey"] = s.colorBogey;
    doc["colorFrequency"] = s.colorFrequency;
    doc["colorArrowFront"] = s.colorArrowFront;
    doc["colorArrowSide"] = s.colorArrowSide;
    doc["colorArrowRear"] = s.colorArrowRear;
    doc["colorBandL"] = s.colorBandL;
    doc["colorBandKa"] = s.colorBandKa;
    doc["colorBandK"] = s.colorBandK;
    doc["colorBandX"] = s.colorBandX;
    doc["colorBandPhoto"] = s.colorBandPhoto;
    doc["colorWiFiIcon"] = s.colorWiFiIcon;
    doc["colorBleConnected"] = s.colorBleConnected;
    doc["colorBleDisconnected"] = s.colorBleDisconnected;
    doc["colorBar1"] = s.colorBar1;
    doc["colorBar2"] = s.colorBar2;
    doc["colorBar3"] = s.colorBar3;
    doc["colorBar4"] = s.colorBar4;
    doc["colorBar5"] = s.colorBar5;
    doc["colorBar6"] = s.colorBar6;
    doc["colorMuted"] = s.colorMuted;
    doc["colorPersisted"] = s.colorPersisted;
    doc["colorVolumeMain"] = s.colorVolumeMain;
    doc["colorVolumeMute"] = s.colorVolumeMute;
    doc["colorWiFiConnected"] = s.colorWiFiConnected;
    doc["colorRssiV1"] = s.colorRssiV1;
    doc["colorRssiProxy"] = s.colorRssiProxy;
    doc["freqUseBandColor"] = s.freqUseBandColor;
    
    // Display visibility
    doc["hideWifiIcon"] = s.hideWifiIcon;
    doc["hideProfileIndicator"] = s.hideProfileIndicator;
    doc["hideBatteryIcon"] = s.hideBatteryIcon;
    doc["showBatteryPercent"] = s.showBatteryPercent;
    doc["hideBleIcon"] = s.hideBleIcon;
    doc["hideVolumeIndicator"] = s.hideVolumeIndicator;
    doc["hideRssiIndicator"] = s.hideRssiIndicator;
    doc["showRestTelemetryCards"] = s.showRestTelemetryCards;
    
    // Development
    doc["enableWifiAtBoot"] = s.enableWifiAtBoot;
    
    // WiFi client settings
    doc["wifiMode"] = (int)s.wifiMode;
    doc["wifiClientEnabled"] = s.wifiClientEnabled;
    doc["wifiClientSSID"] = s.wifiClientSSID;
    
    // Auto power-off
    doc["autoPowerOffMinutes"] = s.autoPowerOffMinutes;
    doc["apTimeoutMinutes"] = s.apTimeoutMinutes;
    
    // Voice settings
    doc["voiceAlertMode"] = (int)s.voiceAlertMode;
    doc["voiceDirectionEnabled"] = s.voiceDirectionEnabled;
    doc["announceBogeyCount"] = s.announceBogeyCount;
    doc["muteVoiceIfVolZero"] = s.muteVoiceIfVolZero;
    doc["voiceVolume"] = s.voiceVolume;
    doc["announceSecondaryAlerts"] = s.announceSecondaryAlerts;
    doc["secondaryLaser"] = s.secondaryLaser;
    doc["secondaryKa"] = s.secondaryKa;
    doc["secondaryK"] = s.secondaryK;
    doc["secondaryX"] = s.secondaryX;
    doc["alertVolumeFadeEnabled"] = s.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = s.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = s.alertVolumeFadeVolume;
    doc["speedVolumeEnabled"] = s.speedVolumeEnabled;
    doc["speedVolumeThresholdMph"] = s.speedVolumeThresholdMph;
    doc["speedVolumeBoost"] = s.speedVolumeBoost;
    doc["lowSpeedMuteEnabled"] = s.lowSpeedMuteEnabled;
    doc["lowSpeedMuteThresholdMph"] = s.lowSpeedMuteThresholdMph;
    
    // Auto-push slot settings
    doc["autoPushEnabled"] = s.autoPushEnabled;
    doc["activeSlot"] = s.activeSlot;
    doc["slot0Name"] = s.slot0Name;
    doc["slot1Name"] = s.slot1Name;
    doc["slot2Name"] = s.slot2Name;
    doc["slot0Color"] = s.slot0Color;
    doc["slot1Color"] = s.slot1Color;
    doc["slot2Color"] = s.slot2Color;
    doc["slot0Volume"] = s.slot0Volume;
    doc["slot1Volume"] = s.slot1Volume;
    doc["slot2Volume"] = s.slot2Volume;
    doc["slot0MuteVolume"] = s.slot0MuteVolume;
    doc["slot1MuteVolume"] = s.slot1MuteVolume;
    doc["slot2MuteVolume"] = s.slot2MuteVolume;
    doc["slot0DarkMode"] = s.slot0DarkMode;
    doc["slot1DarkMode"] = s.slot1DarkMode;
    doc["slot2DarkMode"] = s.slot2DarkMode;
    doc["slot0MuteToZero"] = s.slot0MuteToZero;
    doc["slot1MuteToZero"] = s.slot1MuteToZero;
    doc["slot2MuteToZero"] = s.slot2MuteToZero;
    doc["slot0AlertPersist"] = s.slot0AlertPersist;
    doc["slot1AlertPersist"] = s.slot1AlertPersist;
    doc["slot2AlertPersist"] = s.slot2AlertPersist;
    doc["slot0PriorityArrow"] = s.slot0PriorityArrow;
    doc["slot1PriorityArrow"] = s.slot1PriorityArrow;
    doc["slot2PriorityArrow"] = s.slot2PriorityArrow;
    doc["slot0ProfileName"] = s.slot0_default.profileName;
    doc["slot0Mode"] = s.slot0_default.mode;
    doc["slot1ProfileName"] = s.slot1_highway.profileName;
    doc["slot1Mode"] = s.slot1_highway.mode;
    doc["slot2ProfileName"] = s.slot2_comfort.profileName;
    doc["slot2Mode"] = s.slot2_comfort.mode;
    
    // V1 Profiles backup
    JsonArray profilesArr = doc["profiles"].to<JsonArray>();
    std::vector<String> profileNames = v1ProfileManager.listProfiles();
    for (const String& name : profileNames) {
        V1Profile profile;
        if (v1ProfileManager.loadProfile(name, profile)) {
            JsonObject p = profilesArr.add<JsonObject>();
            p["name"] = profile.name;
            p["description"] = profile.description;
            p["displayOn"] = profile.displayOn;
            p["mainVolume"] = profile.mainVolume;
            p["mutedVolume"] = profile.mutedVolume;
            // Store raw bytes array
            JsonArray bytes = p["bytes"].to<JsonArray>();
            for (int i = 0; i < 6; i++) {
                bytes.add(profile.settings.bytes[i]);
            }
        }
    }
    
    String json;
    serializeJsonPretty(doc, json);
    
    // Send with Content-Disposition header for download
    server.sendHeader("Content-Disposition", "attachment; filename=\"v1simple_backup.json\"");
    server.send(200, "application/json", json);
}

void handleApiBackup(WebServer& server,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    sendBackup(server);
}

static void handleRestore(WebServer& server) {
    Serial.println("[HTTP] POST /api/settings/restore");
    static constexpr size_t kMaxRestoreBodyBytes = 16 * 1024;
    
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"No JSON body provided\"}");
        return;
    }

    if (server.hasHeader("Content-Length")) {
        long declaredLength = server.header("Content-Length").toInt();
        if (declaredLength < 0 || static_cast<size_t>(declaredLength) > kMaxRestoreBodyBytes) {
            server.send(413, "application/json", "{\"success\":false,\"error\":\"Body too large\"}");
            return;
        }
    }
    
    String body = server.arg("plain");
    if (body.length() > kMaxRestoreBodyBytes) {
        server.send(413, "application/json", "{\"success\":false,\"error\":\"Body too large\"}");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    
    if (err) {
        Serial.printf("[Settings] Restore parse error: %s\n", err.c_str());
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    
    // Verify backup format
    if (!doc["_type"].is<const char*>() || String(doc["_type"].as<const char*>()) != "v1simple_backup") {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid backup format\"}");
        return;
    }
    
    // Restore settings (same logic as restoreFromSD but from JSON body)
    V1Settings& s = settingsManager.mutableSettings();
    
    // BLE settings
    if (doc["proxyBLE"].is<bool>()) s.proxyBLE = doc["proxyBLE"];
    if (doc["proxyName"].is<const char*>()) s.proxyName = sanitizeProxyNameValue(doc["proxyName"].as<String>());
    if (doc["obdEnabled"].is<bool>()) s.obdEnabled = doc["obdEnabled"];
    if (doc["obdVwDataEnabled"].is<bool>()) s.obdVwDataEnabled = doc["obdVwDataEnabled"];
    if (doc["gpsEnabled"].is<bool>()) s.gpsEnabled = doc["gpsEnabled"];
    if (doc["cameraEnabled"].is<bool>()) s.cameraEnabled = doc["cameraEnabled"];
    if (doc["gpsLockoutMode"].is<int>()) {
        s.gpsLockoutMode = clampLockoutRuntimeModeValue(doc["gpsLockoutMode"].as<int>());
    } else if (doc["gpsLockoutMode"].is<const char*>()) {
        s.gpsLockoutMode = gpsLockoutParseRuntimeModeArg(doc["gpsLockoutMode"].as<String>(),
                                                         s.gpsLockoutMode);
    }
    if (doc["gpsLockoutCoreGuardEnabled"].is<bool>()) {
        s.gpsLockoutCoreGuardEnabled = doc["gpsLockoutCoreGuardEnabled"];
    }
    if (doc["gpsLockoutMaxQueueDrops"].is<int>()) {
        s.gpsLockoutMaxQueueDrops = clampU16Value(doc["gpsLockoutMaxQueueDrops"].as<int>(), 0, 65535);
    }
    if (doc["gpsLockoutMaxPerfDrops"].is<int>()) {
        s.gpsLockoutMaxPerfDrops = clampU16Value(doc["gpsLockoutMaxPerfDrops"].as<int>(), 0, 65535);
    }
    if (doc["gpsLockoutMaxEventBusDrops"].is<int>()) {
        s.gpsLockoutMaxEventBusDrops = clampU16Value(doc["gpsLockoutMaxEventBusDrops"].as<int>(), 0, 65535);
    }
    if (doc["gpsLockoutKaLearningEnabled"].is<bool>()) {
        s.gpsLockoutKaLearningEnabled = doc["gpsLockoutKaLearningEnabled"];
    }
    
    // WiFi settings (password intentionally excluded from backups)
    if (doc["apSSID"].is<const char*>()) {
        // Preserve existing password while restoring SSID
        settingsManager.updateAPCredentials(sanitizeApSsidValue(doc["apSSID"].as<String>()), settingsManager.get().apPassword);
    }
    
    // Display settings
    if (doc["brightness"].is<int>()) s.brightness = clampU8Value(doc["brightness"].as<int>(), 1, 255);
    if (doc["displayStyle"].is<int>()) s.displayStyle = normalizeDisplayStyle(doc["displayStyle"].as<int>());
    if (doc["turnOffDisplay"].is<bool>()) s.turnOffDisplay = doc["turnOffDisplay"];
    
    // All colors
    if (doc["colorBogey"].is<int>()) s.colorBogey = doc["colorBogey"];
    if (doc["colorFrequency"].is<int>()) s.colorFrequency = doc["colorFrequency"];
    if (doc["colorArrowFront"].is<int>()) s.colorArrowFront = doc["colorArrowFront"];
    if (doc["colorArrowSide"].is<int>()) s.colorArrowSide = doc["colorArrowSide"];
    if (doc["colorArrowRear"].is<int>()) s.colorArrowRear = doc["colorArrowRear"];
    if (doc["colorBandL"].is<int>()) s.colorBandL = doc["colorBandL"];
    if (doc["colorBandKa"].is<int>()) s.colorBandKa = doc["colorBandKa"];
    if (doc["colorBandK"].is<int>()) s.colorBandK = doc["colorBandK"];
    if (doc["colorBandX"].is<int>()) s.colorBandX = doc["colorBandX"];
    if (doc["colorBandPhoto"].is<int>()) s.colorBandPhoto = doc["colorBandPhoto"];
    if (doc["colorWiFiIcon"].is<int>()) s.colorWiFiIcon = doc["colorWiFiIcon"];
    if (doc["colorBleConnected"].is<int>()) s.colorBleConnected = doc["colorBleConnected"];
    if (doc["colorBleDisconnected"].is<int>()) s.colorBleDisconnected = doc["colorBleDisconnected"];
    if (doc["colorBar1"].is<int>()) s.colorBar1 = doc["colorBar1"];
    if (doc["colorBar2"].is<int>()) s.colorBar2 = doc["colorBar2"];
    if (doc["colorBar3"].is<int>()) s.colorBar3 = doc["colorBar3"];
    if (doc["colorBar4"].is<int>()) s.colorBar4 = doc["colorBar4"];
    if (doc["colorBar5"].is<int>()) s.colorBar5 = doc["colorBar5"];
    if (doc["colorBar6"].is<int>()) s.colorBar6 = doc["colorBar6"];
    if (doc["colorMuted"].is<int>()) s.colorMuted = doc["colorMuted"];
    if (doc["colorPersisted"].is<int>()) s.colorPersisted = doc["colorPersisted"];
    if (doc["colorVolumeMain"].is<int>()) s.colorVolumeMain = doc["colorVolumeMain"];
    if (doc["colorVolumeMute"].is<int>()) s.colorVolumeMute = doc["colorVolumeMute"];
    if (doc["colorWiFiConnected"].is<int>()) s.colorWiFiConnected = doc["colorWiFiConnected"];
    if (doc["colorRssiV1"].is<int>()) s.colorRssiV1 = doc["colorRssiV1"];
    if (doc["colorRssiProxy"].is<int>()) s.colorRssiProxy = doc["colorRssiProxy"];
    if (doc["freqUseBandColor"].is<bool>()) s.freqUseBandColor = doc["freqUseBandColor"];
    
    // Display visibility
    if (doc["hideWifiIcon"].is<bool>()) s.hideWifiIcon = doc["hideWifiIcon"];
    if (doc["hideProfileIndicator"].is<bool>()) s.hideProfileIndicator = doc["hideProfileIndicator"];
    if (doc["hideBatteryIcon"].is<bool>()) s.hideBatteryIcon = doc["hideBatteryIcon"];
    if (doc["showBatteryPercent"].is<bool>()) s.showBatteryPercent = doc["showBatteryPercent"];
    if (doc["hideBleIcon"].is<bool>()) s.hideBleIcon = doc["hideBleIcon"];
    if (doc["hideVolumeIndicator"].is<bool>()) s.hideVolumeIndicator = doc["hideVolumeIndicator"];
    if (doc["hideRssiIndicator"].is<bool>()) s.hideRssiIndicator = doc["hideRssiIndicator"];
    if (doc["showRestTelemetryCards"].is<bool>()) s.showRestTelemetryCards = doc["showRestTelemetryCards"];
    
    // Development
    if (doc["enableWifiAtBoot"].is<bool>()) s.enableWifiAtBoot = doc["enableWifiAtBoot"];
    
    // WiFi client settings
    if (doc["wifiMode"].is<int>()) {
        int mode = doc["wifiMode"].as<int>();
        mode = std::max(static_cast<int>(V1_WIFI_OFF), std::min(mode, static_cast<int>(V1_WIFI_APSTA)));
        s.wifiMode = static_cast<WiFiModeSetting>(mode);
    }
    if (doc["wifiClientEnabled"].is<bool>()) s.wifiClientEnabled = doc["wifiClientEnabled"];
    if (doc["wifiClientSSID"].is<const char*>()) s.wifiClientSSID = sanitizeWifiClientSsidValue(doc["wifiClientSSID"].as<String>());
    
    // Auto power-off
    if (doc["autoPowerOffMinutes"].is<int>()) {
        s.autoPowerOffMinutes = clampU8Value(doc["autoPowerOffMinutes"].as<int>(), 0, 60);
    }
    if (doc["apTimeoutMinutes"].is<int>()) {
        s.apTimeoutMinutes = clampApTimeoutValue(doc["apTimeoutMinutes"].as<int>());
    }
    
    // Voice settings
    if (doc["voiceAlertMode"].is<int>()) {
        int mode = doc["voiceAlertMode"].as<int>();
        mode = std::max(static_cast<int>(VOICE_MODE_DISABLED), std::min(mode, static_cast<int>(VOICE_MODE_BAND_FREQ)));
        s.voiceAlertMode = static_cast<VoiceAlertMode>(mode);
    }
    if (doc["voiceDirectionEnabled"].is<bool>()) s.voiceDirectionEnabled = doc["voiceDirectionEnabled"];
    if (doc["announceBogeyCount"].is<bool>()) s.announceBogeyCount = doc["announceBogeyCount"];
    if (doc["muteVoiceIfVolZero"].is<bool>()) s.muteVoiceIfVolZero = doc["muteVoiceIfVolZero"];
    if (doc["voiceVolume"].is<int>()) s.voiceVolume = clampU8Value(doc["voiceVolume"].as<int>(), 0, 100);
    if (doc["alertVolumeFadeEnabled"].is<bool>()) s.alertVolumeFadeEnabled = doc["alertVolumeFadeEnabled"];
    if (doc["alertVolumeFadeDelaySec"].is<int>()) {
        s.alertVolumeFadeDelaySec = clampU8Value(doc["alertVolumeFadeDelaySec"].as<int>(), 1, 10);
    }
    if (doc["alertVolumeFadeVolume"].is<int>()) {
        s.alertVolumeFadeVolume = clampU8Value(doc["alertVolumeFadeVolume"].as<int>(), 0, 9);
    }
    if (doc["speedVolumeEnabled"].is<bool>()) s.speedVolumeEnabled = doc["speedVolumeEnabled"];
    if (doc["speedVolumeThresholdMph"].is<int>()) {
        s.speedVolumeThresholdMph = clampU8Value(doc["speedVolumeThresholdMph"].as<int>(), 10, 100);
    }
    if (doc["speedVolumeBoost"].is<int>()) {
        s.speedVolumeBoost = clampU8Value(doc["speedVolumeBoost"].as<int>(), 1, 5);
    }
    if (doc["lowSpeedMuteEnabled"].is<bool>()) s.lowSpeedMuteEnabled = doc["lowSpeedMuteEnabled"];
    if (doc["lowSpeedMuteThresholdMph"].is<int>()) {
        s.lowSpeedMuteThresholdMph = clampU8Value(doc["lowSpeedMuteThresholdMph"].as<int>(), 1, 30);
    }
    if (doc["announceSecondaryAlerts"].is<bool>()) s.announceSecondaryAlerts = doc["announceSecondaryAlerts"];
    if (doc["secondaryLaser"].is<bool>()) s.secondaryLaser = doc["secondaryLaser"];
    if (doc["secondaryKa"].is<bool>()) s.secondaryKa = doc["secondaryKa"];
    if (doc["secondaryK"].is<bool>()) s.secondaryK = doc["secondaryK"];
    if (doc["secondaryX"].is<bool>()) s.secondaryX = doc["secondaryX"];
    
    // Auto-push slot settings
    // Only allow backup to enable auto-push (avoid stale backups disabling it)
    if (doc["autoPushEnabled"].is<bool>() && doc["autoPushEnabled"].as<bool>()) {
        s.autoPushEnabled = true;
    }
    if (doc["activeSlot"].is<int>()) s.activeSlot = std::max(0, std::min(doc["activeSlot"].as<int>(), 2));
    if (doc["slot0Name"].is<const char*>()) s.slot0Name = sanitizeSlotNameValue(doc["slot0Name"].as<String>());
    if (doc["slot1Name"].is<const char*>()) s.slot1Name = sanitizeSlotNameValue(doc["slot1Name"].as<String>());
    if (doc["slot2Name"].is<const char*>()) s.slot2Name = sanitizeSlotNameValue(doc["slot2Name"].as<String>());
    if (doc["slot0Color"].is<int>()) s.slot0Color = doc["slot0Color"];
    if (doc["slot1Color"].is<int>()) s.slot1Color = doc["slot1Color"];
    if (doc["slot2Color"].is<int>()) s.slot2Color = doc["slot2Color"];
    if (doc["slot0Volume"].is<int>()) s.slot0Volume = clampSlotVolumeValue(doc["slot0Volume"].as<int>());
    if (doc["slot1Volume"].is<int>()) s.slot1Volume = clampSlotVolumeValue(doc["slot1Volume"].as<int>());
    if (doc["slot2Volume"].is<int>()) s.slot2Volume = clampSlotVolumeValue(doc["slot2Volume"].as<int>());
    if (doc["slot0MuteVolume"].is<int>()) s.slot0MuteVolume = clampSlotVolumeValue(doc["slot0MuteVolume"].as<int>());
    if (doc["slot1MuteVolume"].is<int>()) s.slot1MuteVolume = clampSlotVolumeValue(doc["slot1MuteVolume"].as<int>());
    if (doc["slot2MuteVolume"].is<int>()) s.slot2MuteVolume = clampSlotVolumeValue(doc["slot2MuteVolume"].as<int>());
    if (doc["slot0DarkMode"].is<bool>()) s.slot0DarkMode = doc["slot0DarkMode"];
    if (doc["slot1DarkMode"].is<bool>()) s.slot1DarkMode = doc["slot1DarkMode"];
    if (doc["slot2DarkMode"].is<bool>()) s.slot2DarkMode = doc["slot2DarkMode"];
    if (doc["slot0MuteToZero"].is<bool>()) s.slot0MuteToZero = doc["slot0MuteToZero"];
    if (doc["slot1MuteToZero"].is<bool>()) s.slot1MuteToZero = doc["slot1MuteToZero"];
    if (doc["slot2MuteToZero"].is<bool>()) s.slot2MuteToZero = doc["slot2MuteToZero"];
    if (doc["slot0AlertPersist"].is<int>()) s.slot0AlertPersist = clampU8Value(doc["slot0AlertPersist"].as<int>(), 0, 5);
    if (doc["slot1AlertPersist"].is<int>()) s.slot1AlertPersist = clampU8Value(doc["slot1AlertPersist"].as<int>(), 0, 5);
    if (doc["slot2AlertPersist"].is<int>()) s.slot2AlertPersist = clampU8Value(doc["slot2AlertPersist"].as<int>(), 0, 5);
    if (doc["slot0PriorityArrow"].is<bool>()) s.slot0PriorityArrow = doc["slot0PriorityArrow"];
    if (doc["slot1PriorityArrow"].is<bool>()) s.slot1PriorityArrow = doc["slot1PriorityArrow"];
    if (doc["slot2PriorityArrow"].is<bool>()) s.slot2PriorityArrow = doc["slot2PriorityArrow"];
    if (doc["slot0ProfileName"].is<const char*>()) s.slot0_default.profileName = sanitizeProfileNameValue(doc["slot0ProfileName"].as<String>());
    if (doc["slot0Mode"].is<int>()) s.slot0_default.mode = normalizeV1ModeValue(doc["slot0Mode"].as<int>());
    if (doc["slot1ProfileName"].is<const char*>()) s.slot1_highway.profileName = sanitizeProfileNameValue(doc["slot1ProfileName"].as<String>());
    if (doc["slot1Mode"].is<int>()) s.slot1_highway.mode = normalizeV1ModeValue(doc["slot1Mode"].as<int>());
    if (doc["slot2ProfileName"].is<const char*>()) s.slot2_comfort.profileName = sanitizeProfileNameValue(doc["slot2ProfileName"].as<String>());
    if (doc["slot2Mode"].is<int>()) s.slot2_comfort.mode = normalizeV1ModeValue(doc["slot2Mode"].as<int>());
    
    // Restore V1 profiles if present
    int profilesRestored = 0;
    if (doc["profiles"].is<JsonArray>()) {
        JsonArray profilesArr = doc["profiles"].as<JsonArray>();
        for (JsonObject p : profilesArr) {
            if (!p["name"].is<const char*>() || !p["bytes"].is<JsonArray>()) {
                continue;  // Skip invalid profile entries
            }
            
            V1Profile profile;
            profile.name = sanitizeProfileNameValue(p["name"].as<String>());
            if (profile.name.length() == 0) {
                continue;
            }
            if (p["description"].is<const char*>()) {
                profile.description = sanitizeProfileDescriptionValue(p["description"].as<String>());
            }
            if (p["displayOn"].is<bool>()) profile.displayOn = p["displayOn"];
            if (p["mainVolume"].is<int>()) profile.mainVolume = clampSlotVolumeValue(p["mainVolume"].as<int>());
            if (p["mutedVolume"].is<int>()) profile.mutedVolume = clampSlotVolumeValue(p["mutedVolume"].as<int>());
            
            JsonArray bytes = p["bytes"].as<JsonArray>();
            if (bytes.size() == 6) {
                for (int i = 0; i < 6; i++) {
                    profile.settings.bytes[i] = bytes[i].as<uint8_t>();
                }
                
                ProfileSaveResult result = v1ProfileManager.saveProfile(profile);
                if (result.success) {
                    profilesRestored++;
                    Serial.printf("[Settings] Restored profile: %s\n", profile.name.c_str());
                } else {
                    Serial.printf("[Settings] Failed to restore profile: %s - %s\n", 
                                  profile.name.c_str(), result.error.c_str());
                }
            }
        }
    }
    
    // Save to flash
    settingsManager.save();

    if (!settingsManager.get().obdEnabled) {
        obdHandler.stopScan();
        obdHandler.disconnect();
    }
    obdHandler.setVwDataEnabled(settingsManager.get().obdVwDataEnabled);
    gpsRuntimeModule.setEnabled(settingsManager.get().gpsEnabled);
    speedSourceSelector.setGpsEnabled(settingsManager.get().gpsEnabled);
    cameraRuntimeModule.setEnabled(computeCameraRuntimeEnabled(settingsManager.get()));
    lockoutSetKaLearningEnabled(settingsManager.get().gpsLockoutKaLearningEnabled);
    
    Serial.printf("[Settings] Restored from uploaded backup (%d profiles)\n", profilesRestored);
    
    // Build response with profile count
    String response = "{\"success\":true,\"message\":\"Settings restored successfully";
    if (profilesRestored > 0) {
        response += " (" + String(profilesRestored) + " profiles)";
    }
    response += "\"}";
    server.send(200, "application/json", response);
}

void handleApiRestore(WebServer& server,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleRestore(server);
}

}  // namespace BackupApiService

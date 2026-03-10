#include "backup_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>

#include "../../settings.h"
#include "../../settings_sanitize.h"
#include "../../storage_manager.h"
#include "../../v1_profiles.h"
#include "../../backup_payload_builder.h"
#include "../gps/gps_runtime_module.h"
#include "../gps/gps_lockout_safety.h"
#include "../lockout/lockout_band_policy.h"
#include "../speed/speed_source_selector.h"
#include "../../../include/clamp_utils.h"
#include "json_stream_response.h"

namespace {

uint8_t clampU8Value(int value, int minVal, int maxVal) {
    if (value < minVal) return static_cast<uint8_t>(minVal);
    if (value > maxVal) return static_cast<uint8_t>(maxVal);
    return static_cast<uint8_t>(value);
}

String sanitizeLastV1AddressForBackup(const String& raw) {
    static constexpr size_t kMaxV1AddressLen = 32;
    return clampStringLength(raw, kMaxV1AddressLen);
}

}  // anonymous namespace

namespace BackupApiService {

static void sendBackup(WebServer& server) {
    Serial.println("[HTTP] GET /api/settings/backup");
    JsonDocument doc;
    BackupPayloadBuilder::buildBackupDocument(
        doc,
        settingsManager.get(),
        v1ProfileManager,
        BackupPayloadBuilder::BackupTransport::HttpDownload,
        millis());
    
    // Send with Content-Disposition header for download
    server.sendHeader("Content-Disposition", "attachment; filename=\"v1simple_backup.json\"");
    sendJsonStream(server, doc);
}

static void handleBackupNow(WebServer& server) {
    Serial.println("[HTTP] POST /api/settings/backup-now");

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        server.send(503, "application/json",
                    "{\"success\":false,\"error\":\"SD card unavailable\"}");
        return;
    }

    settingsManager.backupToSD();
    server.send(200, "application/json",
                "{\"success\":true,\"message\":\"Backup written to SD\"}");
}

void handleApiBackup(WebServer& server,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    sendBackup(server);
}

void handleApiBackupNow(WebServer& server,
                        const std::function<bool()>& checkRateLimit,
                        const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleBackupNow(server);
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
    if (!doc["_type"].is<const char*>() ||
        !BackupPayloadBuilder::isRecognizedBackupType(doc["_type"].as<const char*>())) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid backup format\"}");
        return;
    }
    
    // Restore settings (same logic as restoreFromSD but from JSON body)
    V1Settings& s = settingsManager.mutableSettings();
    
    // BLE settings
    if (doc["enableWifi"].is<bool>()) s.enableWifi = doc["enableWifi"];
    if (doc["proxyBLE"].is<bool>()) s.proxyBLE = doc["proxyBLE"];
    if (doc["proxyName"].is<const char*>()) s.proxyName = sanitizeProxyNameValue(doc["proxyName"].as<String>());
    if (doc["gpsEnabled"].is<bool>()) s.gpsEnabled = doc["gpsEnabled"];
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
        s.gpsLockoutMaxQueueDrops =
            clamp_utils::clampU16Value(doc["gpsLockoutMaxQueueDrops"].as<int>(), 0, 65535);
    }
    if (doc["gpsLockoutMaxPerfDrops"].is<int>()) {
        s.gpsLockoutMaxPerfDrops =
            clamp_utils::clampU16Value(doc["gpsLockoutMaxPerfDrops"].as<int>(), 0, 65535);
    }
    if (doc["gpsLockoutMaxEventBusDrops"].is<int>()) {
        s.gpsLockoutMaxEventBusDrops =
            clamp_utils::clampU16Value(doc["gpsLockoutMaxEventBusDrops"].as<int>(), 0, 65535);
    }
    if (doc["gpsLockoutLearnerPromotionHits"].is<int>()) {
        s.gpsLockoutLearnerPromotionHits = clampLockoutLearnerHitsValue(
            doc["gpsLockoutLearnerPromotionHits"].as<int>());
    }
    if (doc["gpsLockoutLearnerRadiusE5"].is<int>()) {
        s.gpsLockoutLearnerRadiusE5 = clampLockoutLearnerRadiusE5Value(
            doc["gpsLockoutLearnerRadiusE5"].as<int>());
    }
    if (doc["gpsLockoutLearnerFreqToleranceMHz"].is<int>()) {
        s.gpsLockoutLearnerFreqToleranceMHz = clampLockoutLearnerFreqTolValue(
            doc["gpsLockoutLearnerFreqToleranceMHz"].as<int>());
    }
    if (doc["gpsLockoutLearnerLearnIntervalHours"].is<int>()) {
        s.gpsLockoutLearnerLearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
            doc["gpsLockoutLearnerLearnIntervalHours"].as<int>());
    }
    if (doc["gpsLockoutLearnerUnlearnIntervalHours"].is<int>()) {
        s.gpsLockoutLearnerUnlearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
            doc["gpsLockoutLearnerUnlearnIntervalHours"].as<int>());
    }
    if (doc["gpsLockoutLearnerUnlearnCount"].is<int>()) {
        s.gpsLockoutLearnerUnlearnCount = clampLockoutLearnerUnlearnCountValue(
            doc["gpsLockoutLearnerUnlearnCount"].as<int>());
    }
    if (doc["gpsLockoutManualDemotionMissCount"].is<int>()) {
        s.gpsLockoutManualDemotionMissCount = clampLockoutManualDemotionMissCountValue(
            doc["gpsLockoutManualDemotionMissCount"].as<int>());
    }
    if (doc["gpsLockoutKaLearningEnabled"].is<bool>()) {
        s.gpsLockoutKaLearningEnabled = doc["gpsLockoutKaLearningEnabled"];
    }
    if (doc["gpsLockoutKLearningEnabled"].is<bool>()) {
        s.gpsLockoutKLearningEnabled = doc["gpsLockoutKLearningEnabled"];
    }
    if (doc["gpsLockoutXLearningEnabled"].is<bool>()) {
        s.gpsLockoutXLearningEnabled = doc["gpsLockoutXLearningEnabled"];
    }
    if (doc["gpsLockoutPreQuiet"].is<bool>()) {
        s.gpsLockoutPreQuiet = doc["gpsLockoutPreQuiet"];
    }
    if (doc["gpsLockoutPreQuietBufferE5"].is<int>()) {
        s.gpsLockoutPreQuietBufferE5 = clampLockoutPreQuietBufferE5Value(
            doc["gpsLockoutPreQuietBufferE5"].as<int>());
    }
    if (doc["cameraAlertsEnabled"].is<bool>()) {
        s.cameraAlertsEnabled = doc["cameraAlertsEnabled"];
    }
    if (doc["cameraAlertRangeCm"].is<int>()) {
        s.cameraAlertRangeCm = clampCameraAlertRangeCmValue(doc["cameraAlertRangeCm"].as<int>());
    }
    if (doc["cameraAlertNearRangeCm"].is<int>()) {
        s.cameraAlertNearRangeCm = clampCameraAlertNearRangeCmValue(
            doc["cameraAlertNearRangeCm"].as<int>());
    }
    normalizeCameraAlertRanges(s.cameraAlertRangeCm, s.cameraAlertNearRangeCm);
    if (doc["cameraTypeAlpr"].is<bool>()) {
        s.cameraTypeAlpr = doc["cameraTypeAlpr"];
    }
    if (doc["cameraTypeRedLight"].is<bool>()) {
        s.cameraTypeRedLight = doc["cameraTypeRedLight"];
    }
    if (doc["cameraTypeSpeed"].is<bool>()) {
        s.cameraTypeSpeed = doc["cameraTypeSpeed"];
    }
    if (doc["cameraTypeBusLane"].is<bool>()) {
        s.cameraTypeBusLane = doc["cameraTypeBusLane"];
    }
    if (doc["colorCameraArrow"].is<int>()) {
        s.colorCameraArrow = doc["colorCameraArrow"];
    }
    if (doc["colorCameraText"].is<int>()) {
        s.colorCameraText = doc["colorCameraText"];
    }
    if (doc["cameraVoiceFarEnabled"].is<bool>()) {
        s.cameraVoiceFarEnabled = doc["cameraVoiceFarEnabled"];
    }
    if (doc["cameraVoiceNearEnabled"].is<bool>()) {
        s.cameraVoiceNearEnabled = doc["cameraVoiceNearEnabled"];
    }
    if (doc["lastV1Address"].is<const char*>()) {
        s.lastV1Address = sanitizeLastV1AddressForBackup(doc["lastV1Address"].as<String>());
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
    if (doc["colorLockout"].is<int>()) s.colorLockout = doc["colorLockout"];
    if (doc["colorGps"].is<int>()) s.colorGps = doc["colorGps"];
    if (doc["freqUseBandColor"].is<bool>()) s.freqUseBandColor = doc["freqUseBandColor"];
    
    // Display visibility
    if (doc["hideWifiIcon"].is<bool>()) s.hideWifiIcon = doc["hideWifiIcon"];
    if (doc["hideProfileIndicator"].is<bool>()) s.hideProfileIndicator = doc["hideProfileIndicator"];
    if (doc["hideBatteryIcon"].is<bool>()) s.hideBatteryIcon = doc["hideBatteryIcon"];
    if (doc["showBatteryPercent"].is<bool>()) s.showBatteryPercent = doc["showBatteryPercent"];
    if (doc["hideBleIcon"].is<bool>()) s.hideBleIcon = doc["hideBleIcon"];
    if (doc["hideVolumeIndicator"].is<bool>()) s.hideVolumeIndicator = doc["hideVolumeIndicator"];
    if (doc["hideRssiIndicator"].is<bool>()) s.hideRssiIndicator = doc["hideRssiIndicator"];
    
    // Development
    if (doc["enableWifiAtBoot"].is<bool>()) s.enableWifiAtBoot = doc["enableWifiAtBoot"];
    if (doc["enableSignalTraceLogging"].is<bool>()) {
        s.enableSignalTraceLogging = doc["enableSignalTraceLogging"];
    }
    
    // WiFi client settings
    if (doc["wifiMode"].is<int>()) {
        int mode = doc["wifiMode"].as<int>();
        mode = std::max(static_cast<int>(V1_WIFI_OFF), std::min(mode, static_cast<int>(V1_WIFI_APSTA)));
        s.wifiMode = static_cast<WiFiModeSetting>(mode);
    }
    if (doc["wifiClientEnabled"].is<bool>()) s.wifiClientEnabled = doc["wifiClientEnabled"];
    if (doc["wifiClientSSID"].is<const char*>()) s.wifiClientSSID = sanitizeWifiClientSsidValue(doc["wifiClientSSID"].as<String>());
    // Self-healing: derive wifiClientEnabled from SSID presence
    if (!s.wifiClientEnabled && s.wifiClientSSID.length() > 0) {
        s.wifiClientEnabled = true;
    }
    s.wifiMode = s.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;
    
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

    gpsRuntimeModule.setEnabled(settingsManager.get().gpsEnabled);
    speedSourceSelector.setGpsEnabled(settingsManager.get().gpsEnabled);
    lockoutSetKaLearningEnabled(settingsManager.get().gpsLockoutKaLearningEnabled);
    lockoutSetKLearningEnabled(settingsManager.get().gpsLockoutKLearningEnabled);
    lockoutSetXLearningEnabled(settingsManager.get().gpsLockoutXLearningEnabled);
    
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

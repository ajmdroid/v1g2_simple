/**
 * Settings SD backup writer and backup-file utilities.
 */

#include "settings_internals.h"

// --- Backup file static helpers ---

bool isSupportedBackupType(const JsonDocument& doc) {
    if (!doc["_type"].is<const char*>()) {
        return true;  // Legacy backups may not include a type marker.
    }
    const String type = doc["_type"].as<String>();
    return type == "v1simple_sd_backup" || type == "v1simple_backup";
}

bool hasBackupSignature(const JsonDocument& doc) {
    // Require a small signature set to avoid accepting arbitrary JSON blobs.
    return doc["apSSID"].is<const char*>() ||
           doc["brightness"].is<int>() ||
           doc["colorBogey"].is<int>() ||
           doc["slot0Name"].is<const char*>();
}

bool parseBackupFile(fs::FS* fs,
                            const char* path,
                            JsonDocument& doc,
                            bool verboseErrors) {
    if (!fs || !path || path[0] == '\0') {
        return false;
    }

    File file = fs->open(path, FILE_READ);
    if (!file) {
        if (verboseErrors) {
            Serial.printf("[Settings] Failed to open backup file: %s\n", path);
        }
        return false;
    }

    const size_t size = file.size();
    if (size == 0 || size > SETTINGS_BACKUP_MAX_BYTES) {
        if (verboseErrors) {
            Serial.printf("[Settings] Backup file size invalid (%u bytes): %s\n",
                          static_cast<unsigned int>(size),
                          path);
        }
        file.close();
        return false;
    }

    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        if (verboseErrors) {
            Serial.printf("[Settings] Failed to parse backup '%s': %s\n", path, err.c_str());
        }
        return false;
    }

    if (!isSupportedBackupType(doc)) {
        if (verboseErrors) {
            Serial.printf("[Settings] Unsupported backup type in %s\n", path);
        }
        return false;
    }
    if (!hasBackupSignature(doc)) {
        if (verboseErrors) {
            Serial.printf("[Settings] Backup signature check failed for %s\n", path);
        }
        return false;
    }

    return true;
}

int backupDocumentVersion(const JsonDocument& doc) {
    return doc["_version"] | doc["version"] | 1;
}

int backupCriticalFieldScore(const JsonDocument& doc) {
    int score = 0;
    if (!doc["gpsEnabled"].isNull()) score++;
    if (!doc["cameraAlertsEnabled"].isNull()) score++;
    if (!doc["cameraAlertRangeCm"].isNull()) score++;
    if (!doc["cameraAlertNearRangeCm"].isNull()) score++;
    if (!doc["brightness"].isNull()) score++;
    if (!doc["displayStyle"].isNull()) score++;
    if (!doc["proxyBLE"].isNull()) score++;
    if (!doc["proxyName"].isNull()) score++;
    if (!doc["wifiClientEnabled"].isNull()) score++;
    if (!doc["colorBogey"].isNull()) score++;
    if (!doc["slot0ProfileName"].isNull()) score++;
    if (!doc["slot1ProfileName"].isNull()) score++;
    if (!doc["slot2ProfileName"].isNull()) score++;
    return score;
}

int backupCandidateScore(const JsonDocument& doc) {
    // Prefer newer schema, then richer field coverage.
    return backupDocumentVersion(doc) * 100 + backupCriticalFieldScore(doc);
}


bool writeBackupAtomically(fs::FS* fs, const JsonDocument& doc) {
    if (!fs) {
        return false;
    }

    if (fs->exists(SETTINGS_BACKUP_TMP_PATH)) {
        fs->remove(SETTINGS_BACKUP_TMP_PATH);
    }

    File tmp = fs->open(SETTINGS_BACKUP_TMP_PATH, FILE_WRITE);
    if (!tmp) {
        Serial.println("[Settings] Failed to create temp SD backup file");
        return false;
    }

    const size_t written = serializeJson(doc, tmp);
    tmp.flush();
    tmp.close();

    if (written == 0) {
        Serial.println("[Settings] Failed to write temp SD backup");
        fs->remove(SETTINGS_BACKUP_TMP_PATH);
        return false;
    }

    // Parse-check temp backup before promotion.
    JsonDocument verifyTmp;
    if (!parseBackupFile(fs, SETTINGS_BACKUP_TMP_PATH, verifyTmp, true)) {
        Serial.println("[Settings] Temp SD backup failed validation");
        fs->remove(SETTINGS_BACKUP_TMP_PATH);
        return false;
    }

    if (fs->exists(SETTINGS_BACKUP_PREV_PATH)) {
        fs->remove(SETTINGS_BACKUP_PREV_PATH);
    }

    bool rotatedPrimary = false;
    if (fs->exists(SETTINGS_BACKUP_PATH)) {
        if (fs->rename(SETTINGS_BACKUP_PATH, SETTINGS_BACKUP_PREV_PATH)) {
            rotatedPrimary = true;
        } else {
            Serial.println("[Settings] ERROR: Failed to rotate primary backup; keeping existing file");
            fs->remove(SETTINGS_BACKUP_TMP_PATH);
            return false;
        }
    }

    if (!fs->rename(SETTINGS_BACKUP_TMP_PATH, SETTINGS_BACKUP_PATH)) {
        Serial.println("[Settings] ERROR: Failed to promote temp backup to primary");
        fs->remove(SETTINGS_BACKUP_TMP_PATH);

        if (rotatedPrimary && fs->exists(SETTINGS_BACKUP_PREV_PATH) && !fs->exists(SETTINGS_BACKUP_PATH)) {
            if (!fs->rename(SETTINGS_BACKUP_PREV_PATH, SETTINGS_BACKUP_PATH)) {
                Serial.println("[Settings] CRITICAL: Failed to rollback previous backup");
            }
        }
        return false;
    }

    // Final parse-check on promoted backup.
    JsonDocument verifyPrimary;
    if (!parseBackupFile(fs, SETTINGS_BACKUP_PATH, verifyPrimary, true)) {
        Serial.println("[Settings] ERROR: Promoted backup failed validation");
        fs->remove(SETTINGS_BACKUP_PATH);
        if (fs->exists(SETTINGS_BACKUP_PREV_PATH) && !fs->exists(SETTINGS_BACKUP_PATH)) {
            if (!fs->rename(SETTINGS_BACKUP_PREV_PATH, SETTINGS_BACKUP_PATH)) {
                Serial.println("[Settings] CRITICAL: Failed to restore previous backup after validation failure");
            }
        }
        return false;
    }

    return true;
}

// --- Member methods: SD backup write path ---


// Backup display/color settings to SD card

void SettingsManager::backupToSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return;  // SD not available, skip silently
    }
    
    // Acquire SD mutex to protect file I/O.
    // checkDmaHeap=false: backupToSD() is called from save() which runs inside
    // WiFi handlers — WiFi's SRAM buffers reduce DMA heap below the guard
    // thresholds, causing every web-UI save to silently skip the SD backup.
    // The write is small (one JSON file) and infrequent, so bypassing is safe.
    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex(), /*checkDmaHeap=*/false);
    if (!sdLock) {
        Serial.println("[Settings] Failed to acquire SD mutex for backup");
        return;
    }
    
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return;
    
    JsonDocument doc;
    doc["_type"] = "v1simple_sd_backup";
    doc["_version"] = SD_BACKUP_VERSION;
    doc["timestamp"] = millis();
    
    // === WiFi/Network Settings ===
    // Note: AP password intentionally NOT stored on SD card for security
    // (SD cards can be removed and read elsewhere)
    doc["enableWifi"] = settings.enableWifi;
    doc["wifiMode"] = (int)settings.wifiMode;
    doc["apSSID"] = settings.apSSID;
    doc["wifiClientEnabled"] = settings.wifiClientEnabled;
    doc["wifiClientSSID"] = settings.wifiClientSSID;
    doc["proxyBLE"] = settings.proxyBLE;
    doc["proxyName"] = settings.proxyName;
    doc["gpsEnabled"] = settings.gpsEnabled;
    doc["gpsLockoutMode"] = static_cast<int>(settings.gpsLockoutMode);
    doc["gpsLockoutCoreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    doc["gpsLockoutMaxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    doc["gpsLockoutMaxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    doc["gpsLockoutMaxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    doc["gpsLockoutLearnerPromotionHits"] = settings.gpsLockoutLearnerPromotionHits;
    doc["gpsLockoutLearnerRadiusE5"] = settings.gpsLockoutLearnerRadiusE5;
    doc["gpsLockoutLearnerFreqToleranceMHz"] = settings.gpsLockoutLearnerFreqToleranceMHz;
    doc["gpsLockoutLearnerLearnIntervalHours"] = settings.gpsLockoutLearnerLearnIntervalHours;
    doc["gpsLockoutLearnerUnlearnIntervalHours"] = settings.gpsLockoutLearnerUnlearnIntervalHours;
    doc["gpsLockoutLearnerUnlearnCount"] = settings.gpsLockoutLearnerUnlearnCount;
    doc["gpsLockoutManualDemotionMissCount"] = settings.gpsLockoutManualDemotionMissCount;
    doc["gpsLockoutKaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    doc["gpsLockoutPreQuiet"] = settings.gpsLockoutPreQuiet;
    doc["gpsLockoutPreQuietBufferE5"] = settings.gpsLockoutPreQuietBufferE5;
    doc["cameraAlertsEnabled"] = settings.cameraAlertsEnabled;
    doc["cameraAlertRangeCm"] = settings.cameraAlertRangeCm;
    doc["cameraAlertNearRangeCm"] = settings.cameraAlertNearRangeCm;
    doc["cameraTypeAlpr"] = settings.cameraTypeAlpr;
    doc["cameraTypeRedLight"] = settings.cameraTypeRedLight;
    doc["cameraTypeSpeed"] = settings.cameraTypeSpeed;
    doc["cameraTypeBusLane"] = settings.cameraTypeBusLane;
    doc["colorCameraArrow"] = settings.colorCameraArrow;
    doc["colorCameraText"] = settings.colorCameraText;
    doc["cameraVoiceFarEnabled"] = settings.cameraVoiceFarEnabled;
    doc["cameraVoiceNearEnabled"] = settings.cameraVoiceNearEnabled;
    doc["lastV1Address"] = settings.lastV1Address;
    doc["autoPowerOffMinutes"] = settings.autoPowerOffMinutes;
    doc["apTimeoutMinutes"] = settings.apTimeoutMinutes;
    
    
    // === Display Settings ===
    doc["brightness"] = settings.brightness;
    doc["turnOffDisplay"] = settings.turnOffDisplay;
    doc["displayStyle"] = static_cast<int>(settings.displayStyle);
    
    // === All Colors (RGB565) ===
    doc["colorBogey"] = settings.colorBogey;
    doc["colorFrequency"] = settings.colorFrequency;
    doc["colorArrowFront"] = settings.colorArrowFront;
    doc["colorArrowSide"] = settings.colorArrowSide;
    doc["colorArrowRear"] = settings.colorArrowRear;
    doc["colorBandL"] = settings.colorBandL;
    doc["colorBandKa"] = settings.colorBandKa;
    doc["colorBandK"] = settings.colorBandK;
    doc["colorBandX"] = settings.colorBandX;
    doc["colorBandPhoto"] = settings.colorBandPhoto;
    doc["colorWiFiIcon"] = settings.colorWiFiIcon;
    doc["colorWiFiConnected"] = settings.colorWiFiConnected;
    doc["colorBleConnected"] = settings.colorBleConnected;
    doc["colorBleDisconnected"] = settings.colorBleDisconnected;
    doc["colorBar1"] = settings.colorBar1;
    doc["colorBar2"] = settings.colorBar2;
    doc["colorBar3"] = settings.colorBar3;
    doc["colorBar4"] = settings.colorBar4;
    doc["colorBar5"] = settings.colorBar5;
    doc["colorBar6"] = settings.colorBar6;
    doc["colorMuted"] = settings.colorMuted;
    doc["colorPersisted"] = settings.colorPersisted;
    doc["colorVolumeMain"] = settings.colorVolumeMain;
    doc["colorVolumeMute"] = settings.colorVolumeMute;
    doc["colorRssiV1"] = settings.colorRssiV1;
    doc["colorRssiProxy"] = settings.colorRssiProxy;
    doc["colorLockout"] = settings.colorLockout;
    doc["colorGps"] = settings.colorGps;
    doc["freqUseBandColor"] = settings.freqUseBandColor;
    
    // === UI Toggle Settings ===
    doc["hideWifiIcon"] = settings.hideWifiIcon;
    doc["hideProfileIndicator"] = settings.hideProfileIndicator;
    doc["hideBatteryIcon"] = settings.hideBatteryIcon;
    doc["showBatteryPercent"] = settings.showBatteryPercent;
    doc["hideBleIcon"] = settings.hideBleIcon;
    doc["hideVolumeIndicator"] = settings.hideVolumeIndicator;
    doc["hideRssiIndicator"] = settings.hideRssiIndicator;
    doc["enableWifiAtBoot"] = settings.enableWifiAtBoot;
    doc["enableSignalTraceLogging"] = settings.enableSignalTraceLogging;
    
    // === Voice Alert Settings ===
    doc["voiceAlertMode"] = (int)settings.voiceAlertMode;
    doc["voiceDirectionEnabled"] = settings.voiceDirectionEnabled;
    doc["announceBogeyCount"] = settings.announceBogeyCount;
    doc["muteVoiceIfVolZero"] = settings.muteVoiceIfVolZero;
    doc["voiceVolume"] = settings.voiceVolume;
    doc["announceSecondaryAlerts"] = settings.announceSecondaryAlerts;
    doc["secondaryLaser"] = settings.secondaryLaser;
    doc["secondaryKa"] = settings.secondaryKa;
    doc["secondaryK"] = settings.secondaryK;
    doc["secondaryX"] = settings.secondaryX;
    doc["alertVolumeFadeEnabled"] = settings.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = settings.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = settings.alertVolumeFadeVolume;
    
    // === Auto-Push Settings ===
    doc["autoPushEnabled"] = settings.autoPushEnabled;
    doc["activeSlot"] = settings.activeSlot;
    
    // === Slot 0 Settings ===
    doc["slot0Name"] = settings.slot0Name;
    doc["slot0Color"] = settings.slot0Color;
    doc["slot0Volume"] = settings.slot0Volume;
    doc["slot0MuteVolume"] = settings.slot0MuteVolume;
    doc["slot0DarkMode"] = settings.slot0DarkMode;
    doc["slot0MuteToZero"] = settings.slot0MuteToZero;
    doc["slot0AlertPersist"] = settings.slot0AlertPersist;
    doc["slot0PriorityArrow"] = settings.slot0PriorityArrow;
    doc["slot0ProfileName"] = settings.slot0_default.profileName;
    doc["slot0Mode"] = settings.slot0_default.mode;
    
    // === Slot 1 Settings ===
    doc["slot1Name"] = settings.slot1Name;
    doc["slot1Color"] = settings.slot1Color;
    doc["slot1Volume"] = settings.slot1Volume;
    doc["slot1MuteVolume"] = settings.slot1MuteVolume;
    doc["slot1DarkMode"] = settings.slot1DarkMode;
    doc["slot1MuteToZero"] = settings.slot1MuteToZero;
    doc["slot1AlertPersist"] = settings.slot1AlertPersist;
    doc["slot1PriorityArrow"] = settings.slot1PriorityArrow;
    doc["slot1ProfileName"] = settings.slot1_highway.profileName;
    doc["slot1Mode"] = settings.slot1_highway.mode;
    
    // === Slot 2 Settings ===
    doc["slot2Name"] = settings.slot2Name;
    doc["slot2Color"] = settings.slot2Color;
    doc["slot2Volume"] = settings.slot2Volume;
    doc["slot2MuteVolume"] = settings.slot2MuteVolume;
    doc["slot2DarkMode"] = settings.slot2DarkMode;
    doc["slot2MuteToZero"] = settings.slot2MuteToZero;
    doc["slot2AlertPersist"] = settings.slot2AlertPersist;
    doc["slot2PriorityArrow"] = settings.slot2PriorityArrow;
    doc["slot2ProfileName"] = settings.slot2_comfort.profileName;
    doc["slot2Mode"] = settings.slot2_comfort.mode;

    // Include full V1 profile payloads so restore can recover from filesystem loss
    JsonArray profilesArr = doc["profiles"].to<JsonArray>();
    int profilesBackedUp = 0;
    if (v1ProfileManager.isReady()) {
        std::vector<String> profileNames = v1ProfileManager.listProfiles();
        for (const String& name : profileNames) {
            V1Profile profile;
            if (!v1ProfileManager.loadProfile(name, profile)) {
                continue;
            }

            JsonObject p = profilesArr.add<JsonObject>();
            p["name"] = profile.name;
            p["description"] = profile.description;
            p["displayOn"] = profile.displayOn;
            p["mainVolume"] = profile.mainVolume;
            p["mutedVolume"] = profile.mutedVolume;

            JsonArray bytes = p["bytes"].to<JsonArray>();
            for (int i = 0; i < 6; i++) {
                bytes.add(profile.settings.bytes[i]);
            }
            profilesBackedUp++;
        }
    }
    
    if (!writeBackupAtomically(fs, doc)) {
        Serial.println("[Settings] ERROR: Failed to commit SD backup atomically");
        return;
    }
    
    Serial.printf("[Settings] Full backup saved to SD card (%d profiles)\n", profilesBackedUp);
    Serial.printf("[Settings] Backed up: slot0Mode=%d, slot1Mode=%d, slot2Mode=%d\n",
                  settings.slot0_default.mode, settings.slot1_highway.mode, settings.slot2_comfort.mode);
}

/**
 * Settings SD backup/restore and NVS-restore detection.
 * Extracted from settings.cpp to reduce file size.
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
    if (!doc["cameraEnabled"].isNull()) score++;
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

bool loadBestBackupDocument(fs::FS* fs,
                                   JsonDocument& outDoc,
                                   const char** outPath,
                                   bool verboseErrors) {
    if (!fs) {
        return false;
    }

    int bestScore = -1;
    const char* bestPath = nullptr;
    String bestJson;
    JsonDocument candidateDoc;

    for (size_t i = 0; i < SETTINGS_BACKUP_CANDIDATES_COUNT; ++i) {
        const char* candidate = SETTINGS_BACKUP_CANDIDATES[i];
        if (!fs->exists(candidate)) {
            continue;
        }

        candidateDoc.clear();
        if (!parseBackupFile(fs, candidate, candidateDoc, verboseErrors)) {
            if (verboseErrors) {
                Serial.printf("[Settings] WARN: Ignoring invalid backup candidate: %s\n", candidate);
            }
            continue;
        }

        const int score = backupCandidateScore(candidateDoc);
        if (score > bestScore) {
            bestScore = score;
            bestPath = candidate;
            bestJson = "";
            serializeJson(candidateDoc, bestJson);
        }
    }

    if (bestScore < 0 || bestJson.length() == 0 || !bestPath) {
        return false;
    }

    outDoc.clear();
    DeserializationError err = deserializeJson(outDoc, bestJson);
    if (err) {
        if (verboseErrors) {
            Serial.printf("[Settings] Failed to parse selected backup '%s': %s\n",
                          bestPath,
                          err.c_str());
        }
        return false;
    }

    if (outPath) {
        *outPath = bestPath;
    }
    return true;
}

bool parseBoolVariant(const JsonVariantConst& value, bool& out) {
    if (value.isNull()) {
        return false;
    }
    if (value.is<bool>()) {
        out = value.as<bool>();
        return true;
    }
    if (value.is<int>()) {
        out = value.as<int>() != 0;
        return true;
    }
    if (value.is<const char*>()) {
        String raw = value.as<String>();
        raw.trim();
        raw.toLowerCase();
        if (raw == "1" || raw == "true" || raw == "on" || raw == "yes") {
            out = true;
            return true;
        }
        if (raw == "0" || raw == "false" || raw == "off" || raw == "no") {
            out = false;
            return true;
        }
    }
    return false;
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

// --- Member methods: SD backup/restore ---

bool SettingsManager::checkAndRestoreFromSD() {
    // Check if NVS was erased (appears default) and backup exists on SD
    // This can be called after storage is mounted to retry the restore
    bool needsRestore = checkNeedsRestore();
    fs::FS* fs = nullptr;
    bool hasSdBackup = false;
    JsonDocument bestBackupDoc;
    const char* bestBackupPath = nullptr;
    if (storageManager.isReady() && storageManager.isSDCard()) {
        fs = storageManager.getFilesystem();
        hasSdBackup = loadBestBackupDocument(fs, bestBackupDoc, &bestBackupPath, false);
    }

    if (needsRestore) {
        Serial.println("[Settings] Checking for SD backup restore...");
        if (restoreFromSD()) {
            Serial.println("[Settings] Restored settings from SD backup!");
            // Immediately re-emit backup in current schema after a successful restore.
            backupToSD();
            return true;
        }
        Serial.println("[Settings] Restore requested but no valid SD backup was applied");
    } else if (hasSdBackup) {
        // Keep user/NVS state authoritative unless corruption is detected.
        // Slot/profile healing is handled separately by validateProfileReferences().
        Serial.println("[Settings] NVS healthy; skipping automatic SD settings restore");

        // Targeted WiFi credential recovery: if wifiClientEnabled=true but SSID
        // is missing from NVS (e.g. partial NVS wipe, firmware flash that erased
        // some keys), recover the SSID from the SD backup without overwriting
        // the rest of the NVS settings.
        if (settings.wifiClientEnabled && settings.wifiClientSSID.length() == 0) {
            const char* backupSsid = bestBackupDoc["wifiClientSSID"] | "";
            if (strlen(backupSsid) > 0) {
                settings.wifiClientSSID = sanitizeWifiClientSsidValue(String(backupSsid));
                settings.wifiMode = V1_WIFI_APSTA;
                Serial.printf("[Settings] HEAL: recovered wifiClientSSID='%s' from SD backup\n",
                              settings.wifiClientSSID.c_str());
                save();
            } else {
                // SSID also missing from backup — disable to avoid an inconsistent state
                settings.wifiClientEnabled = false;
                settings.wifiMode = V1_WIFI_AP;
                Serial.println("[Settings] HEAL: wifiClientEnabled=true but no SSID anywhere — disabling");
                save();
            }
        }
    }

    // Keep SD backup schema fresh so newly added settings survive the next reflash.
    if (!needsRestore && storageManager.isReady() && storageManager.isSDCard()) {
        if (!hasSdBackup) {
            Serial.println("[Settings] No valid SD backup found; creating backup from current settings");
            backupToSD();
        } else {
            const int backupVersion = backupDocumentVersion(bestBackupDoc);
            const bool missingCoreFields =
                bestBackupDoc["gpsEnabled"].isNull() ||
                bestBackupDoc["cameraEnabled"].isNull() ||
                bestBackupDoc["displayStyle"].isNull() ||
                bestBackupDoc["brightness"].isNull();
            if (backupVersion < SD_BACKUP_VERSION || missingCoreFields) {
                Serial.printf("[Settings] Refreshing SD backup schema (path=%s version=%d)\n",
                              bestBackupPath ? bestBackupPath : "(unknown)",
                              backupVersion);
                backupToSD();
            }
        }
    }
    return false;
}

bool SettingsManager::checkNeedsRestore() {
    // Check if NVS was likely wiped by looking for the settings version marker
    // If settingsVer is missing (defaults to 1, triggers migration message),
    // that's a strong indicator NVS was erased during a partition table change
    // 
    // We use a dedicated "nvsValid" marker that's only set after a successful save
    // If this marker is missing but an SD backup exists, we should restore
    
    String activeNs = getActiveNamespace();
    Preferences checkPrefs;
    if (!checkPrefs.begin(activeNs.c_str(), true)) {
        // Can't even open the namespace - definitely needs restore
        return true;
    }
    
    // Check for our validity marker - set to current version after successful save
    int nvsMarker = checkPrefs.getInt("nvsValid", 0);
    int settingsVer = checkPrefs.getInt("settingsVer", 0);
    bool missingCriticalKey = false;
    // These keys exist in all modern schemas and should never disappear in a healthy namespace.
    static constexpr const char* kCriticalKeys[] = {
        "gpsEn",
        "camEn",
        "proxyBLE",
        "proxyName",
        "brightness",
        "dispStyle",
        "autoPush",
        "gpsLkMode"
    };
    for (const char* key : kCriticalKeys) {
        if (!checkPrefs.isKey(key)) {
            missingCriticalKey = true;
            Serial.printf("[Settings] Missing critical key '%s' in active namespace\n", key);
        }
    }
    checkPrefs.end();
    
    // If neither marker exists, NVS was likely wiped
    if (nvsMarker == 0 && settingsVer == 0) {
        Serial.println("[Settings] NVS appears empty (no version markers)");
        return true;
    }
    
    // Also check if brightness is still at exact default - common indicator of wipe
    // combined with missing settingsVer (which would be >= 2 if properly saved)
    if (settingsVer <= 1 && settings.brightness == 200) {
        Serial.println("[Settings] NVS appears default (v1 migration + default brightness)");
        return true;
    }

    if (missingCriticalKey && (settingsVer >= 2 || nvsMarker == 0)) {
        Serial.println("[Settings] NVS appears partial/corrupt (critical keys missing)");
        return true;
    }

    // Detect incomplete writes: settingsVer is the FIRST key written and
    // nvsValid is the LAST.  If settingsVer exists but nvsValid does not,
    // the namespace was only partially written (crash/reset mid-save).
    if (nvsMarker == 0 && settingsVer >= 2) {
        Serial.println("[Settings] NVS appears incomplete (settingsVer present but nvsValid missing)");
        return true;
    }
    
    return false;
}

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
    doc["obdEnabled"] = settings.obdEnabled;
    doc["obdVwDataEnabled"] = settings.obdVwDataEnabled;
    doc["gpsEnabled"] = settings.gpsEnabled;
    doc["cameraEnabled"] = settings.cameraEnabled;
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
    doc["colorCameraToken"] = settings.colorCameraToken;
    doc["colorCameraArrow"] = settings.colorCameraArrow;
    doc["colorLockout"] = settings.colorLockout;
    doc["colorGps"] = settings.colorGps;
    doc["colorObd"] = settings.colorObd;
    doc["freqUseBandColor"] = settings.freqUseBandColor;
    
    // === UI Toggle Settings ===
    doc["hideWifiIcon"] = settings.hideWifiIcon;
    doc["hideProfileIndicator"] = settings.hideProfileIndicator;
    doc["hideBatteryIcon"] = settings.hideBatteryIcon;
    doc["showBatteryPercent"] = settings.showBatteryPercent;
    doc["hideBleIcon"] = settings.hideBleIcon;
    doc["hideVolumeIndicator"] = settings.hideVolumeIndicator;
    doc["hideRssiIndicator"] = settings.hideRssiIndicator;
    doc["showRestTelemetryCards"] = settings.showRestTelemetryCards;
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
    doc["speedVolumeEnabled"] = settings.speedVolumeEnabled;
    doc["speedVolumeThresholdMph"] = settings.speedVolumeThresholdMph;
    doc["speedVolumeBoost"] = settings.speedVolumeBoost;
    doc["lowSpeedMuteEnabled"] = settings.lowSpeedMuteEnabled;
    doc["lowSpeedMuteThresholdMph"] = settings.lowSpeedMuteThresholdMph;
    doc["lowSpeedVolume"] = settings.lowSpeedVolume;
    
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

// Restore ALL settings from SD card

bool SettingsManager::restoreFromSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }
    
    // Acquire SD mutex to protect file I/O
    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        Serial.println("[Settings] Failed to acquire SD mutex for restore");
        return false;
    }
    
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return false;
    
    const char* backupPath = nullptr;
    JsonDocument doc;
    if (!loadBestBackupDocument(fs, doc, &backupPath, true)) {
        backupPath = nullptr;
    }

    if (!backupPath) {
        Serial.println("[Settings] No valid SD backup found");
        return false;
    }

    Serial.printf("[Settings] Using backup file: %s\n", backupPath);
    
    int backupVersion = doc["_version"] | doc["version"] | 1;
    Serial.printf("[Settings] Restoring from SD backup (version %d)\n", backupVersion);
    bool backupAutoPush = false;
    const bool hasAutoPush = parseBoolVariant(doc["autoPushEnabled"], backupAutoPush);
    const char* backupSlot0 = doc["slot0ProfileName"].is<const char*>()
        ? doc["slot0ProfileName"].as<const char*>() : "";
    const int backupSlot0Mode = doc["slot0Mode"].is<int>() ? doc["slot0Mode"].as<int>() : -1;
    Serial.printf("[Settings] Backup fields: autoPush=%s slot0Profile='%s' slot0Mode=%d\n",
                  hasAutoPush ? (backupAutoPush ? "true" : "false") : "missing",
                  backupSlot0,
                  backupSlot0Mode);

    auto restoreBool = [&](const char* key, bool& target) {
        bool parsed = false;
        if (parseBoolVariant(doc[key], parsed)) {
            target = parsed;
        }
    };
    
    // === WiFi/Network Settings ===
    // Note: AP password NOT restored from SD for security - user must re-enter after restore
    restoreBool("enableWifi", settings.enableWifi);
    if (doc["wifiMode"].is<int>()) settings.wifiMode = clampWifiModeValue(doc["wifiMode"].as<int>());
    if (doc["apSSID"].is<const char*>()) settings.apSSID = sanitizeApSsidValue(doc["apSSID"].as<String>());
    restoreBool("wifiClientEnabled", settings.wifiClientEnabled);
    if (doc["wifiClientSSID"].is<const char*>()) settings.wifiClientSSID = sanitizeWifiClientSsidValue(doc["wifiClientSSID"].as<String>());
    // Self-healing: derive wifiClientEnabled from SSID presence
    if (!settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0) {
        settings.wifiClientEnabled = true;
    }
    settings.wifiMode = settings.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;
    restoreBool("proxyBLE", settings.proxyBLE);
    if (doc["proxyName"].is<const char*>()) settings.proxyName = sanitizeProxyNameValue(doc["proxyName"].as<String>());
    restoreBool("obdEnabled", settings.obdEnabled);
    restoreBool("obdVwDataEnabled", settings.obdVwDataEnabled);
    restoreBool("gpsEnabled", settings.gpsEnabled);
    restoreBool("cameraEnabled", settings.cameraEnabled);
    if (doc["gpsLockoutMode"].is<int>()) {
        settings.gpsLockoutMode = clampLockoutRuntimeModeValue(doc["gpsLockoutMode"].as<int>());
    } else if (doc["gpsLockoutMode"].is<const char*>()) {
        String mode = doc["gpsLockoutMode"].as<String>();
        mode.toLowerCase();
        if (mode == "shadow") {
            settings.gpsLockoutMode = LOCKOUT_RUNTIME_SHADOW;
        } else if (mode == "advisory") {
            settings.gpsLockoutMode = LOCKOUT_RUNTIME_ADVISORY;
        } else if (mode == "enforce") {
            settings.gpsLockoutMode = LOCKOUT_RUNTIME_ENFORCE;
        } else {
            settings.gpsLockoutMode = LOCKOUT_RUNTIME_OFF;
        }
    }
    restoreBool("gpsLockoutCoreGuardEnabled", settings.gpsLockoutCoreGuardEnabled);
    if (doc["gpsLockoutMaxQueueDrops"].is<int>()) {
        settings.gpsLockoutMaxQueueDrops = static_cast<uint16_t>(
            std::max(0, std::min(doc["gpsLockoutMaxQueueDrops"].as<int>(), 65535)));
    }
    if (doc["gpsLockoutMaxPerfDrops"].is<int>()) {
        settings.gpsLockoutMaxPerfDrops = static_cast<uint16_t>(
            std::max(0, std::min(doc["gpsLockoutMaxPerfDrops"].as<int>(), 65535)));
    }
    if (doc["gpsLockoutMaxEventBusDrops"].is<int>()) {
        settings.gpsLockoutMaxEventBusDrops = static_cast<uint16_t>(
            std::max(0, std::min(doc["gpsLockoutMaxEventBusDrops"].as<int>(), 65535)));
    }
    if (doc["gpsLockoutLearnerPromotionHits"].is<int>()) {
        settings.gpsLockoutLearnerPromotionHits = clampLockoutLearnerHitsValue(
            doc["gpsLockoutLearnerPromotionHits"].as<int>());
    }
    if (doc["gpsLockoutLearnerRadiusE5"].is<int>()) {
        settings.gpsLockoutLearnerRadiusE5 = clampLockoutLearnerRadiusE5Value(
            doc["gpsLockoutLearnerRadiusE5"].as<int>());
    }
    if (doc["gpsLockoutLearnerFreqToleranceMHz"].is<int>()) {
        settings.gpsLockoutLearnerFreqToleranceMHz = clampLockoutLearnerFreqTolValue(
            doc["gpsLockoutLearnerFreqToleranceMHz"].as<int>());
    }
    if (doc["gpsLockoutLearnerLearnIntervalHours"].is<int>()) {
        settings.gpsLockoutLearnerLearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
            doc["gpsLockoutLearnerLearnIntervalHours"].as<int>());
    }
    if (doc["gpsLockoutLearnerUnlearnIntervalHours"].is<int>()) {
        settings.gpsLockoutLearnerUnlearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
            doc["gpsLockoutLearnerUnlearnIntervalHours"].as<int>());
    }
    if (doc["gpsLockoutLearnerUnlearnCount"].is<int>()) {
        settings.gpsLockoutLearnerUnlearnCount = clampLockoutLearnerUnlearnCountValue(
            doc["gpsLockoutLearnerUnlearnCount"].as<int>());
    }
    if (doc["gpsLockoutManualDemotionMissCount"].is<int>()) {
        settings.gpsLockoutManualDemotionMissCount = clampLockoutManualDemotionMissCountValue(
            doc["gpsLockoutManualDemotionMissCount"].as<int>());
    }
    restoreBool("gpsLockoutKaLearningEnabled", settings.gpsLockoutKaLearningEnabled);
    restoreBool("gpsLockoutPreQuiet", settings.gpsLockoutPreQuiet);
    if (doc["lastV1Address"].is<const char*>()) settings.lastV1Address = sanitizeLastV1AddressValue(doc["lastV1Address"].as<String>());
    if (doc["autoPowerOffMinutes"].is<int>()) {
        settings.autoPowerOffMinutes = clampU8(doc["autoPowerOffMinutes"].as<int>(), 0, 60);
    }
    if (doc["apTimeoutMinutes"].is<int>()) {
        settings.apTimeoutMinutes = clampApTimeoutValue(doc["apTimeoutMinutes"].as<int>());
    }
    
    
    // === Display Settings ===
    if (doc["brightness"].is<int>()) settings.brightness = clampU8(doc["brightness"].as<int>(), 1, 255);
    restoreBool("turnOffDisplay", settings.turnOffDisplay);
    if (doc["displayStyle"].is<int>()) settings.displayStyle = normalizeDisplayStyle(doc["displayStyle"].as<int>());
    
    // === All Colors ===
    if (doc["colorBogey"].is<int>()) settings.colorBogey = doc["colorBogey"];
    if (doc["colorFrequency"].is<int>()) settings.colorFrequency = doc["colorFrequency"];
    if (doc["colorArrowFront"].is<int>()) settings.colorArrowFront = doc["colorArrowFront"];
    if (doc["colorArrowSide"].is<int>()) settings.colorArrowSide = doc["colorArrowSide"];
    if (doc["colorArrowRear"].is<int>()) settings.colorArrowRear = doc["colorArrowRear"];
    if (doc["colorBandL"].is<int>()) settings.colorBandL = doc["colorBandL"];
    if (doc["colorBandKa"].is<int>()) settings.colorBandKa = doc["colorBandKa"];
    if (doc["colorBandK"].is<int>()) settings.colorBandK = doc["colorBandK"];
    if (doc["colorBandX"].is<int>()) settings.colorBandX = doc["colorBandX"];
    if (doc["colorBandPhoto"].is<int>()) settings.colorBandPhoto = doc["colorBandPhoto"];
    if (doc["colorWiFiIcon"].is<int>()) settings.colorWiFiIcon = doc["colorWiFiIcon"];
    if (doc["colorWiFiConnected"].is<int>()) settings.colorWiFiConnected = doc["colorWiFiConnected"];
    if (doc["colorBleConnected"].is<int>()) settings.colorBleConnected = doc["colorBleConnected"];
    if (doc["colorBleDisconnected"].is<int>()) settings.colorBleDisconnected = doc["colorBleDisconnected"];
    if (doc["colorBar1"].is<int>()) settings.colorBar1 = doc["colorBar1"];
    if (doc["colorBar2"].is<int>()) settings.colorBar2 = doc["colorBar2"];
    if (doc["colorBar3"].is<int>()) settings.colorBar3 = doc["colorBar3"];
    if (doc["colorBar4"].is<int>()) settings.colorBar4 = doc["colorBar4"];
    if (doc["colorBar5"].is<int>()) settings.colorBar5 = doc["colorBar5"];
    if (doc["colorBar6"].is<int>()) settings.colorBar6 = doc["colorBar6"];
    if (doc["colorMuted"].is<int>()) settings.colorMuted = doc["colorMuted"];
    if (doc["colorPersisted"].is<int>()) settings.colorPersisted = doc["colorPersisted"];
    if (doc["colorVolumeMain"].is<int>()) settings.colorVolumeMain = doc["colorVolumeMain"];
    if (doc["colorVolumeMute"].is<int>()) settings.colorVolumeMute = doc["colorVolumeMute"];
    if (doc["colorRssiV1"].is<int>()) settings.colorRssiV1 = doc["colorRssiV1"];
    if (doc["colorRssiProxy"].is<int>()) settings.colorRssiProxy = doc["colorRssiProxy"];
    if (doc["colorCameraToken"].is<int>()) settings.colorCameraToken = doc["colorCameraToken"];
    if (doc["colorCameraArrow"].is<int>()) settings.colorCameraArrow = doc["colorCameraArrow"];
    if (doc["colorLockout"].is<int>()) settings.colorLockout = doc["colorLockout"];
    if (doc["colorGps"].is<int>()) settings.colorGps = doc["colorGps"];
    if (doc["colorObd"].is<int>()) settings.colorObd = doc["colorObd"];
    restoreBool("freqUseBandColor", settings.freqUseBandColor);
    
    // === UI Toggles ===
    restoreBool("hideWifiIcon", settings.hideWifiIcon);
    restoreBool("hideProfileIndicator", settings.hideProfileIndicator);
    restoreBool("hideBatteryIcon", settings.hideBatteryIcon);
    restoreBool("showBatteryPercent", settings.showBatteryPercent);
    restoreBool("hideBleIcon", settings.hideBleIcon);
    restoreBool("hideVolumeIndicator", settings.hideVolumeIndicator);
    restoreBool("hideRssiIndicator", settings.hideRssiIndicator);
    restoreBool("showRestTelemetryCards", settings.showRestTelemetryCards);
    restoreBool("enableWifiAtBoot", settings.enableWifiAtBoot);
    restoreBool("enableSignalTraceLogging", settings.enableSignalTraceLogging);
    
    // === Voice Settings ===
    if (doc["voiceAlertMode"].is<int>()) {
        settings.voiceAlertMode = clampVoiceAlertModeValue(doc["voiceAlertMode"].as<int>());
    } else {
        bool legacyVoiceEnabled = false;
        if (parseBoolVariant(doc["voiceAlertsEnabled"], legacyVoiceEnabled)) {
            settings.voiceAlertMode = legacyVoiceEnabled ? VOICE_MODE_BAND_FREQ : VOICE_MODE_DISABLED;
        }
    }
    restoreBool("voiceDirectionEnabled", settings.voiceDirectionEnabled);
    restoreBool("announceBogeyCount", settings.announceBogeyCount);
    restoreBool("muteVoiceIfVolZero", settings.muteVoiceIfVolZero);
    if (doc["voiceVolume"].is<int>()) settings.voiceVolume = clampU8(doc["voiceVolume"].as<int>(), 0, 100);
    restoreBool("announceSecondaryAlerts", settings.announceSecondaryAlerts);
    restoreBool("secondaryLaser", settings.secondaryLaser);
    restoreBool("secondaryKa", settings.secondaryKa);
    restoreBool("secondaryK", settings.secondaryK);
    restoreBool("secondaryX", settings.secondaryX);
    restoreBool("alertVolumeFadeEnabled", settings.alertVolumeFadeEnabled);
    if (doc["alertVolumeFadeDelaySec"].is<int>()) {
        settings.alertVolumeFadeDelaySec = clampU8(doc["alertVolumeFadeDelaySec"].as<int>(), 1, 10);
    }
    if (doc["alertVolumeFadeVolume"].is<int>()) {
        settings.alertVolumeFadeVolume = clampU8(doc["alertVolumeFadeVolume"].as<int>(), 0, 9);
    }
    restoreBool("speedVolumeEnabled", settings.speedVolumeEnabled);
    if (doc["speedVolumeThresholdMph"].is<int>()) {
        settings.speedVolumeThresholdMph = clampU8(doc["speedVolumeThresholdMph"].as<int>(), 10, 100);
    }
    if (doc["speedVolumeBoost"].is<int>()) {
        settings.speedVolumeBoost = clampU8(doc["speedVolumeBoost"].as<int>(), 1, 5);
    }
    restoreBool("lowSpeedMuteEnabled", settings.lowSpeedMuteEnabled);
    if (doc["lowSpeedMuteThresholdMph"].is<int>()) {
        settings.lowSpeedMuteThresholdMph = clampU8(doc["lowSpeedMuteThresholdMph"].as<int>(), 1, 30);
    }
    if (doc["lowSpeedVolume"].is<int>()) {
        settings.lowSpeedVolume = clampU8(doc["lowSpeedVolume"].as<int>(), 0, 9);
    }
    
    // === Auto-Push Settings ===
    // Only allow backup to enable auto-push (avoid stale backups disabling it)
    bool backupAutoPushEnabled = false;
    if (parseBoolVariant(doc["autoPushEnabled"], backupAutoPushEnabled) && backupAutoPushEnabled) {
        settings.autoPushEnabled = true;
    }
    if (doc["activeSlot"].is<int>()) settings.activeSlot = std::max(0, std::min(doc["activeSlot"].as<int>(), 2));
    
    // === Slot 0 Full Settings ===
    if (doc["slot0Name"].is<const char*>()) settings.slot0Name = sanitizeSlotNameValue(doc["slot0Name"].as<String>());
    if (doc["slot0Color"].is<int>()) settings.slot0Color = doc["slot0Color"];
    if (doc["slot0Volume"].is<int>()) settings.slot0Volume = clampSlotVolumeValue(doc["slot0Volume"].as<int>());
    if (doc["slot0MuteVolume"].is<int>()) settings.slot0MuteVolume = clampSlotVolumeValue(doc["slot0MuteVolume"].as<int>());
    restoreBool("slot0DarkMode", settings.slot0DarkMode);
    restoreBool("slot0MuteToZero", settings.slot0MuteToZero);
    if (doc["slot0AlertPersist"].is<int>()) settings.slot0AlertPersist = clampU8(doc["slot0AlertPersist"].as<int>(), 0, 5);
    restoreBool("slot0PriorityArrow", settings.slot0PriorityArrow);
    if (doc["slot0ProfileName"].is<const char*>()) settings.slot0_default.profileName = sanitizeProfileNameValue(doc["slot0ProfileName"].as<String>());
    if (doc["slot0Mode"].is<int>()) settings.slot0_default.mode = normalizeV1ModeValue(doc["slot0Mode"].as<int>());
    
    // === Slot 1 Full Settings ===
    if (doc["slot1Name"].is<const char*>()) settings.slot1Name = sanitizeSlotNameValue(doc["slot1Name"].as<String>());
    if (doc["slot1Color"].is<int>()) settings.slot1Color = doc["slot1Color"];
    if (doc["slot1Volume"].is<int>()) settings.slot1Volume = clampSlotVolumeValue(doc["slot1Volume"].as<int>());
    if (doc["slot1MuteVolume"].is<int>()) settings.slot1MuteVolume = clampSlotVolumeValue(doc["slot1MuteVolume"].as<int>());
    restoreBool("slot1DarkMode", settings.slot1DarkMode);
    restoreBool("slot1MuteToZero", settings.slot1MuteToZero);
    if (doc["slot1AlertPersist"].is<int>()) settings.slot1AlertPersist = clampU8(doc["slot1AlertPersist"].as<int>(), 0, 5);
    restoreBool("slot1PriorityArrow", settings.slot1PriorityArrow);
    if (doc["slot1ProfileName"].is<const char*>()) settings.slot1_highway.profileName = sanitizeProfileNameValue(doc["slot1ProfileName"].as<String>());
    if (doc["slot1Mode"].is<int>()) settings.slot1_highway.mode = normalizeV1ModeValue(doc["slot1Mode"].as<int>());
    
    // === Slot 2 Full Settings ===
    if (doc["slot2Name"].is<const char*>()) settings.slot2Name = sanitizeSlotNameValue(doc["slot2Name"].as<String>());
    if (doc["slot2Color"].is<int>()) settings.slot2Color = doc["slot2Color"];
    if (doc["slot2Volume"].is<int>()) settings.slot2Volume = clampSlotVolumeValue(doc["slot2Volume"].as<int>());
    if (doc["slot2MuteVolume"].is<int>()) settings.slot2MuteVolume = clampSlotVolumeValue(doc["slot2MuteVolume"].as<int>());
    restoreBool("slot2DarkMode", settings.slot2DarkMode);
    restoreBool("slot2MuteToZero", settings.slot2MuteToZero);
    if (doc["slot2AlertPersist"].is<int>()) settings.slot2AlertPersist = clampU8(doc["slot2AlertPersist"].as<int>(), 0, 5);
    restoreBool("slot2PriorityArrow", settings.slot2PriorityArrow);
    if (doc["slot2ProfileName"].is<const char*>()) settings.slot2_comfort.profileName = sanitizeProfileNameValue(doc["slot2ProfileName"].as<String>());
    if (doc["slot2Mode"].is<int>()) settings.slot2_comfort.mode = normalizeV1ModeValue(doc["slot2Mode"].as<int>());

    // Restore V1 profiles if present in backup
    int profilesRestored = 0;
    if (v1ProfileManager.isReady() && doc["profiles"].is<JsonArray>()) {
        JsonArray profilesArr = doc["profiles"].as<JsonArray>();
        for (JsonObject p : profilesArr) {
            if (!p["name"].is<const char*>() || !p["bytes"].is<JsonArray>()) {
                continue;
            }

            JsonArray bytes = p["bytes"].as<JsonArray>();
            if (bytes.size() != 6) {
                continue;
            }

            V1Profile profile;
            profile.name = sanitizeProfileNameValue(p["name"].as<String>());
            if (profile.name.length() == 0) {
                continue;
            }
            if (p["description"].is<const char*>()) {
                profile.description = sanitizeProfileDescriptionValue(p["description"].as<String>());
            }
            bool profileDisplayOn = false;
            if (parseBoolVariant(p["displayOn"], profileDisplayOn)) {
                profile.displayOn = profileDisplayOn;
            }
            if (p["mainVolume"].is<int>()) profile.mainVolume = clampSlotVolumeValue(p["mainVolume"].as<int>());
            if (p["mutedVolume"].is<int>()) profile.mutedVolume = clampSlotVolumeValue(p["mutedVolume"].as<int>());

            for (int i = 0; i < 6; i++) {
                profile.settings.bytes[i] = bytes[i].as<uint8_t>();
            }

            ProfileSaveResult result = v1ProfileManager.saveProfile(profile);
            if (result.success) {
                profilesRestored++;
            } else {
                Serial.printf("[Settings] Failed to restore profile '%s': %s\n",
                              profile.name.c_str(),
                              result.error.c_str());
            }
        }
    }
    
    // Debug: log what modes were restored
    Serial.printf("[Settings] Restored modes from backup: slot0Mode=%d (in json: %s), slot1Mode=%d (in json: %s), slot2Mode=%d (in json: %s)\n",
                  settings.slot0_default.mode, doc["slot0Mode"].is<int>() ? "yes" : "NO",
                  settings.slot1_highway.mode, doc["slot1Mode"].is<int>() ? "yes" : "NO",
                  settings.slot2_comfort.mode, doc["slot2Mode"].is<int>() ? "yes" : "NO");
    
    if (!persistSettingsAtomically()) {
        Serial.println("[Settings] ERROR: Failed to persist restored settings");
        return false;
    }

    Serial.printf("[Settings] ✅ Full restore from SD backup complete (%d profiles)\n", profilesRestored);
    return true;
}


void SettingsManager::validateProfileReferences(V1ProfileManager& profileMgr) {
    if (!profileMgr.isReady()) {
        Serial.println("[Settings] Profile manager not ready; skipping profile reference validation");
        return;
    }

    // Validate that profile names in auto-push slots actually exist
    // If not, clear them to prevent repeated "file not found" errors
    bool needsSave = false;
    
    auto validateSlot = [&](AutoPushSlot& slot, const char* slotName) {
        if (slot.profileName.length() > 0) {
            V1Profile testProfile;
            if (!profileMgr.loadProfile(slot.profileName, testProfile)) {
                Serial.printf("[Settings] WARNING: Profile '%s' for %s does not exist - clearing reference\n",
                             slot.profileName.c_str(), slotName);
                slot.profileName = "";
                needsSave = true;
            } else {
                Serial.printf("[Settings] Profile '%s' for %s validated OK\n",
                             slot.profileName.c_str(), slotName);
            }
        }
    };
    
    validateSlot(settings.slot0_default, "Slot 0 (Default)");
    validateSlot(settings.slot1_highway, "Slot 1 (Highway)");
    validateSlot(settings.slot2_comfort, "Slot 2 (Comfort)");
    
    if (needsSave) {
        if (persistSettingsAtomically()) {
            Serial.println("[Settings] Cleared invalid profile references and saved");
        } else {
            Serial.println("[Settings] ERROR: Failed to persist cleared profile references");
        }
    }

    // No additional side effects needed beyond clearing invalid references.
}

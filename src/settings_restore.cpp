/**
 * Settings SD restore and validation paths.
 * Extracted from settings_backup.cpp to keep backup writer focused.
 */

#include "settings_internals.h"
#include <nvs.h>

bool shouldSkipProfileReferenceValidation(size_t availableProfileCount,
                                          bool hasConfiguredSlotReferences) {
    return availableProfileCount == 0 && hasConfiguredSlotReferences;
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

SettingsBackupApplyResult SettingsManager::applyBackupDocument(const JsonDocument& doc,
                                                              bool deferBackupRewrite) {
    SettingsBackupApplyResult result;

    auto restoreBool = [&](const char* key, bool& target) {
        bool parsed = false;
        if (parseBoolVariant(doc[key], parsed)) {
            target = parsed;
        }
    };

    // ============================================================================
    // WiFi/Network Settings
    // ============================================================================
    // AP password is intentionally preserved across backup restores.
    restoreBool("enableWifi", settings_.enableWifi);
    if (doc["wifiMode"].is<int>()) settings_.wifiMode = clampWifiModeValue(doc["wifiMode"].as<int>());
    if (doc["apSSID"].is<const char*>()) settings_.apSSID = sanitizeApSsidValue(doc["apSSID"].as<String>());
    restoreBool("wifiClientEnabled", settings_.wifiClientEnabled);
    if (doc["wifiClientSSID"].is<const char*>()) {
        settings_.wifiClientSSID = sanitizeWifiClientSsidValue(doc["wifiClientSSID"].as<String>());
    }
    if (!settings_.wifiClientEnabled && settings_.wifiClientSSID.length() > 0) {
        settings_.wifiClientEnabled = true;
    }
    settings_.wifiMode = settings_.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;
    restoreBool("proxyBLE", settings_.proxyBLE);
    if (doc["proxyName"].is<const char*>()) {
        settings_.proxyName = sanitizeProxyNameValue(doc["proxyName"].as<String>());
    }
    if (doc["lastV1Address"].is<const char*>()) {
        settings_.lastV1Address = sanitizeLastV1AddressValue(doc["lastV1Address"].as<String>());
    }
    if (doc["autoPowerOffMinutes"].is<int>()) {
        settings_.autoPowerOffMinutes = clampU8(doc["autoPowerOffMinutes"].as<int>(), 0, 60);
    }
    if (doc["apTimeoutMinutes"].is<int>()) {
        settings_.apTimeoutMinutes = clampApTimeoutValue(doc["apTimeoutMinutes"].as<int>());
    }

    // ============================================================================
    // Display Settings
    // ============================================================================
    if (doc["brightness"].is<int>()) settings_.brightness = clampU8(doc["brightness"].as<int>(), 1, 255);
    restoreBool("turnOffDisplay", settings_.turnOffDisplay);
    if (doc["displayStyle"].is<int>()) settings_.displayStyle = normalizeDisplayStyle(doc["displayStyle"].as<int>());

    // ============================================================================
    // All Colors (sanitized identically to the NVS-load path in settings.cpp)
    // ============================================================================
    if (doc["colorBogey"].is<int>()) settings_.colorBogey = sanitizeRgb565Color(doc["colorBogey"], 0xF800);
    if (doc["colorFrequency"].is<int>()) settings_.colorFrequency = sanitizeRgb565Color(doc["colorFrequency"], 0xF800);
    if (doc["colorArrowFront"].is<int>()) settings_.colorArrowFront = sanitizeRgb565Color(doc["colorArrowFront"], 0xF800);
    if (doc["colorArrowSide"].is<int>()) settings_.colorArrowSide = sanitizeRgb565Color(doc["colorArrowSide"], 0xF800);
    if (doc["colorArrowRear"].is<int>()) settings_.colorArrowRear = sanitizeRgb565Color(doc["colorArrowRear"], 0xF800);
    if (doc["colorBandL"].is<int>()) settings_.colorBandL = sanitizeRgb565Color(doc["colorBandL"], 0x001F);
    if (doc["colorBandKa"].is<int>()) settings_.colorBandKa = sanitizeRgb565Color(doc["colorBandKa"], 0xF800);
    if (doc["colorBandK"].is<int>()) settings_.colorBandK = sanitizeRgb565Color(doc["colorBandK"], 0x001F);
    if (doc["colorBandX"].is<int>()) settings_.colorBandX = sanitizeRgb565Color(doc["colorBandX"], 0x07E0);
    if (doc["colorBandPhoto"].is<int>()) settings_.colorBandPhoto = sanitizeRgb565Color(doc["colorBandPhoto"], 0x780F);
    if (doc["colorWiFiIcon"].is<int>()) settings_.colorWiFiIcon = sanitizeRgb565Color(doc["colorWiFiIcon"], 0x07FF);
    if (doc["colorWiFiConnected"].is<int>()) settings_.colorWiFiConnected = sanitizeRgb565Color(doc["colorWiFiConnected"], 0x07E0);
    if (doc["colorBleConnected"].is<int>()) settings_.colorBleConnected = sanitizeRgb565Color(doc["colorBleConnected"], 0x07E0);
    if (doc["colorBleDisconnected"].is<int>()) settings_.colorBleDisconnected = sanitizeRgb565Color(doc["colorBleDisconnected"], 0x001F);
    if (doc["colorBar1"].is<int>()) settings_.colorBar1 = sanitizeRgb565Color(doc["colorBar1"], 0x07E0);
    if (doc["colorBar2"].is<int>()) settings_.colorBar2 = sanitizeRgb565Color(doc["colorBar2"], 0x07E0);
    if (doc["colorBar3"].is<int>()) settings_.colorBar3 = sanitizeRgb565Color(doc["colorBar3"], 0xFFE0);
    if (doc["colorBar4"].is<int>()) settings_.colorBar4 = sanitizeRgb565Color(doc["colorBar4"], 0xFFE0);
    if (doc["colorBar5"].is<int>()) settings_.colorBar5 = sanitizeRgb565Color(doc["colorBar5"], 0xF800);
    if (doc["colorBar6"].is<int>()) settings_.colorBar6 = sanitizeRgb565Color(doc["colorBar6"], 0xF800);
    if (doc["colorMuted"].is<int>()) settings_.colorMuted = sanitizeRgb565Color(doc["colorMuted"], 0x3186);
    if (doc["colorPersisted"].is<int>()) settings_.colorPersisted = sanitizeRgb565Color(doc["colorPersisted"], 0x18C3);
    if (doc["colorVolumeMain"].is<int>()) settings_.colorVolumeMain = sanitizeRgb565Color(doc["colorVolumeMain"], 0xF800);
    if (doc["colorVolumeMute"].is<int>()) settings_.colorVolumeMute = sanitizeRgb565Color(doc["colorVolumeMute"], 0x7BEF);
    if (doc["colorRssiV1"].is<int>()) settings_.colorRssiV1 = sanitizeRgb565Color(doc["colorRssiV1"], 0x07E0);
    if (doc["colorRssiProxy"].is<int>()) settings_.colorRssiProxy = sanitizeRgb565Color(doc["colorRssiProxy"], 0x001F);
    if (doc["colorObd"].is<int>()) settings_.colorObd = sanitizeRgb565Color(doc["colorObd"], 0x001F);
    restoreBool("freqUseBandColor", settings_.freqUseBandColor);

    // ============================================================================
    // UI Toggles
    // ============================================================================
    restoreBool("hideWifiIcon", settings_.hideWifiIcon);
    restoreBool("hideProfileIndicator", settings_.hideProfileIndicator);
    restoreBool("hideBatteryIcon", settings_.hideBatteryIcon);
    restoreBool("showBatteryPercent", settings_.showBatteryPercent);
    restoreBool("hideBleIcon", settings_.hideBleIcon);
    restoreBool("hideVolumeIndicator", settings_.hideVolumeIndicator);
    restoreBool("hideRssiIndicator", settings_.hideRssiIndicator);
    restoreBool("enableWifiAtBoot", settings_.enableWifiAtBoot);

    // ============================================================================
    // Voice Settings
    // ============================================================================
    if (doc["voiceAlertMode"].is<int>()) {
        settings_.voiceAlertMode = clampVoiceAlertModeValue(doc["voiceAlertMode"].as<int>());
    } else {
        bool legacyVoiceEnabled = false;
        if (parseBoolVariant(doc["voiceAlertsEnabled"], legacyVoiceEnabled)) {
            settings_.voiceAlertMode = legacyVoiceEnabled ? VOICE_MODE_BAND_FREQ : VOICE_MODE_DISABLED;
        }
    }
    restoreBool("voiceDirectionEnabled", settings_.voiceDirectionEnabled);
    restoreBool("announceBogeyCount", settings_.announceBogeyCount);
    restoreBool("muteVoiceIfVolZero", settings_.muteVoiceIfVolZero);
    if (doc["voiceVolume"].is<int>()) settings_.voiceVolume = clampU8(doc["voiceVolume"].as<int>(), 0, 100);
    restoreBool("announceSecondaryAlerts", settings_.announceSecondaryAlerts);
    restoreBool("secondaryLaser", settings_.secondaryLaser);
    restoreBool("secondaryKa", settings_.secondaryKa);
    restoreBool("secondaryK", settings_.secondaryK);
    restoreBool("secondaryX", settings_.secondaryX);
    restoreBool("alertVolumeFadeEnabled", settings_.alertVolumeFadeEnabled);
    if (doc["alertVolumeFadeDelaySec"].is<int>()) {
        settings_.alertVolumeFadeDelaySec = clampU8(doc["alertVolumeFadeDelaySec"].as<int>(), 1, 10);
    }
    if (doc["alertVolumeFadeVolume"].is<int>()) {
        settings_.alertVolumeFadeVolume = clampU8(doc["alertVolumeFadeVolume"].as<int>(), 0, 9);
    }
    restoreBool("speedMuteEnabled", settings_.speedMuteEnabled);
    if (doc["speedMuteThresholdMph"].is<int>()) {
        settings_.speedMuteThresholdMph = clampU8(doc["speedMuteThresholdMph"].as<int>(), 5, 60);
    }
    if (doc["speedMuteHysteresisMph"].is<int>()) {
        settings_.speedMuteHysteresisMph = clampU8(doc["speedMuteHysteresisMph"].as<int>(), 1, 10);
    }
    if (doc["speedMuteVolume"].is<int>()) {
        int raw = doc["speedMuteVolume"].as<int>();
        settings_.speedMuteVolume = (raw >= 0 && raw <= 9) ? static_cast<uint8_t>(raw) : 0xFF;
    }

    // ============================================================================
    // Auto-Push Settings
    // ============================================================================
    restoreBool("autoPushEnabled", settings_.autoPushEnabled);
    if (doc["activeSlot"].is<int>()) settings_.activeSlot = std::max(0, std::min(doc["activeSlot"].as<int>(), 2));

    if (doc["slot0Name"].is<const char*>()) settings_.slot0Name = sanitizeSlotNameValue(doc["slot0Name"].as<String>());
    if (doc["slot0Color"].is<int>()) settings_.slot0Color = doc["slot0Color"];
    if (doc["slot0Volume"].is<int>()) settings_.slot0Volume = clampSlotVolumeValue(doc["slot0Volume"].as<int>());
    if (doc["slot0MuteVolume"].is<int>()) settings_.slot0MuteVolume = clampSlotVolumeValue(doc["slot0MuteVolume"].as<int>());
    restoreBool("slot0DarkMode", settings_.slot0DarkMode);
    restoreBool("slot0MuteToZero", settings_.slot0MuteToZero);
    if (doc["slot0AlertPersist"].is<int>()) settings_.slot0AlertPersist = clampU8(doc["slot0AlertPersist"].as<int>(), 0, 5);
    restoreBool("slot0PriorityArrow", settings_.slot0PriorityArrow);
    if (doc["slot0ProfileName"].is<const char*>()) settings_.slot0_default.profileName = sanitizeProfileNameValue(doc["slot0ProfileName"].as<String>());
    if (doc["slot0Mode"].is<int>()) settings_.slot0_default.mode = normalizeV1ModeValue(doc["slot0Mode"].as<int>());

    if (doc["slot1Name"].is<const char*>()) settings_.slot1Name = sanitizeSlotNameValue(doc["slot1Name"].as<String>());
    if (doc["slot1Color"].is<int>()) settings_.slot1Color = doc["slot1Color"];
    if (doc["slot1Volume"].is<int>()) settings_.slot1Volume = clampSlotVolumeValue(doc["slot1Volume"].as<int>());
    if (doc["slot1MuteVolume"].is<int>()) settings_.slot1MuteVolume = clampSlotVolumeValue(doc["slot1MuteVolume"].as<int>());
    restoreBool("slot1DarkMode", settings_.slot1DarkMode);
    restoreBool("slot1MuteToZero", settings_.slot1MuteToZero);
    if (doc["slot1AlertPersist"].is<int>()) settings_.slot1AlertPersist = clampU8(doc["slot1AlertPersist"].as<int>(), 0, 5);
    restoreBool("slot1PriorityArrow", settings_.slot1PriorityArrow);
    if (doc["slot1ProfileName"].is<const char*>()) settings_.slot1_highway.profileName = sanitizeProfileNameValue(doc["slot1ProfileName"].as<String>());
    if (doc["slot1Mode"].is<int>()) settings_.slot1_highway.mode = normalizeV1ModeValue(doc["slot1Mode"].as<int>());

    if (doc["slot2Name"].is<const char*>()) settings_.slot2Name = sanitizeSlotNameValue(doc["slot2Name"].as<String>());
    if (doc["slot2Color"].is<int>()) settings_.slot2Color = doc["slot2Color"];
    if (doc["slot2Volume"].is<int>()) settings_.slot2Volume = clampSlotVolumeValue(doc["slot2Volume"].as<int>());
    if (doc["slot2MuteVolume"].is<int>()) settings_.slot2MuteVolume = clampSlotVolumeValue(doc["slot2MuteVolume"].as<int>());
    restoreBool("slot2DarkMode", settings_.slot2DarkMode);
    restoreBool("slot2MuteToZero", settings_.slot2MuteToZero);
    if (doc["slot2AlertPersist"].is<int>()) settings_.slot2AlertPersist = clampU8(doc["slot2AlertPersist"].as<int>(), 0, 5);
    restoreBool("slot2PriorityArrow", settings_.slot2PriorityArrow);
    if (doc["slot2ProfileName"].is<const char*>()) settings_.slot2_comfort.profileName = sanitizeProfileNameValue(doc["slot2ProfileName"].as<String>());
    if (doc["slot2Mode"].is<int>()) settings_.slot2_comfort.mode = normalizeV1ModeValue(doc["slot2Mode"].as<int>());

    // ============================================================================
    // OBD Settings
    // ============================================================================
    restoreBool("obdEnabled", settings_.obdEnabled);
    if (doc["obdSavedAddress"].is<const char*>()) {
        String addr = doc["obdSavedAddress"].as<String>();
        if (isValidBleAddress(addr)) {
            settings_.obdSavedAddress = addr;
        } else {
            Serial.printf("[Settings] WARN: Invalid OBD saved address in backup: '%s' — skipping\n", addr.c_str());
            settings_.obdSavedAddress = "";
        }
    }
    if (doc["obdSavedName"].is<const char*>()) settings_.obdSavedName = sanitizeObdSavedNameValue(doc["obdSavedName"].as<String>());
    if (doc["obdSavedAddrType"].is<int>()) {
        settings_.obdSavedAddrType = static_cast<uint8_t>(std::max(0, std::min(doc["obdSavedAddrType"].as<int>(), 1)));
    }
    if (doc["obdMinRssi"].is<int>()) {
        const int rssi = doc["obdMinRssi"].as<int>();
        settings_.obdMinRssi = static_cast<int8_t>(std::max(-100, std::min(rssi, -40)));
    }

    int profilesRestored = 0;
    if (v1ProfileManager.isReady() && doc["profiles"].is<JsonArrayConst>()) {
        JsonArrayConst profilesArr = doc["profiles"].as<JsonArrayConst>();
        for (JsonObjectConst p : profilesArr) {
            if (!p["name"].is<const char*>() || !p["bytes"].is<JsonArrayConst>()) {
                continue;
            }

            JsonArrayConst bytes = p["bytes"].as<JsonArrayConst>();
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

            ProfileSaveResult saveResult = v1ProfileManager.saveProfile(profile);
            if (saveResult.success) {
                profilesRestored++;
            } else {
                Serial.printf("[Settings] Failed to restore profile '%s': %s\n",
                              profile.name.c_str(),
                              saveResult.error.c_str());
            }
        }
    }

    if (deferBackupRewrite) {
        saveDeferredBackup();
    } else {
        if (!persistSettingsAtomically()) {
            Serial.println("[Settings] ERROR: Failed to persist restored settings_");
            return result;
        }
        bumpBackupRevision();
    }

    result.success = true;
    result.profilesRestored = profilesRestored;
    return result;
}

bool backupFieldMatchesBool(const JsonDocument& doc, const char* key, bool expected) {
    bool parsed = false;
    return parseBoolVariant(doc[key], parsed) && parsed == expected;
}

bool backupFieldMatchesInt(const JsonDocument& doc, const char* key, int expected) {
    return doc[key].is<int>() && doc[key].as<int>() == expected;
}

bool backupFieldMatchesString(const JsonDocument& doc, const char* key, const String& expected) {
    return doc[key].is<const char*>() && String(doc[key].as<const char*>()) == expected;
}

bool backupAppearsInSyncWithNvs(const JsonDocument& doc, const V1Settings& current) {
    // Core fields that should track one-for-one between healthy NVS and SD backup.
    return
        backupFieldMatchesBool(doc, "enableWifi", current.enableWifi) &&
        backupFieldMatchesInt(doc, "wifiMode", static_cast<int>(current.wifiMode)) &&
        backupFieldMatchesBool(doc, "wifiClientEnabled", current.wifiClientEnabled) &&
        backupFieldMatchesString(doc, "wifiClientSSID", current.wifiClientSSID) &&
        backupFieldMatchesBool(doc, "proxyBLE", current.proxyBLE) &&
        backupFieldMatchesString(doc, "proxyName", current.proxyName) &&
        backupFieldMatchesInt(doc, "brightness", current.brightness) &&
        backupFieldMatchesInt(doc, "displayStyle", static_cast<int>(current.displayStyle)) &&
        backupFieldMatchesBool(doc, "autoPushEnabled", current.autoPushEnabled) &&
        backupFieldMatchesInt(doc, "activeSlot", current.activeSlot) &&
        backupFieldMatchesString(doc, "slot0ProfileName", current.slot0_default.profileName) &&
        backupFieldMatchesInt(doc, "slot0Mode", current.slot0_default.mode) &&
        backupFieldMatchesString(doc, "slot1ProfileName", current.slot1_highway.profileName) &&
        backupFieldMatchesInt(doc, "slot1Mode", current.slot1_highway.mode) &&
        backupFieldMatchesString(doc, "slot2ProfileName", current.slot2_comfort.profileName) &&
        backupFieldMatchesInt(doc, "slot2Mode", current.slot2_comfort.mode);
}

struct WifiClientKeyPresence {
    bool enabledKeyPresent = false;
    bool ssidKeyPresent = false;
};

WifiClientKeyPresence readWifiClientKeyPresence(const char* settingsNamespace) {
    WifiClientKeyPresence presence;
    if (!settingsNamespace || settingsNamespace[0] == '\0') {
        return presence;
    }

    Preferences prefs;
    if (!prefs.begin(settingsNamespace, true)) {
        return presence;
    }
    presence.enabledKeyPresent = prefs.isKey("wifiClientEn");
    presence.ssidKeyPresent = prefs.isKey("wifiClSSID");
    prefs.end();
    return presence;
}

struct WifiClientSecretPresence {
    bool valid = false;
    String ssid;
};

WifiClientSecretPresence readWifiClientSecretPresence(fs::FS* fs) {
    WifiClientSecretPresence presence;
    if (!fs || !fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
        return presence;
    }

    File file = fs->open(WIFI_CLIENT_SD_SECRET_PATH, FILE_READ);
    if (!file) {
        return presence;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
        Serial.printf("[Settings] WARN: Failed to parse WiFi secret '%s': %s\n",
                      WIFI_CLIENT_SD_SECRET_PATH,
                      err.c_str());
        return presence;
    }

    const char* type = doc["_type"] | "";
    if (strcmp(type, WIFI_CLIENT_SD_SECRET_TYPE) != 0) {
        return presence;
    }

    const char* secretSsid = doc["ssid"] | "";
    if (!secretSsid || secretSsid[0] == '\0') {
        return presence;
    }

    presence.valid = true;
    presence.ssid = sanitizeWifiClientSsidValue(String(secretSsid));
    return presence;
}

// --- Member methods: SD restore and validation ---

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
            Serial.println("[Settings] Restored settings_ from SD backup!");
            // Immediately re-emit backup in current schema after a successful restore.
            backupToSD();
            cleanupNamespacesIfNeeded(true);
            return true;
        }
        Serial.println("[Settings] Restore requested but no valid SD backup was applied");
    } else if (hasSdBackup) {
        // Keep user/NVS state authoritative unless corruption is detected.
        // Slot/profile healing is handled separately by validateProfileReferences().
        Serial.println("[Settings] NVS healthy; skipping automatic SD settings_ restore");
    }

    if (!needsRestore && storageManager.isReady() && storageManager.isSDCard()) {
        const WifiClientKeyPresence wifiKeyPresence =
            readWifiClientKeyPresence(getActiveNamespace().c_str());
        const bool wifiKeysMissing =
            !wifiKeyPresence.enabledKeyPresent || !wifiKeyPresence.ssidKeyPresent;
        const bool missingCurrentSsid = settings_.wifiClientSSID.length() == 0;

        if (wifiKeysMissing && !missingCurrentSsid) {
            // SSID is already present in memory; rewrite namespace to restore missing keys.
            settings_.wifiClientEnabled = true;
            settings_.wifiMode = V1_WIFI_APSTA;
            Serial.println("[Settings] HEAL: repairing missing WiFi client keys from in-memory SSID");
            save();
        } else if (missingCurrentSsid) {
            bool backupWifiClientEnabled = false;
            const bool backupEnabledKnown =
                hasSdBackup &&
                parseBoolVariant(bestBackupDoc["wifiClientEnabled"], backupWifiClientEnabled);
            const char* backupSsid = hasSdBackup ? (bestBackupDoc["wifiClientSSID"] | "") : "";
            const bool backupHasSsid = (backupSsid && backupSsid[0] != '\0');

            const WifiClientSecretPresence secretPresence = readWifiClientSecretPresence(fs);
            const bool secretHasSsid = secretPresence.valid && secretPresence.ssid.length() > 0;

            String recoveredSsid = "";
            const char* recoveredFrom = "none";
            if (backupHasSsid) {
                recoveredSsid = sanitizeWifiClientSsidValue(String(backupSsid));
                recoveredFrom = "settings_backup";
            } else if (secretHasSsid) {
                recoveredSsid = secretPresence.ssid;
                recoveredFrom = "wifi_secret";
            }

            // Targeted WiFi credential recovery:
            // - classic case: wifiClientEnabled=true but SSID missing
            // - partial-key case: WiFi client keys missing from NVS
            // - backup-missing case: recover SSID from SD WiFi secret metadata
            const bool shouldRecoverWifiClient =
                recoveredSsid.length() > 0 &&
                (settings_.wifiClientEnabled ||
                 wifiKeysMissing ||
                 (backupEnabledKnown && backupWifiClientEnabled) ||
                 secretHasSsid);

            if (shouldRecoverWifiClient) {
                settings_.wifiClientEnabled = true;
                settings_.wifiClientSSID = recoveredSsid;
                settings_.wifiMode = V1_WIFI_APSTA;
                Serial.printf("[Settings] HEAL: recovered WiFi client config from %s (ssid='%s', keysMissing=%s)\n",
                              recoveredFrom,
                              settings_.wifiClientSSID.c_str(),
                              wifiKeysMissing ? "yes" : "no");
                save();
            } else if (settings_.wifiClientEnabled) {
                // SSID missing in all recovery sources — disable to avoid inconsistent state.
                settings_.wifiClientEnabled = false;
                settings_.wifiMode = V1_WIFI_AP;
                Serial.println("[Settings] HEAL: wifiClientEnabled=true but no SSID anywhere — disabling");
                save();
            } else if (wifiKeysMissing) {
                Serial.println("[Settings] WARN: WiFi client keys missing and no SSID recovery source found");
            }
        }
    }

    // Keep SD backup schema fresh so newly added settings survive the next reflash.
    if (!needsRestore && storageManager.isReady() && storageManager.isSDCard()) {
        if (!hasSdBackup) {
            Serial.println("[Settings] No valid SD backup found; creating backup from current settings_");
            backupToSD();
        } else {
            const int backupVersion = backupDocumentVersion(bestBackupDoc);
            const bool missingCoreFields =
                bestBackupDoc["displayStyle"].isNull() ||
                bestBackupDoc["brightness"].isNull();
            const bool backupOutOfSync = !backupAppearsInSyncWithNvs(bestBackupDoc, settings_);
            if (backupVersion < SD_BACKUP_VERSION || missingCoreFields || backupOutOfSync) {
                Serial.printf("[Settings] Refreshing SD backup schema (path=%s version=%d)\n",
                              bestBackupPath ? bestBackupPath : "(unknown)",
                              backupVersion);
                if (backupOutOfSync) {
                    Serial.println("[Settings] SD backup differs from healthy NVS; refreshing backup content");
                }
                backupToSD();
            }
        }
    }
    cleanupNamespacesIfNeeded(hasSdBackup);
    return false;
}

void SettingsManager::cleanupNamespacesIfNeeded(bool hasSdBackup) {
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) != ESP_OK || stats.total_entries == 0) {
        return;
    }

    const uint32_t usedPct = (stats.used_entries * 100u) / stats.total_entries;
    const String activeNs = getActiveNamespace();
    const SettingsNamespaceCleanupPlan plan =
        buildSettingsNamespaceCleanupPlan(usedPct, activeNs, hasSdBackup);

    if (!plan.shouldCleanup) {
        if (usedPct > 80) {
            Serial.printf("[Settings] NVS high usage (%lu%%); deferring cleanup (active=%s backup=%s)\n",
                          static_cast<unsigned long>(usedPct),
                          activeNs.c_str(),
                          hasSdBackup ? "yes" : "no");
        }
        return;
    }

    auto clearNamespaceIfPresent = [](const char* ns, const char* label) {
        if (!ns || ns[0] == '\0' || namespaceHealthScore(ns) <= 0) {
            return;
        }
        Preferences prefs;
        if (prefs.begin(ns, false)) {
            prefs.clear();
            prefs.end();
            Serial.printf("[Settings] Cleared %s namespace %s\n", label, ns);
        }
    };

    Serial.printf("[Settings] NVS high usage (%lu%%); cleaning stale namespaces after active resolution (active=%s)\n",
                  static_cast<unsigned long>(usedPct),
                  activeNs.c_str());
    clearNamespaceIfPresent(plan.inactiveNamespace, "inactive");
    if (plan.clearLegacyNamespace) {
        clearNamespaceIfPresent(SETTINGS_NS_LEGACY, "legacy");
    }
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
    int nvsMarker = checkPrefs.getInt(kNvsValid, 0);
    int settingsVer = checkPrefs.getInt(kNvsSettingsVer, 0);
    bool missingCriticalKey = false;
    // These keys exist in all modern schemas and should never disappear in a healthy namespace.
    static constexpr const char* kCriticalKeys[] = {
        kNvsProxyBle,
        kNvsProxyName,
        kNvsBrightness,
        kNvsDispStyle,
        kNvsAutoPush
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
    // combined with missing settingsVer (which would be >= 2 if properly saved).
    // Only apply this heuristic when the commit marker is missing.
    if (nvsMarker == 0 && settingsVer <= 1 && settings_.brightness == 200) {
        Serial.println("[Settings] NVS appears default (v1 migration + default brightness)");
        return true;
    }

    // Any missing critical key means this namespace is not trustworthy,
    // regardless of marker/version combinations.
    if (missingCriticalKey) {
        Serial.println("[Settings] NVS appears partial/corrupt (critical keys missing)");
        return true;
    }

    // nvsValid means a full write completed; tolerate legacy/missing settingsVer
    // to avoid clobbering valid user settings with an older SD backup.

    // Detect incomplete writes: settingsVer is the FIRST key written and
    // nvsValid is the LAST.  If settingsVer exists but nvsValid does not,
    // the namespace was only partially written (crash/reset mid-save).
    if (nvsMarker == 0 && settingsVer >= SETTINGS_VERSION) {
        Serial.println("[Settings] NVS appears incomplete (settingsVer present but nvsValid missing)");
        return true;
    }

    return false;
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

    const SettingsBackupApplyResult applyResult = applyBackupDocument(doc, false);
    if (!applyResult.success) {
        return false;
    }
    Serial.printf("[Settings] Restored modes from backup: slot0Mode=%d (in json: %s), slot1Mode=%d (in json: %s), slot2Mode=%d (in json: %s)\n",
                  settings_.slot0_default.mode, doc["slot0Mode"].is<int>() ? "yes" : "NO",
                  settings_.slot1_highway.mode, doc["slot1Mode"].is<int>() ? "yes" : "NO",
                  settings_.slot2_comfort.mode, doc["slot2Mode"].is<int>() ? "yes" : "NO");
    Serial.printf("[Settings] ✅ Full restore from SD backup complete (%d profiles)\n",
                  applyResult.profilesRestored);
    return true;
}


void SettingsManager::validateProfileReferences(V1ProfileManager& profileMgr) {
    if (!profileMgr.isReady()) {
        Serial.println("[Settings] Profile manager not ready; skipping profile reference validation");
        return;
    }

    const bool hasConfiguredSlotReferences =
        settings_.slot0_default.profileName.length() > 0 ||
        settings_.slot1_highway.profileName.length() > 0 ||
        settings_.slot2_comfort.profileName.length() > 0;
    const size_t availableProfileCount = profileMgr.listProfiles().size();
    if (shouldSkipProfileReferenceValidation(availableProfileCount,
                                             hasConfiguredSlotReferences)) {
        Serial.println("[Settings] Profile catalog empty; preserving slot profile references");
        return;
    }

    // Validate that profile names in auto-push slots actually exist
    // If not, clear them to prevent repeated "file not found" errors
    bool needsSave = false;

    auto validateSlot = [&](AutoPushSlot& slot, const char* slotName) {
        if (slot.profileName.length() > 0) {
            V1Profile testProfile;
            if (!profileMgr.loadProfile(slot.profileName, testProfile)) {
                Serial.printf("[Settings] WARN: Profile '%s' for %s does not exist - clearing reference\n",
                             slot.profileName.c_str(), slotName);
                slot.profileName = "";
                needsSave = true;
            } else {
                Serial.printf("[Settings] Profile '%s' for %s validated OK\n",
                             slot.profileName.c_str(), slotName);
            }
        }
    };

    validateSlot(settings_.slot0_default, "Slot 0 (Default)");
    validateSlot(settings_.slot1_highway, "Slot 1 (Highway)");
    validateSlot(settings_.slot2_comfort, "Slot 2 (Comfort)");

    if (needsSave) {
        if (persistSettingsAtomically()) {
            bumpBackupRevision();
            Serial.println("[Settings] Cleared invalid profile references and saved");
        } else {
            Serial.println("[Settings] ERROR: Failed to persist cleared profile references");
        }
    }

    // No additional side effects needed beyond clearing invalid references.
}

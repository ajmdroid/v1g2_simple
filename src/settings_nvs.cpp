/**
 * Settings NVS persistence layer, crypto/obfuscation, and WiFi client credentials.
 * Extracted from settings_.cpp to reduce file size.
 */

#include "settings_internals.h"

// --- NVS recovery, crypto, WiFi SD secret helpers ---

// NVS recovery: clear unused namespace when NVS is full
// Returns true if space was freed
bool attemptNvsRecovery(const char* activeNs) {
    Serial.println("[Settings] NVS space low - attempting recovery...");

    // Clear the inactive settings namespace to free space
    const char* inactiveNs = nullptr;
    if (strcmp(activeNs, SETTINGS_NS_A) == 0) {
        inactiveNs = SETTINGS_NS_B;
    } else if (strcmp(activeNs, SETTINGS_NS_B) == 0) {
        inactiveNs = SETTINGS_NS_A;
    }

    if (inactiveNs) {
        Preferences prefs;
        if (prefs.begin(inactiveNs, false)) {
            prefs.clear();
            prefs.end();
            Serial.printf("[Settings] Cleared inactive namespace %s\n", inactiveNs);
        }
    }

    return true;
}

// Obfuscate a string using XOR (same function for encode/decode)
String xorObfuscate(const String& input) {
    if (input.length() == 0) return input;

    String output;
    output.reserve(input.length());
    size_t keyLen = strlen(XOR_KEY);

    for (size_t i = 0; i < input.length(); i++) {
        output += (char)(input[i] ^ XOR_KEY[i % keyLen]);
    }
    return output;
}

char hexDigit(uint8_t nibble) {
    nibble &= 0x0F;
    return (nibble < 10) ? static_cast<char>('0' + nibble)
                         : static_cast<char>('A' + (nibble - 10));
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

String bytesToHex(const String& input) {
    if (input.length() == 0) return "";
    String out;
    out.reserve(input.length() * 2);
    for (size_t i = 0; i < input.length(); ++i) {
        uint8_t b = static_cast<uint8_t>(input[i]);
        out += hexDigit(b >> 4);
        out += hexDigit(b);
    }
    return out;
}

bool hexToBytes(const String& input, String& out) {
    if ((input.length() % 2) != 0) return false;
    out = "";
    out.reserve(input.length() / 2);
    for (size_t i = 0; i < input.length(); i += 2) {
        int hi = hexNibble(input[i]);
        int lo = hexNibble(input[i + 1]);
        if (hi < 0 || lo < 0) return false;
        char decoded = static_cast<char>((hi << 4) | lo);
        out += decoded;
    }
    return true;
}

String encodeObfuscatedForStorage(const String& plainText) {
    if (plainText.length() == 0) return "";
    String obfuscated = xorObfuscate(plainText);
    String encoded = OBFUSCATION_HEX_PREFIX;
    encoded += bytesToHex(obfuscated);
    return encoded;
}

String decodeObfuscatedFromStorage(const String& stored) {
    if (stored.length() == 0) return "";

    if (stored.startsWith(OBFUSCATION_HEX_PREFIX)) {
        String hexPayload = stored.substring(strlen(OBFUSCATION_HEX_PREFIX));
        String obfuscated;
        if (!hexToBytes(hexPayload, obfuscated)) {
            Serial.println("[Settings] WARN: Invalid obfuscated hex payload");
            return "";
        }
        return xorObfuscate(obfuscated);
    }

    // Legacy format: raw XOR bytes stored directly as a String.
    return xorObfuscate(stored);
}

bool saveWifiClientSecretToSD(const String& ssid, const String& encodedPassword) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        Serial.println("[Settings] WARN: Failed to acquire SD mutex for WiFi secret save");
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return false;
    }

    JsonDocument doc;
    doc["_type"] = WIFI_CLIENT_SD_SECRET_TYPE;
    doc["_version"] = WIFI_CLIENT_SD_SECRET_VERSION;
    doc["ssid"] = ssid;
    doc["password_obf"] = encodedPassword;
    doc["timestamp"] = millis();

    File file = fs->open(WIFI_CLIENT_SD_SECRET_PATH, FILE_WRITE);
    if (!file) {
        Serial.println("[Settings] WARN: Failed to open SD WiFi secret file for write");
        return false;
    }

    serializeJson(doc, file);
    file.flush();
    file.close();
    return true;
}

String loadWifiClientSecretFromSD(const String& expectedSsid) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return "";
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return "";
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
        return "";
    }

    File file = fs->open(WIFI_CLIENT_SD_SECRET_PATH, FILE_READ);
    if (!file) {
        return "";
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
        Serial.printf("[Settings] WARN: Failed to parse SD WiFi secret: %s\n", err.c_str());
        return "";
    }

    const char* type = doc["_type"] | "";
    if (strcmp(type, WIFI_CLIENT_SD_SECRET_TYPE) != 0) {
        return "";
    }

    String savedSsid = doc["ssid"] | "";
    if (expectedSsid.length() > 0 && savedSsid.length() > 0 && savedSsid != expectedSsid) {
        Serial.printf("[Settings] WARN: SD WiFi secret SSID mismatch (want='%s' got='%s')\n",
                      expectedSsid.c_str(),
                      savedSsid.c_str());
        return "";
    }

    return doc["password_obf"] | "";
}

void clearWifiClientSecretFromSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return;
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return;
    }

    if (fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
        fs->remove(WIFI_CLIENT_SD_SECRET_PATH);
    }
}

int namespaceHealthScore(const char* ns) {
    if (!ns || ns[0] == '\0') {
        return -1;
    }

    Preferences prefs;
    if (!prefs.begin(ns, true)) {
        return -1;
    }

    const int nvsMarker = prefs.getInt(kNvsValid, 0);
    const int settingsVer = prefs.getInt(kNvsSettingsVer, 0);
    int score = 0;

    // Validity marker is the strongest signal that a namespace is current.
    if (nvsMarker > 0) score += 1000;
    if (settingsVer > 0) score += settingsVer * 10;

    static constexpr const char* kCriticalKeys[] = {
        kNvsProxyBle,
        kNvsProxyName,
        kNvsBrightness,
        kNvsDispStyle,
        kNvsAutoPush
    };
    for (const char* key : kCriticalKeys) {
        if (prefs.isKey(key)) {
            score += 5;
        }
    }

    prefs.end();
    return score;
}

bool isKnownSettingsNamespace(const String& ns) {
    return ns == SETTINGS_NS_A || ns == SETTINGS_NS_B || ns == SETTINGS_NS_LEGACY;
}

String SettingsManager::getActiveNamespace() {
    String active = "";
    Preferences meta;
    if (meta.begin(SETTINGS_NS_META, true)) {
        active = meta.getString(kNvsMetaActive, "");
        meta.end();
        if (active.length() > 0 && isKnownSettingsNamespace(active)) {
            // Verify the meta-pointed namespace is actually healthy.
            // If a crash interrupted writeSettingsToNamespace (which clears
            // then rewrites), the namespace could be partial/empty while
            // the OTHER namespace still holds the previous good copy.
            const int activeScore = namespaceHealthScore(active.c_str());
            if (activeScore >= 1000) {
                // nvsValid marker present → write completed fully.
                return active;
            }
            // Meta points to an unhealthy namespace — fall through to
            // health-scoring so we pick the best surviving copy.
            Serial.printf("[Settings] WARN: Meta namespace '%s' unhealthy (score=%d), recovering\n",
                          active.c_str(), activeScore);
        }
    }

    // Meta missing/corrupt: recover by selecting the healthiest settings namespace.
    const int scoreA = namespaceHealthScore(SETTINGS_NS_A);
    const int scoreB = namespaceHealthScore(SETTINGS_NS_B);
    const int scoreLegacy = namespaceHealthScore(SETTINGS_NS_LEGACY);

    String recovered = SETTINGS_NS_LEGACY;
    int bestScore = scoreLegacy;
    if (scoreA > bestScore) {
        recovered = SETTINGS_NS_A;
        bestScore = scoreA;
    }
    if (scoreB > bestScore) {
        recovered = SETTINGS_NS_B;
        bestScore = scoreB;
    }

    if (!isKnownSettingsNamespace(active) && active.length() > 0) {
        Serial.printf("[Settings] WARN: Unknown active namespace '%s', recovering\n", active.c_str());
    }

    if ((recovered == SETTINGS_NS_A || recovered == SETTINGS_NS_B) && recovered != active) {
        Preferences repairMeta;
        if (repairMeta.begin(SETTINGS_NS_META, false)) {
            if (repairMeta.putString(kNvsMetaActive, recovered) > 0) {
                Serial.printf("[Settings] Recovered active namespace to %s\n", recovered.c_str());
            }
            repairMeta.end();
        }
    }

    return recovered;
}

String SettingsManager::getStagingNamespace(const String& activeNamespace) {
    if (activeNamespace == SETTINGS_NS_A) return String(SETTINGS_NS_B);
    if (activeNamespace == SETTINGS_NS_B) return String(SETTINGS_NS_A);
    return String(SETTINGS_NS_A);
}

bool SettingsManager::writeSettingsToNamespace(const char* ns) {
    Preferences prefs;
    if (!prefs.begin(ns, false)) {
        Serial.printf("[Settings] ERROR: Failed to open namespace %s for writing\n", ns);
        return false;
    }

    // Clear old keys in this namespace to avoid stale data from previous versions
    prefs.clear();
    size_t written = 0;
    // Store settings version for migration handling
    written += prefs.putInt(kNvsSettingsVer, SETTINGS_VERSION);
    written += prefs.putBool(kNvsEnableWifi, settings_.enableWifi);
    written += prefs.putInt(kNvsWifiMode, settings_.wifiMode);
    written += prefs.putString(kNvsApSsid, settings_.apSSID);
    // Obfuscate passwords before storing
    written += prefs.putString(kNvsApPassword, encodeObfuscatedForStorage(settings_.apPassword));
    // WiFi client (STA) settings - password stored in separate secure namespace
    written += prefs.putBool(kNvsWifiClientEnabled, settings_.wifiClientEnabled);
    written += prefs.putString(kNvsWifiClientSsid, settings_.wifiClientSSID);
    written += prefs.putBool(kNvsProxyBle, settings_.proxyBLE);
    written += prefs.putString(kNvsProxyName, settings_.proxyName);
    written += prefs.putBool(kNvsDisplayOff, settings_.turnOffDisplay);
    written += prefs.putUChar(kNvsBrightness, settings_.brightness);
    written += prefs.putInt(kNvsDispStyle, settings_.displayStyle);
    written += prefs.putUShort(kNvsColorBogey, settings_.colorBogey);
    written += prefs.putUShort(kNvsColorFreq, settings_.colorFrequency);
    written += prefs.putUShort(kNvsColorArrowFront, settings_.colorArrowFront);
    written += prefs.putUShort(kNvsColorArrowSide, settings_.colorArrowSide);
    written += prefs.putUShort(kNvsColorArrowRear, settings_.colorArrowRear);
    written += prefs.putUShort(kNvsColorBandLaser, settings_.colorBandL);
    written += prefs.putUShort(kNvsColorBandKa, settings_.colorBandKa);
    written += prefs.putUShort(kNvsColorBandK, settings_.colorBandK);
    written += prefs.putUShort(kNvsColorBandX, settings_.colorBandX);
    written += prefs.putUShort(kNvsColorBandPhoto, settings_.colorBandPhoto);
    written += prefs.putUShort(kNvsColorWifi, settings_.colorWiFiIcon);
    written += prefs.putUShort(kNvsColorWifiConnected, settings_.colorWiFiConnected);
    written += prefs.putUShort(kNvsColorBleConnected, settings_.colorBleConnected);
    written += prefs.putUShort(kNvsColorBleDisconnected, settings_.colorBleDisconnected);
    written += prefs.putUShort(kNvsColorBar1, settings_.colorBar1);
    written += prefs.putUShort(kNvsColorBar2, settings_.colorBar2);
    written += prefs.putUShort(kNvsColorBar3, settings_.colorBar3);
    written += prefs.putUShort(kNvsColorBar4, settings_.colorBar4);
    written += prefs.putUShort(kNvsColorBar5, settings_.colorBar5);
    written += prefs.putUShort(kNvsColorBar6, settings_.colorBar6);
    written += prefs.putUShort(kNvsColorMuted, settings_.colorMuted);
    written += prefs.putUShort(kNvsColorPersisted, settings_.colorPersisted);
    written += prefs.putUShort(kNvsColorVolumeMain, settings_.colorVolumeMain);
    written += prefs.putUShort(kNvsColorVolumeMute, settings_.colorVolumeMute);
    written += prefs.putUShort(kNvsColorRssiV1, settings_.colorRssiV1);
    written += prefs.putUShort(kNvsColorRssiProxy, settings_.colorRssiProxy);
    written += prefs.putUShort(kNvsColorObd, settings_.colorObd);
    written += prefs.putUShort(kNvsColorAlpConn, settings_.colorAlpConnected);
    written += prefs.putUShort(kNvsColorAlpScan, settings_.colorAlpScan);
    written += prefs.putUShort(kNvsColorAlpArmed, settings_.colorAlpArmed);
    written += prefs.putBool(kNvsFreqBandColor, settings_.freqUseBandColor);
    written += prefs.putBool(kNvsHideWifi, settings_.hideWifiIcon);
    written += prefs.putBool(kNvsHideProfile, settings_.hideProfileIndicator);
    written += prefs.putBool(kNvsHideBattery, settings_.hideBatteryIcon);
    written += prefs.putBool(kNvsBatteryPercent, settings_.showBatteryPercent);
    written += prefs.putBool(kNvsHideBle, settings_.hideBleIcon);
    written += prefs.putBool(kNvsHideVolume, settings_.hideVolumeIndicator);
    written += prefs.putBool(kNvsHideRssi, settings_.hideRssiIndicator);
    written += prefs.putBool(kNvsWifiAtBoot, settings_.enableWifiAtBoot);
    written += prefs.putUChar(kNvsVoiceMode, (uint8_t)settings_.voiceAlertMode);
    written += prefs.putBool(kNvsVoiceDirection, settings_.voiceDirectionEnabled);
    written += prefs.putBool(kNvsVoiceBogeys, settings_.announceBogeyCount);
    written += prefs.putBool(kNvsMuteVoiceAtVol0, settings_.muteVoiceIfVolZero);
    written += prefs.putUChar(kNvsVoiceVolume, settings_.voiceVolume);
    written += prefs.putBool(kNvsSecondaryAlerts, settings_.announceSecondaryAlerts);
    written += prefs.putBool(kNvsSecondaryLaser, settings_.secondaryLaser);
    written += prefs.putBool(kNvsSecondaryKa, settings_.secondaryKa);
    written += prefs.putBool(kNvsSecondaryK, settings_.secondaryK);
    written += prefs.putBool(kNvsSecondaryX, settings_.secondaryX);
    written += prefs.putBool(kNvsVolFadeEnabled, settings_.alertVolumeFadeEnabled);
    written += prefs.putUChar(kNvsVolFadeSeconds, settings_.alertVolumeFadeDelaySec);
    written += prefs.putUChar(kNvsVolFadeVolume, settings_.alertVolumeFadeVolume);
    written += prefs.putBool(kNvsSpeedMuteEnabled, settings_.speedMuteEnabled);
    written += prefs.putUChar(kNvsSpeedMuteThreshold, settings_.speedMuteThresholdMph);
    written += prefs.putUChar(kNvsSpeedMuteHysteresis, settings_.speedMuteHysteresisMph);
    written += prefs.putUChar(kNvsSpeedMuteVolume, settings_.speedMuteVolume);
    written += prefs.putBool(kNvsSpeedMuteVoice, settings_.speedMuteVoice);
    written += prefs.putBool(kNvsAutoPush, settings_.autoPushEnabled);
    written += prefs.putInt(kNvsActiveSlot, settings_.activeSlot);
    written += prefs.putString(kNvsSlot0Name, settings_.slot0Name);
    written += prefs.putString(kNvsSlot1Name, settings_.slot1Name);
    written += prefs.putString(kNvsSlot2Name, settings_.slot2Name);
    written += prefs.putUShort(kNvsSlot0Color, settings_.slot0Color);
    written += prefs.putUShort(kNvsSlot1Color, settings_.slot1Color);
    written += prefs.putUShort(kNvsSlot2Color, settings_.slot2Color);
    written += prefs.putUChar(kNvsSlot0Volume, settings_.slot0Volume);
    written += prefs.putUChar(kNvsSlot1Volume, settings_.slot1Volume);
    written += prefs.putUChar(kNvsSlot2Volume, settings_.slot2Volume);
    written += prefs.putUChar(kNvsSlot0MuteVolume, settings_.slot0MuteVolume);
    written += prefs.putUChar(kNvsSlot1MuteVolume, settings_.slot1MuteVolume);
    written += prefs.putUChar(kNvsSlot2MuteVolume, settings_.slot2MuteVolume);
    written += prefs.putBool(kNvsSlot0DarkMode, settings_.slot0DarkMode);
    written += prefs.putBool(kNvsSlot1DarkMode, settings_.slot1DarkMode);
    written += prefs.putBool(kNvsSlot2DarkMode, settings_.slot2DarkMode);
    written += prefs.putBool(kNvsSlot0MuteToZero, settings_.slot0MuteToZero);
    written += prefs.putBool(kNvsSlot1MuteToZero, settings_.slot1MuteToZero);
    written += prefs.putBool(kNvsSlot2MuteToZero, settings_.slot2MuteToZero);
    written += prefs.putUChar(kNvsSlot0Persistence, settings_.slot0AlertPersist);
    written += prefs.putUChar(kNvsSlot1Persistence, settings_.slot1AlertPersist);
    written += prefs.putUChar(kNvsSlot2Persistence, settings_.slot2AlertPersist);
    written += prefs.putBool(kNvsSlot0PriorityArrow, settings_.slot0PriorityArrow);
    written += prefs.putBool(kNvsSlot1PriorityArrow, settings_.slot1PriorityArrow);
    written += prefs.putBool(kNvsSlot2PriorityArrow, settings_.slot2PriorityArrow);
    written += prefs.putString(kNvsSlot0Profile, settings_.slot0_default.profileName);
    written += prefs.putInt(kNvsSlot0Mode, settings_.slot0_default.mode);
    written += prefs.putString(kNvsSlot1Profile, settings_.slot1_highway.profileName);
    written += prefs.putInt(kNvsSlot1Mode, settings_.slot1_highway.mode);
    written += prefs.putString(kNvsSlot2Profile, settings_.slot2_comfort.profileName);
    written += prefs.putInt(kNvsSlot2Mode, settings_.slot2_comfort.mode);
    written += prefs.putString(kNvsLastV1Address, settings_.lastV1Address);
    written += prefs.putUChar(kNvsAutoPowerOff, settings_.autoPowerOffMinutes);
    written += prefs.putUChar(kNvsApTimeout, settings_.apTimeoutMinutes);

    // OBD settings
    written += prefs.putBool(kNvsObdEnabled, settings_.obdEnabled);
    written += prefs.putString(kNvsObdAddress, settings_.obdSavedAddress);
    written += prefs.putString(kNvsObdName, settings_.obdSavedName);
    written += prefs.putUChar(kNvsObdAddressType, settings_.obdSavedAddrType);
    written += prefs.putChar(kNvsObdMinRssi, settings_.obdMinRssi);

    // ALP settings
    written += prefs.putBool(kNvsAlpEnabled, settings_.alpEnabled);
    written += prefs.putBool(kNvsAlpSdLog, settings_.alpSdLogEnabled);

    // Debug / diagnostics
    written += prefs.putBool(kNvsPowerOffSdLog, settings_.powerOffSdLog);

    // NVS validity marker - used to detect if NVS was wiped.
    // Written LAST so its presence proves the entire write completed.
    written += prefs.putInt(kNvsValid, SETTINGS_VERSION);

    // Verify the marker was actually persisted.  If NVS ran out of
    // entries/pages, later keys silently fail and the namespace would
    // appear incomplete on the next boot.
    const int verifyMarker = prefs.getInt(kNvsValid, 0);
    prefs.end();

    if (verifyMarker != SETTINGS_VERSION) {
        Serial.printf("[Settings] ERROR: nvsValid verify failed in %s (expected %d, got %d) — written=%d\n",
                      ns, SETTINGS_VERSION, verifyMarker, (int)written);
        return false;
    }

    Serial.printf("[Settings] Wrote %d bytes to namespace %s\n", (int)written, ns);
    return true;
}

bool SettingsManager::persistSettingsAtomically() {
    String activeNs = getActiveNamespace();
    String stagingNs = getStagingNamespace(activeNs);

    if (!writeSettingsToNamespace(stagingNs.c_str())) {
        // First attempt failed - try NVS recovery and retry once
        Serial.println("[Settings] First write attempt failed, trying NVS recovery...");
        attemptNvsRecovery(activeNs.c_str());

        if (!writeSettingsToNamespace(stagingNs.c_str())) {
            Serial.println("[Settings] ERROR: Failed to write staging settings_ even after recovery");
            return false;
        }
    }

    Preferences meta;
    if (!meta.begin(SETTINGS_NS_META, false)) {
        Serial.println("[Settings] ERROR: Failed to open settings_ meta namespace");
        Serial.printf("[Settings] WARN: Falling back to in-place write on %s\n", activeNs.c_str());
        if (!writeSettingsToNamespace(activeNs.c_str())) {
            Serial.println("[Settings] ERROR: In-place fallback write failed");
            return false;
        }
        Serial.printf("[Settings] Fallback write succeeded in %s\n", activeNs.c_str());
        return true;
    }

    bool committed = meta.putString(kNvsMetaActive, stagingNs) > 0;
    meta.end();

    if (!committed) {
        Serial.println("[Settings] ERROR: Failed to update active settings_ namespace");
        Serial.printf("[Settings] WARN: Falling back to in-place write on %s\n", activeNs.c_str());
        if (!writeSettingsToNamespace(activeNs.c_str())) {
            Serial.println("[Settings] ERROR: In-place fallback write failed");
            return false;
        }
        Serial.printf("[Settings] Fallback write succeeded in %s\n", activeNs.c_str());
        return true;
    }

    Serial.printf("[Settings] Active namespace advanced from %s to %s\n", activeNs.c_str(), stagingNs.c_str());
    return true;
}

// --- WiFi client credential methods ---

String SettingsManager::getWifiClientPassword() {
    Preferences prefs;
    bool hasNvsKey = false;
    String storedPwd;
    if (!prefs.begin(WIFI_CLIENT_NS, true)) {  // Read-only
        storedPwd = "";
    } else {
        hasNvsKey = prefs.isKey(kNvsWifiPassword);
        if (hasNvsKey) {
            storedPwd = prefs.getString(kNvsWifiPassword, "");
        }
        prefs.end();
    }

    // Open-network credential: key present with empty value is valid.
    if (hasNvsKey && storedPwd.length() == 0) {
        return "";
    }

    if (storedPwd.length() > 0) {
        // Password is stored as obfuscated hex payload (legacy raw XOR still supported).
        String decoded = decodeObfuscatedFromStorage(storedPwd);
        if (decoded.length() == 0) {
            Serial.println("[Settings] WARN: WiFi password decode returned empty for non-empty stored value — possible NVS corruption");
        }
        return decoded;
    }

    // Fallback: recover password from SD-backed secret store if available.
    String sdEncoded = loadWifiClientSecretFromSD(settings_.wifiClientSSID);
    if (sdEncoded.length() == 0) {
        return "";
    }

    String decoded = decodeObfuscatedFromStorage(sdEncoded);
    if (decoded.length() == 0) {
        return "";
    }

    // Heal NVS from SD fallback so future reconnects do not hit SD.
    Preferences healPrefs;
    if (healPrefs.begin(WIFI_CLIENT_NS, false)) {
        healPrefs.putString(kNvsWifiPassword, sdEncoded);
        healPrefs.end();
        Serial.println("[Settings] Recovered WiFi client password from SD secret");
    }

    return decoded;
}

void SettingsManager::setWifiClientEnabled(bool enabled) {
    settings_.wifiClientEnabled = enabled;
    settings_.wifiMode = enabled ? V1_WIFI_APSTA : V1_WIFI_AP;
    save();
}

void SettingsManager::setWifiClientCredentials(const String& ssid, const String& password) {
    settings_.wifiClientSSID = sanitizeWifiClientSsidValue(ssid);
    settings_.wifiClientEnabled = settings_.wifiClientSSID.length() > 0;
    settings_.wifiMode = settings_.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;

    const String encodedPassword = encodeObfuscatedForStorage(password);
    bool nvsSaved = false;

    // Store password in separate namespace with obfuscation
    Preferences prefs;
    if (prefs.begin(WIFI_CLIENT_NS, false)) {  // Read-write
        size_t written = 0;
        if (password.length() == 0) {
            // Open network: no password required.
            prefs.remove(kNvsWifiPassword);
            nvsSaved = true;
        } else {
            written = prefs.putString(kNvsWifiPassword, encodedPassword);
            nvsSaved = written > 0;
        }
        prefs.end();

        if (nvsSaved) {
            Serial.println("[Settings] WiFi client credentials saved");
        } else {
            // NVS might be full - try recovery and retry
            Serial.println("[Settings] WiFi password save failed, trying NVS recovery...");
            String activeNs = getActiveNamespace();
            attemptNvsRecovery(activeNs.c_str());

            // Retry save
            if (prefs.begin(WIFI_CLIENT_NS, false)) {
                if (password.length() == 0) {
                    prefs.remove(kNvsWifiPassword);
                    nvsSaved = true;
                } else {
                    written = prefs.putString(kNvsWifiPassword, encodedPassword);
                    nvsSaved = written > 0;
                }
                prefs.end();
                if (nvsSaved) {
                    Serial.println("[Settings] WiFi client credentials saved after recovery");
                } else {
                    Serial.println("[Settings] ERROR: WiFi password save failed even after recovery");
                }
            }
        }
    } else {
        Serial.println("[Settings] ERROR: Failed to open WiFi client namespace");
    }

    // Redundant SD copy for recovery when NVS gets wiped/corrupted.
    if (settings_.wifiClientSSID.length() > 0 && saveWifiClientSecretToSD(settings_.wifiClientSSID, encodedPassword)) {
        Serial.println("[Settings] WiFi client secret mirrored to SD");
    }

    save();
}

void SettingsManager::clearWifiClientCredentials() {
    settings_.wifiClientSSID = "";
    settings_.wifiClientEnabled = false;
    settings_.wifiMode = V1_WIFI_AP;

    // Clear the password from secure namespace
    Preferences prefs;
    if (prefs.begin(WIFI_CLIENT_NS, false)) {
        prefs.clear();
        prefs.end();
        Serial.println("[Settings] WiFi client credentials cleared");
    }

    clearWifiClientSecretFromSD();

    save();
}

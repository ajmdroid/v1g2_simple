/**
 * Settings NVS persistence layer, crypto/obfuscation, and WiFi client credentials.
 * Extracted from settings.cpp to reduce file size.
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
    
    // Also clear legacy namespace if it exists
    Preferences legacy;
    if (legacy.begin(SETTINGS_NS_LEGACY, false)) {
        legacy.clear();
        legacy.end();
        Serial.println("[Settings] Cleared legacy namespace");
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

    const int nvsMarker = prefs.getInt("nvsValid", 0);
    const int settingsVer = prefs.getInt("settingsVer", 0);
    int score = 0;

    // Validity marker is the strongest signal that a namespace is current.
    if (nvsMarker > 0) score += 1000;
    if (settingsVer > 0) score += settingsVer * 10;

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
        active = meta.getString("active", "");
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
            if (repairMeta.putString("active", recovered) > 0) {
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
    written += prefs.putInt("settingsVer", SETTINGS_VERSION);
    written += prefs.putBool("enableWifi", settings.enableWifi);
    written += prefs.putInt("wifiMode", settings.wifiMode);
    written += prefs.putString("apSSID", settings.apSSID);
    // Obfuscate passwords before storing
    written += prefs.putString("apPassword", encodeObfuscatedForStorage(settings.apPassword));
    // WiFi client (STA) settings - password stored in separate secure namespace
    written += prefs.putBool("wifiClientEn", settings.wifiClientEnabled);
    written += prefs.putString("wifiClSSID", settings.wifiClientSSID);
    written += prefs.putBool("proxyBLE", settings.proxyBLE);
    written += prefs.putString("proxyName", settings.proxyName);
    written += prefs.putBool("obdEn", settings.obdEnabled);
    written += prefs.putBool("obdVwData", settings.obdVwDataEnabled);
    written += prefs.putBool("gpsEn", settings.gpsEnabled);
    written += prefs.putBool("camEn", settings.cameraEnabled);
    written += prefs.putUShort("camAlertFt", settings.cameraAlertDistanceFt);
    written += prefs.putUChar("camAlertSec", settings.cameraAlertPersistSec);
    written += prefs.putUChar("gpsLkMode", static_cast<uint8_t>(settings.gpsLockoutMode));
    written += prefs.putBool("gpsLkGuard", settings.gpsLockoutCoreGuardEnabled);
    written += prefs.putUShort("gpsLkQDrop", settings.gpsLockoutMaxQueueDrops);
    written += prefs.putUShort("gpsLkPDrop", settings.gpsLockoutMaxPerfDrops);
    written += prefs.putUShort("gpsLkEBDrop", settings.gpsLockoutMaxEventBusDrops);
    written += prefs.putUChar("gpsLkHits", settings.gpsLockoutLearnerPromotionHits);
    written += prefs.putUShort("gpsLkRad", settings.gpsLockoutLearnerRadiusE5);
    written += prefs.putUShort("gpsLkFtol", settings.gpsLockoutLearnerFreqToleranceMHz);
    written += prefs.putUChar("gpsLkLInt", settings.gpsLockoutLearnerLearnIntervalHours);
    written += prefs.putUChar("gpsLkUInt", settings.gpsLockoutLearnerUnlearnIntervalHours);
    written += prefs.putUChar("gpsLkUCnt", settings.gpsLockoutLearnerUnlearnCount);
    written += prefs.putUChar("gpsLkMDCnt", settings.gpsLockoutManualDemotionMissCount);
    written += prefs.putBool("gpsLkKa", settings.gpsLockoutKaLearningEnabled);
    written += prefs.putBool("gpsLkPQ", settings.gpsLockoutPreQuiet);
    written += prefs.putBool("displayOff", settings.turnOffDisplay);
    written += prefs.putUChar("brightness", settings.brightness);
    written += prefs.putInt("dispStyle", settings.displayStyle);
    written += prefs.putUShort("colorBogey", settings.colorBogey);
    written += prefs.putUShort("colorFreq", settings.colorFrequency);
    written += prefs.putUShort("colorArrF", settings.colorArrowFront);
    written += prefs.putUShort("colorArrS", settings.colorArrowSide);
    written += prefs.putUShort("colorArrR", settings.colorArrowRear);
    written += prefs.putUShort("colorBandL", settings.colorBandL);
    written += prefs.putUShort("colorBandKa", settings.colorBandKa);
    written += prefs.putUShort("colorBandK", settings.colorBandK);
    written += prefs.putUShort("colorBandX", settings.colorBandX);
    written += prefs.putUShort("colorBandP", settings.colorBandPhoto);
    written += prefs.putUShort("colorWiFi", settings.colorWiFiIcon);
    written += prefs.putUShort("colorWiFiC", settings.colorWiFiConnected);
    written += prefs.putUShort("colorBleC", settings.colorBleConnected);
    written += prefs.putUShort("colorBleD", settings.colorBleDisconnected);
    written += prefs.putUShort("colorBar1", settings.colorBar1);
    written += prefs.putUShort("colorBar2", settings.colorBar2);
    written += prefs.putUShort("colorBar3", settings.colorBar3);
    written += prefs.putUShort("colorBar4", settings.colorBar4);
    written += prefs.putUShort("colorBar5", settings.colorBar5);
    written += prefs.putUShort("colorBar6", settings.colorBar6);
    written += prefs.putUShort("colorMuted", settings.colorMuted);
    written += prefs.putUShort("colorPersist", settings.colorPersisted);
    written += prefs.putUShort("colorVolMain", settings.colorVolumeMain);
    written += prefs.putUShort("colorVolMute", settings.colorVolumeMute);
    written += prefs.putUShort("colorRssiV1", settings.colorRssiV1);
    written += prefs.putUShort("colorRssiPrx", settings.colorRssiProxy);
    written += prefs.putUShort("colorCamT", settings.colorCameraToken);
    written += prefs.putUShort("colorCamA", settings.colorCameraArrow);
    written += prefs.putUShort("colorLockL", settings.colorLockout);
    written += prefs.putUShort("colorGps", settings.colorGps);
    written += prefs.putUShort("colorObd", settings.colorObd);
    written += prefs.putBool("freqBandCol", settings.freqUseBandColor);
    written += prefs.putBool("hideWifi", settings.hideWifiIcon);
    written += prefs.putBool("hideProfile", settings.hideProfileIndicator);
    written += prefs.putBool("hideBatt", settings.hideBatteryIcon);
    written += prefs.putBool("battPct", settings.showBatteryPercent);
    written += prefs.putBool("hideBle", settings.hideBleIcon);
    written += prefs.putBool("hideVol", settings.hideVolumeIndicator);
    written += prefs.putBool("hideRssi", settings.hideRssiIndicator);
    written += prefs.putBool("restTelem", settings.showRestTelemetryCards);
    written += prefs.putBool("wifiAtBoot", settings.enableWifiAtBoot);
    written += prefs.putBool("sigTraceLog", settings.enableSignalTraceLogging);
    written += prefs.putUChar("voiceMode", (uint8_t)settings.voiceAlertMode);
    written += prefs.putBool("voiceDir", settings.voiceDirectionEnabled);
    written += prefs.putBool("voiceBogeys", settings.announceBogeyCount);
    written += prefs.putBool("muteVoiceVol0", settings.muteVoiceIfVolZero);
    written += prefs.putUChar("voiceVol", settings.voiceVolume);
    written += prefs.putBool("secAlerts", settings.announceSecondaryAlerts);
    written += prefs.putBool("secLaser", settings.secondaryLaser);
    written += prefs.putBool("secKa", settings.secondaryKa);
    written += prefs.putBool("secK", settings.secondaryK);
    written += prefs.putBool("secX", settings.secondaryX);
    written += prefs.putBool("volFadeEn", settings.alertVolumeFadeEnabled);
    written += prefs.putUChar("volFadeSec", settings.alertVolumeFadeDelaySec);
    written += prefs.putUChar("volFadeVol", settings.alertVolumeFadeVolume);
    written += prefs.putBool("spdVolEn", settings.speedVolumeEnabled);
    written += prefs.putUChar("spdVolThr", settings.speedVolumeThresholdMph);
    written += prefs.putUChar("spdVolBoost", settings.speedVolumeBoost);
    written += prefs.putBool("lowSpdMute", settings.lowSpeedMuteEnabled);
    written += prefs.putUChar("lowSpdThr", settings.lowSpeedMuteThresholdMph);
    written += prefs.putUChar("lowSpdVol", settings.lowSpeedVolume);
    written += prefs.putBool("autoPush", settings.autoPushEnabled);
    written += prefs.putInt("activeSlot", settings.activeSlot);
    written += prefs.putString("slot0name", settings.slot0Name);
    written += prefs.putString("slot1name", settings.slot1Name);
    written += prefs.putString("slot2name", settings.slot2Name);
    written += prefs.putUShort("slot0color", settings.slot0Color);
    written += prefs.putUShort("slot1color", settings.slot1Color);
    written += prefs.putUShort("slot2color", settings.slot2Color);
    written += prefs.putUChar("slot0vol", settings.slot0Volume);
    written += prefs.putUChar("slot1vol", settings.slot1Volume);
    written += prefs.putUChar("slot2vol", settings.slot2Volume);
    written += prefs.putUChar("slot0mute", settings.slot0MuteVolume);
    written += prefs.putUChar("slot1mute", settings.slot1MuteVolume);
    written += prefs.putUChar("slot2mute", settings.slot2MuteVolume);
    written += prefs.putBool("slot0dark", settings.slot0DarkMode);
    written += prefs.putBool("slot1dark", settings.slot1DarkMode);
    written += prefs.putBool("slot2dark", settings.slot2DarkMode);
    written += prefs.putBool("slot0mz", settings.slot0MuteToZero);
    written += prefs.putBool("slot1mz", settings.slot1MuteToZero);
    written += prefs.putBool("slot2mz", settings.slot2MuteToZero);
    written += prefs.putUChar("slot0persist", settings.slot0AlertPersist);
    written += prefs.putUChar("slot1persist", settings.slot1AlertPersist);
    written += prefs.putUChar("slot2persist", settings.slot2AlertPersist);
    written += prefs.putBool("slot0prio", settings.slot0PriorityArrow);
    written += prefs.putBool("slot1prio", settings.slot1PriorityArrow);
    written += prefs.putBool("slot2prio", settings.slot2PriorityArrow);
    written += prefs.putString("slot0prof", settings.slot0_default.profileName);
    written += prefs.putInt("slot0mode", settings.slot0_default.mode);
    written += prefs.putString("slot1prof", settings.slot1_highway.profileName);
    written += prefs.putInt("slot1mode", settings.slot1_highway.mode);
    written += prefs.putString("slot2prof", settings.slot2_comfort.profileName);
    written += prefs.putInt("slot2mode", settings.slot2_comfort.mode);
    written += prefs.putString("lastV1Addr", settings.lastV1Address);
    written += prefs.putUChar("autoPwrOff", settings.autoPowerOffMinutes);
    written += prefs.putUChar("apTimeout", settings.apTimeoutMinutes);
    
    // NVS validity marker - used to detect if NVS was wiped.
    // Written LAST so its presence proves the entire write completed.
    written += prefs.putInt("nvsValid", SETTINGS_VERSION);

    // Verify the marker was actually persisted.  If NVS ran out of
    // entries/pages, later keys silently fail and the namespace would
    // appear incomplete on the next boot.
    const int verifyMarker = prefs.getInt("nvsValid", 0);
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
            Serial.println("[Settings] ERROR: Failed to write staging settings even after recovery");
            return false;
        }
    }

    Preferences meta;
    if (!meta.begin(SETTINGS_NS_META, false)) {
        Serial.println("[Settings] ERROR: Failed to open settings meta namespace");
        Serial.printf("[Settings] WARN: Falling back to in-place write on %s\n", activeNs.c_str());
        if (!writeSettingsToNamespace(activeNs.c_str())) {
            Serial.println("[Settings] ERROR: In-place fallback write failed");
            return false;
        }
        Serial.printf("[Settings] Fallback write succeeded in %s\n", activeNs.c_str());
        return true;
    }

    bool committed = meta.putString("active", stagingNs) > 0;
    meta.end();

    if (!committed) {
        Serial.println("[Settings] ERROR: Failed to update active settings namespace");
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
        hasNvsKey = prefs.isKey("password");
        if (hasNvsKey) {
            storedPwd = prefs.getString("password", "");
        }
        prefs.end();
    }

    // Open-network credential: key present with empty value is valid.
    if (hasNvsKey && storedPwd.length() == 0) {
        return "";
    }

    if (storedPwd.length() > 0) {
        // Password is stored as obfuscated hex payload (legacy raw XOR still supported).
        return decodeObfuscatedFromStorage(storedPwd);
    }

    // Fallback: recover password from SD-backed secret store if available.
    String sdEncoded = loadWifiClientSecretFromSD(settings.wifiClientSSID);
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
        healPrefs.putString("password", sdEncoded);
        healPrefs.end();
        Serial.println("[Settings] Recovered WiFi client password from SD secret");
    }

    return decoded;
}

void SettingsManager::setWifiClientEnabled(bool enabled) {
    settings.wifiClientEnabled = enabled;
    settings.wifiMode = enabled ? V1_WIFI_APSTA : V1_WIFI_AP;
    save();
}

void SettingsManager::setWifiClientCredentials(const String& ssid, const String& password) {
    settings.wifiClientSSID = sanitizeWifiClientSsidValue(ssid);
    settings.wifiClientEnabled = settings.wifiClientSSID.length() > 0;
    settings.wifiMode = settings.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;

    const String encodedPassword = encodeObfuscatedForStorage(password);
    bool nvsSaved = false;

    // Store password in separate namespace with obfuscation
    Preferences prefs;
    if (prefs.begin(WIFI_CLIENT_NS, false)) {  // Read-write
        size_t written = 0;
        if (password.length() == 0) {
            // Open network: no password required.
            prefs.remove("password");
            nvsSaved = true;
        } else {
            written = prefs.putString("password", encodedPassword);
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
                    prefs.remove("password");
                    nvsSaved = true;
                } else {
                    written = prefs.putString("password", encodedPassword);
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
    if (settings.wifiClientSSID.length() > 0 && saveWifiClientSecretToSD(settings.wifiClientSSID, encodedPassword)) {
        Serial.println("[Settings] WiFi client secret mirrored to SD");
    }

    save();
}

void SettingsManager::clearWifiClientCredentials() {
    settings.wifiClientSSID = "";
    settings.wifiClientEnabled = false;
    settings.wifiMode = V1_WIFI_AP;
    
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

/**
 * Settings storage implementation
 * 
 * SECURITY NOTE: WiFi passwords are stored with XOR obfuscation, NOT encryption.
 * This is intentional - it prevents casual viewing in hex dumps but is NOT secure
 * against a determined attacker with physical access to the device.
 * 
 * For this use case (a car accessory on a private network), the trade-off is:
 * - Pro: Simple, no crypto library overhead, recoverable if key changes
 * - Con: Not suitable for high-security applications
 * 
 * If stronger security is needed, consider ESP32 NVS encryption (requires flash
 * encryption key management) or storing a hash instead of the actual password.
 */

#include "settings_internals.h"

// SD backup file path
const char* SETTINGS_BACKUP_PATH = "/v1simple_backup.json";
const char* SETTINGS_BACKUP_TMP_PATH = "/v1simple_backup.tmp";
const char* SETTINGS_BACKUP_PREV_PATH = "/v1simple_backup.prev";
const int SD_BACKUP_VERSION = 7;  // Increment when adding new fields to backup
const size_t SETTINGS_BACKUP_MAX_BYTES = 512 * 1024;
const char* SETTINGS_NS_A = "v1settingsA";
const char* SETTINGS_NS_B = "v1settingsB";
const char* SETTINGS_NS_META = "v1settingsMeta";
const char* SETTINGS_NS_LEGACY = "v1settings";
const char* WIFI_CLIENT_NS = "v1wificlient";
const char* WIFI_CLIENT_SD_SECRET_PATH = "/v1wifi_secret.json";
const char* WIFI_CLIENT_SD_SECRET_TYPE = "v1wifi_secret";
const int WIFI_CLIENT_SD_SECRET_VERSION = 1;
const char* const SETTINGS_BACKUP_CANDIDATES[] = {
    SETTINGS_BACKUP_PATH,
    SETTINGS_BACKUP_PREV_PATH,
    "/v1simple_settings.json",
    "/v1settings_backup.json"
};
const size_t SETTINGS_BACKUP_CANDIDATES_COUNT = sizeof(SETTINGS_BACKUP_CANDIDATES) / sizeof(SETTINGS_BACKUP_CANDIDATES[0]);

WiFiModeSetting clampWifiModeValue(int raw) {
    int clamped = std::max(static_cast<int>(V1_WIFI_OFF),
                           std::min(raw, static_cast<int>(V1_WIFI_APSTA)));
    return static_cast<WiFiModeSetting>(clamped);
}

VoiceAlertMode clampVoiceAlertModeValue(int raw) {
    int clamped = std::max(static_cast<int>(VOICE_MODE_DISABLED),
                           std::min(raw, static_cast<int>(VOICE_MODE_BAND_FREQ)));
    return static_cast<VoiceAlertMode>(clamped);
}

static constexpr size_t MAX_V1_ADDRESS_LEN = 32;

String sanitizeApPasswordValue(const String& raw) {
    String value = clampStringLength(raw, MAX_AP_PASSWORD_LEN);
    if (value.length() < MIN_AP_PASSWORD_LEN) {
        return "setupv1g2";
    }
    return value;
}

String sanitizeLastV1AddressValue(const String& raw) {
    return clampStringLength(raw, MAX_V1_ADDRESS_LEN);
}


// --- Backup static helpers (isSupportedBackupType through writeBackupAtomically)
//     moved to settings_backup.cpp ---

// Global instance
SettingsManager settingsManager;

// XOR obfuscation key - deters casual reading but NOT cryptographically secure
// See security note above for rationale
const char XOR_KEY[] = "V1G2-S3cr3t-K3y!";
const int SETTINGS_VERSION = 4;  // Increment when changing persisted settings schema
const char* OBFUSCATION_HEX_PREFIX = "hex:";

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
            return active;
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
    
    // NVS validity marker - used to detect if NVS was wiped
    written += prefs.putInt("nvsValid", SETTINGS_VERSION);

    prefs.end();
    Serial.printf("[Settings] Wrote %d bytes to namespace %s\n", written, ns);
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

SettingsManager::SettingsManager() {}

void SettingsManager::begin() {
    // Ensure WiFi client namespace exists so read-only opens do not spam
    // NOT_FOUND on fresh/erased NVS.
    Preferences wifiClientNs;
    if (wifiClientNs.begin(WIFI_CLIENT_NS, false)) {
        wifiClientNs.end();
    } else {
        Serial.println("[Settings] WARN: Failed to initialize WiFi client namespace");
    }

    load();
    
    // Note: SD card may not be mounted yet during begin().
    // checkAndRestoreFromSD() should be called after storage is ready.
    // We still try here in case storage was already initialized.
    checkAndRestoreFromSD();
}


// --- checkAndRestoreFromSD moved to settings_backup.cpp ---

void SettingsManager::load() {
    String activeNs = getActiveNamespace();
    if (!preferences.begin(activeNs.c_str(), true)) {
        Serial.printf("[Settings] WARN: Failed to open namespace %s, falling back to legacy\n", activeNs.c_str());
        activeNs = SETTINGS_NS_LEGACY;
        if (!preferences.begin(activeNs.c_str(), true)) {
            Serial.println("ERROR: Failed to open preferences for reading!");
            return;
        }
    }
    
    // Check settings version for migration
    int storedVersion = preferences.getInt("settingsVer", 1);
    
    settings.enableWifi = preferences.getBool("enableWifi", true);
    
    // Handle AP password storage - version 1 was plain text, version 2+ is obfuscated
    String storedApPwd = preferences.getString("apPassword", "");
    
    if (storedVersion >= 2) {
        // Passwords are obfuscated - decode and sanitize them.
        settings.apPassword = sanitizeApPasswordValue(
            storedApPwd.length() > 0 ? decodeObfuscatedFromStorage(storedApPwd) : "setupv1g2");
    } else {
        // Version 1 - passwords stored in plain text, use as-is then sanitize.
        settings.apPassword = sanitizeApPasswordValue(storedApPwd.length() > 0 ? storedApPwd : "setupv1g2");
        Serial.println("[Settings] Migrating from v1 to v2 (password obfuscation)");
    }
    
    settings.apSSID = sanitizeApSsidValue(preferences.getString("apSSID", "V1-Simple"));
    
    // WiFi client (STA) settings
    settings.wifiClientEnabled = preferences.getBool("wifiClientEn", false);
    settings.wifiClientSSID = sanitizeWifiClientSsidValue(preferences.getString("wifiClSSID", ""));
    // Determine WiFi mode based on client enabled state
    settings.wifiMode = settings.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;
    
    // Debug: Log WiFi client settings on load
    Serial.printf("[Settings] WiFi client: enabled=%s, SSID='%s'\n",
                  settings.wifiClientEnabled ? "true" : "false",
                  settings.wifiClientSSID.c_str());
    
    settings.proxyBLE = preferences.getBool("proxyBLE", true);
    settings.proxyName = sanitizeProxyNameValue(preferences.getString("proxyName", "V1-Proxy"));
    settings.obdEnabled = preferences.getBool("obdEn", true);
    settings.obdVwDataEnabled = preferences.getBool("obdVwData", true);
    settings.gpsEnabled = preferences.getBool("gpsEn", false);
    settings.cameraEnabled = preferences.getBool("camEn", true);
    settings.gpsLockoutMode = clampLockoutRuntimeModeValue(
        preferences.getUChar("gpsLkMode", static_cast<uint8_t>(LOCKOUT_RUNTIME_OFF)));
    settings.gpsLockoutCoreGuardEnabled = preferences.getBool("gpsLkGuard", true);
    settings.gpsLockoutMaxQueueDrops = preferences.getUShort("gpsLkQDrop", 0);
    settings.gpsLockoutMaxPerfDrops = preferences.getUShort("gpsLkPDrop", 0);
    settings.gpsLockoutMaxEventBusDrops = preferences.getUShort("gpsLkEBDrop", 0);
    settings.gpsLockoutLearnerPromotionHits = clampLockoutLearnerHitsValue(
        preferences.getUChar("gpsLkHits", LOCKOUT_LEARNER_HITS_DEFAULT));
    uint16_t learnerRadiusRaw = preferences.getUShort("gpsLkRad", LOCKOUT_LEARNER_RADIUS_E5_DEFAULT);
    // Legacy radius values were written in a 10x scale (450..3600 intended as 50..400 m).
    // Normalize in-memory during load to preserve intended behavior without immediate NVS rewrite.
    if (storedVersion <= 2 && learnerRadiusRaw >= 450) {
        const uint16_t converted = static_cast<uint16_t>((learnerRadiusRaw + 5u) / 10u);
        Serial.printf("[Settings] Migrated legacy lockout radius %u -> %u\n",
                      static_cast<unsigned>(learnerRadiusRaw),
                      static_cast<unsigned>(converted));
        learnerRadiusRaw = converted;
    }
    settings.gpsLockoutLearnerRadiusE5 = clampLockoutLearnerRadiusE5Value(learnerRadiusRaw);
    settings.gpsLockoutLearnerFreqToleranceMHz = clampLockoutLearnerFreqTolValue(
        preferences.getUShort("gpsLkFtol", LOCKOUT_LEARNER_FREQ_TOL_DEFAULT));
    settings.gpsLockoutLearnerLearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
        preferences.getUChar("gpsLkLInt", LOCKOUT_LEARNER_LEARN_INTERVAL_HOURS_DEFAULT));
    settings.gpsLockoutLearnerUnlearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
        preferences.getUChar("gpsLkUInt", LOCKOUT_LEARNER_UNLEARN_INTERVAL_HOURS_DEFAULT));
    settings.gpsLockoutLearnerUnlearnCount = clampLockoutLearnerUnlearnCountValue(
        preferences.getUChar("gpsLkUCnt", LOCKOUT_LEARNER_UNLEARN_COUNT_DEFAULT));
    settings.gpsLockoutManualDemotionMissCount = clampLockoutManualDemotionMissCountValue(
        preferences.getUChar("gpsLkMDCnt", LOCKOUT_MANUAL_DEMOTION_MISS_COUNT_DEFAULT));
    settings.gpsLockoutKaLearningEnabled = preferences.getBool("gpsLkKa", false);
    settings.gpsLockoutPreQuiet = preferences.getBool("gpsLkPQ", false);
    settings.turnOffDisplay = preferences.getBool("displayOff", false);
    settings.brightness = std::max<uint8_t>(1, preferences.getUChar("brightness", 200));  // Min 1 to avoid blank screen
    settings.displayStyle = normalizeDisplayStyle(preferences.getInt("dispStyle", DISPLAY_STYLE_CLASSIC));
    settings.colorBogey = preferences.getUShort("colorBogey", 0xF800);
    settings.colorFrequency = preferences.getUShort("colorFreq", 0xF800);
    settings.colorArrowFront = preferences.getUShort("colorArrF", 0xF800);
    settings.colorArrowSide = preferences.getUShort("colorArrS", 0xF800);
    settings.colorArrowRear = preferences.getUShort("colorArrR", 0xF800);
    settings.colorBandL = preferences.getUShort("colorBandL", 0x001F);
    settings.colorBandKa = preferences.getUShort("colorBandKa", 0xF800);
    settings.colorBandK = preferences.getUShort("colorBandK", 0x001F);
    settings.colorBandX = preferences.getUShort("colorBandX", 0x07E0);
    settings.colorBandPhoto = preferences.getUShort("colorBandP", 0x780F);  // Purple (photo radar)
    settings.colorWiFiIcon = preferences.getUShort("colorWiFi", 0x07FF);
    settings.colorWiFiConnected = preferences.getUShort("colorWiFiC", 0x07E0);
    settings.colorBleConnected = preferences.getUShort("colorBleC", 0x07E0);
    settings.colorBleDisconnected = preferences.getUShort("colorBleD", 0x001F);
    settings.colorBar1 = preferences.getUShort("colorBar1", 0x07E0);
    settings.colorBar2 = preferences.getUShort("colorBar2", 0x07E0);
    settings.colorBar3 = preferences.getUShort("colorBar3", 0xFFE0);
    settings.colorBar4 = preferences.getUShort("colorBar4", 0xFFE0);
    settings.colorBar5 = preferences.getUShort("colorBar5", 0xF800);
    settings.colorBar6 = preferences.getUShort("colorBar6", 0xF800);
    settings.colorMuted = preferences.getUShort("colorMuted", 0x3186);  // Dark grey muted color
    settings.colorPersisted = preferences.getUShort("colorPersist", 0x18C3);  // Darker grey for persisted alerts
    settings.colorVolumeMain = preferences.getUShort("colorVolMain", 0xF800);  // Red for main volume
    settings.colorVolumeMute = preferences.getUShort("colorVolMute", 0x7BEF);  // Grey for mute volume
    settings.colorRssiV1 = preferences.getUShort("colorRssiV1", 0x07E0);       // Green for V1 RSSI label
    settings.colorRssiProxy = preferences.getUShort("colorRssiPrx", 0x001F);   // Blue for Proxy RSSI label
    settings.colorCameraToken = preferences.getUShort("colorCamT", 0xF800);     // Red for camera token text
    settings.colorCameraArrow = preferences.getUShort("colorCamA", 0xF800);     // Red for camera forward arrow
    settings.colorLockout = preferences.getUShort("colorLockL", 0x07E0);        // Green lockout badge color
    settings.colorGps = preferences.getUShort("colorGps", 0x07FF);              // Cyan GPS badge color
    settings.colorObd = preferences.getUShort("colorObd", 0xFD20);              // Orange OBD badge color
    settings.freqUseBandColor = preferences.getBool("freqBandCol", false);  // Use custom freq color by default
    settings.hideWifiIcon = preferences.getBool("hideWifi", false);
    settings.hideProfileIndicator = preferences.getBool("hideProfile", false);
    settings.hideBatteryIcon = preferences.getBool("hideBatt", false);
    settings.showBatteryPercent = preferences.getBool("battPct", false);
    settings.hideBleIcon = preferences.getBool("hideBle", false);
    settings.hideVolumeIndicator = preferences.getBool("hideVol", false);
    settings.hideRssiIndicator = preferences.getBool("hideRssi", false);
    settings.showRestTelemetryCards = preferences.getBool("restTelem", true);
    
    // Development settings
    settings.enableWifiAtBoot = preferences.getBool("wifiAtBoot", false);
    
    // Voice alert settings - migrate from old boolean to new mode
    // If old voiceAlerts key exists, migrate it; otherwise use new defaults
    bool needsMigration = preferences.isKey("voiceAlerts");
    if (needsMigration) {
        // Migrate old setting: true -> BAND_FREQ, false -> DISABLED
        bool oldEnabled = preferences.getBool("voiceAlerts", true);
        settings.voiceAlertMode = oldEnabled ? VOICE_MODE_BAND_FREQ : VOICE_MODE_DISABLED;
        settings.voiceDirectionEnabled = true;  // Old behavior always included direction
    } else {
        settings.voiceAlertMode = clampVoiceAlertModeValue(preferences.getUChar("voiceMode", VOICE_MODE_BAND_FREQ));
        settings.voiceDirectionEnabled = preferences.getBool("voiceDir", true);
    }
    
    // Close read-only preferences before migration cleanup
    if (needsMigration) {
        preferences.end();
        // Re-open in write mode to remove old key
        if (preferences.begin(activeNs.c_str(), false)) {
            preferences.remove("voiceAlerts");
            Serial.println("[Settings] Migrated voiceAlerts -> voiceMode");
            preferences.end();
        }
        // Re-open in read-only to continue loading
        preferences.begin(activeNs.c_str(), true);
    }
    settings.announceBogeyCount = preferences.getBool("voiceBogeys", true);
    settings.muteVoiceIfVolZero = preferences.getBool("muteVoiceVol0", false);
    settings.voiceVolume = std::min<uint8_t>(100, preferences.getUChar("voiceVol", 75));  // 0-100%
    
    // Secondary alert settings
    settings.announceSecondaryAlerts = preferences.getBool("secAlerts", false);
    settings.secondaryLaser = preferences.getBool("secLaser", true);
    settings.secondaryKa = preferences.getBool("secKa", true);
    settings.secondaryK = preferences.getBool("secK", false);
    settings.secondaryX = preferences.getBool("secX", false);
    
    // Volume fade settings
    settings.alertVolumeFadeEnabled = preferences.getBool("volFadeEn", false);
    settings.alertVolumeFadeDelaySec = std::clamp<uint8_t>(preferences.getUChar("volFadeSec", 2), 1, 10);  // 1-10 seconds
    settings.alertVolumeFadeVolume = std::min<uint8_t>(9, preferences.getUChar("volFadeVol", 1));  // 0-9 (V1 volume range)
    
    // Speed-based volume settings
    settings.speedVolumeEnabled = preferences.getBool("spdVolEn", false);
    settings.speedVolumeThresholdMph = clampU8(preferences.getUChar("spdVolThr", 45), 10, 100);
    settings.speedVolumeBoost = clampU8(preferences.getUChar("spdVolBoost", 2), 1, 5);
    
    // Low-speed mute settings
    settings.lowSpeedMuteEnabled = preferences.getBool("lowSpdMute", false);
    settings.lowSpeedMuteThresholdMph = clampU8(preferences.getUChar("lowSpdThr", 5), 1, 30);
    
    settings.autoPushEnabled = preferences.getBool("autoPush", true);  // Default to enabled for profiles to work
    settings.activeSlot = preferences.getInt("activeSlot", 0);
    if (settings.activeSlot < 0 || settings.activeSlot > 2) {
        settings.activeSlot = 0;
    }
    settings.slot0Name = sanitizeSlotNameValue(preferences.getString("slot0name", "DEFAULT"));
    settings.slot1Name = sanitizeSlotNameValue(preferences.getString("slot1name", "HIGHWAY"));
    settings.slot2Name = sanitizeSlotNameValue(preferences.getString("slot2name", "COMFORT"));
    settings.slot0Color = preferences.getUShort("slot0color", 0x400A);
    settings.slot1Color = preferences.getUShort("slot1color", 0x07E0);
    settings.slot2Color = preferences.getUShort("slot2color", 0x8410);
    settings.slot0Volume = clampSlotVolumeValue(preferences.getUChar("slot0vol", 0xFF));
    settings.slot1Volume = clampSlotVolumeValue(preferences.getUChar("slot1vol", 0xFF));
    settings.slot2Volume = clampSlotVolumeValue(preferences.getUChar("slot2vol", 0xFF));
    settings.slot0MuteVolume = clampSlotVolumeValue(preferences.getUChar("slot0mute", 0xFF));
    settings.slot1MuteVolume = clampSlotVolumeValue(preferences.getUChar("slot1mute", 0xFF));
    settings.slot2MuteVolume = clampSlotVolumeValue(preferences.getUChar("slot2mute", 0xFF));
    settings.slot0DarkMode = preferences.getBool("slot0dark", false);
    settings.slot1DarkMode = preferences.getBool("slot1dark", false);
    settings.slot2DarkMode = preferences.getBool("slot2dark", false);
    settings.slot0MuteToZero = preferences.getBool("slot0mz", false);
    settings.slot1MuteToZero = preferences.getBool("slot1mz", false);
    settings.slot2MuteToZero = preferences.getBool("slot2mz", false);
    settings.slot0AlertPersist = std::min<uint8_t>(5, preferences.getUChar("slot0persist", 0));
    settings.slot1AlertPersist = std::min<uint8_t>(5, preferences.getUChar("slot1persist", 0));
    settings.slot2AlertPersist = std::min<uint8_t>(5, preferences.getUChar("slot2persist", 0));
    settings.slot0PriorityArrow = preferences.getBool("slot0prio", false);
    settings.slot1PriorityArrow = preferences.getBool("slot1prio", false);
    settings.slot2PriorityArrow = preferences.getBool("slot2prio", false);
    settings.slot0_default.profileName = sanitizeProfileNameValue(preferences.getString("slot0prof", ""));
    settings.slot0_default.mode = normalizeV1ModeValue(preferences.getInt("slot0mode", V1_MODE_UNKNOWN));
    settings.slot1_highway.profileName = sanitizeProfileNameValue(preferences.getString("slot1prof", ""));
    settings.slot1_highway.mode = normalizeV1ModeValue(preferences.getInt("slot1mode", V1_MODE_UNKNOWN));
    settings.slot2_comfort.profileName = sanitizeProfileNameValue(preferences.getString("slot2prof", ""));
    settings.slot2_comfort.mode = normalizeV1ModeValue(preferences.getInt("slot2mode", V1_MODE_UNKNOWN));
    settings.lastV1Address = sanitizeLastV1AddressValue(preferences.getString("lastV1Addr", ""));
    settings.autoPowerOffMinutes = clampU8(preferences.getUChar("autoPwrOff", 0), 0, 60);
    settings.apTimeoutMinutes = clampApTimeoutValue(preferences.getUChar("apTimeout", 0));
    
    preferences.end();
    
    Serial.printf("[Settings] OK wifi=%s proxy=%s bright=%d autoPush=%s\n",
                  settings.enableWifi ? "on" : "off",
                  settings.proxyBLE ? "on" : "off",
                  settings.brightness,
                  settings.autoPushEnabled ? "on" : "off");
}

void SettingsManager::save() {
    if (!persistSettingsAtomically()) {
        return;
    }

    Serial.println("Settings saved atomically");

    // Backup display settings to SD card (survives reflash)
    backupToSD();
}

// --- setWiFiEnabled, setAPCredentials moved to settings_setters.cpp ---

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

// --- Property setters (setProxyBLE through setLastV1Address), slot getters/setters,
//     resetToDefaults moved to settings_setters.cpp ---
// Check if NVS appears to be in default state (likely erased during reflash)

// --- checkNeedsRestore, backupToSD, restoreFromSD, validateProfileReferences
//     moved to settings_backup.cpp ---

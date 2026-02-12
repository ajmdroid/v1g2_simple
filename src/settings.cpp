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

#include "settings.h"
#include "storage_manager.h"
#include "v1_profiles.h"
#include <ArduinoJson.h>
#include <algorithm>

// SD backup file path
static const char* SETTINGS_BACKUP_PATH = "/v1simple_backup.json";
static const int SD_BACKUP_VERSION = 4;  // Increment when adding new fields to backup
static const char* SETTINGS_NS_A = "v1settingsA";
static const char* SETTINGS_NS_B = "v1settingsB";
static const char* SETTINGS_NS_META = "v1settingsMeta";
static const char* SETTINGS_NS_LEGACY = "v1settings";
static const char* WIFI_CLIENT_NS = "v1wificlient";
static const char* WIFI_CLIENT_SD_SECRET_PATH = "/v1wifi_secret.json";
static const char* WIFI_CLIENT_SD_SECRET_TYPE = "v1wifi_secret";
static const int WIFI_CLIENT_SD_SECRET_VERSION = 1;

// Global instance
SettingsManager settingsManager;

// XOR obfuscation key - deters casual reading but NOT cryptographically secure
// See security note above for rationale
static const char XOR_KEY[] = "V1G2-S3cr3t-K3y!";
static const int SETTINGS_VERSION = 2;  // Increment when changing password encoding
static const char* OBFUSCATION_HEX_PREFIX = "hex:";

// NVS recovery: clear unused namespace when NVS is full
// Returns true if space was freed
static bool attemptNvsRecovery(const char* activeNs) {
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
static String xorObfuscate(const String& input) {
    if (input.length() == 0) return input;
    
    String output;
    output.reserve(input.length());
    size_t keyLen = strlen(XOR_KEY);
    
    for (size_t i = 0; i < input.length(); i++) {
        output += (char)(input[i] ^ XOR_KEY[i % keyLen]);
    }
    return output;
}

static char hexDigit(uint8_t nibble) {
    nibble &= 0x0F;
    return (nibble < 10) ? static_cast<char>('0' + nibble)
                         : static_cast<char>('A' + (nibble - 10));
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static String bytesToHex(const String& input) {
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

static bool hexToBytes(const String& input, String& out) {
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

static String encodeObfuscatedForStorage(const String& plainText) {
    if (plainText.length() == 0) return "";
    String obfuscated = xorObfuscate(plainText);
    String encoded = OBFUSCATION_HEX_PREFIX;
    encoded += bytesToHex(obfuscated);
    return encoded;
}

static String decodeObfuscatedFromStorage(const String& stored) {
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

static bool saveWifiClientSecretToSD(const String& ssid, const String& encodedPassword) {
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

static String loadWifiClientSecretFromSD(const String& expectedSsid) {
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

static void clearWifiClientSecretFromSD() {
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

String SettingsManager::getActiveNamespace() {
    Preferences meta;
    if (meta.begin(SETTINGS_NS_META, true)) {
        String active = meta.getString("active", "");
        meta.end();
        if (active.length() > 0) {
            return active;
        }
    }
    return String(SETTINGS_NS_LEGACY);
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
    written += prefs.putBool("obdVwData", settings.obdVwDataEnabled);
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
    written += prefs.putBool("debugLog", settings.enableDebugLogging);
    written += prefs.putBool("logAlerts", settings.logAlerts);
    written += prefs.putBool("logWifi", settings.logWifi);
    written += prefs.putBool("logBle", settings.logBle);
    written += prefs.putBool("logSystem", settings.logSystem);
    written += prefs.putBool("logDisplay", settings.logDisplay);
    written += prefs.putBool("logPerfMet", settings.logPerfMetrics);
    written += prefs.putBool("logAudio", settings.logAudio);
    written += prefs.putBool("logTouch", settings.logTouch);
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
        return false;
    }

    bool committed = meta.putString("active", stagingNs) > 0;
    meta.end();

    if (!committed) {
        Serial.println("[Settings] ERROR: Failed to update active settings namespace");
        return false;
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

bool SettingsManager::checkAndRestoreFromSD() {
    // Check if NVS was erased (appears default) and backup exists on SD
    // This can be called after storage is mounted to retry the restore
    bool needsRestore = checkNeedsRestore();
    fs::FS* fs = nullptr;
    bool hasSdBackup = false;
    if (storageManager.isReady() && storageManager.isSDCard()) {
        fs = storageManager.getFilesystem();
        hasSdBackup = fs && (fs->exists(SETTINGS_BACKUP_PATH)
            || fs->exists("/v1simple_settings.json")
            || fs->exists("/v1settings_backup.json"));
    }

    if (!needsRestore) {
        bool slotsEmpty = settings.slot0_default.profileName.length() == 0
            && settings.slot1_highway.profileName.length() == 0
            && settings.slot2_comfort.profileName.length() == 0;
        if (slotsEmpty && hasSdBackup) {
            Serial.println("[Settings] Slots empty, checking for SD backup...");
            needsRestore = true;
        } else if (hasSdBackup && v1ProfileManager.isReady()) {
            auto slotProfileMissing = [&](const AutoPushSlot& slot) -> bool {
                if (slot.profileName.length() == 0) {
                    return false;
                }
                V1Profile testProfile;
                return !v1ProfileManager.loadProfile(slot.profileName, testProfile);
            };

            if (slotProfileMissing(settings.slot0_default)
                || slotProfileMissing(settings.slot1_highway)
                || slotProfileMissing(settings.slot2_comfort)) {
                Serial.println("[Settings] Slot profile reference missing on filesystem, checking SD backup...");
                needsRestore = true;
            }
        }
    }

    if (needsRestore) {
        Serial.println("[Settings] Checking for SD backup restore...");
        if (restoreFromSD()) {
            Serial.println("[Settings] Restored settings from SD backup!");
            return true;
        }
    }
    return false;
}

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
        // Passwords are obfuscated - decode them
        settings.apPassword = storedApPwd.length() > 0 ? decodeObfuscatedFromStorage(storedApPwd) : "setupv1g2";
    } else {
        // Version 1 - passwords stored in plain text, use as-is
        settings.apPassword = storedApPwd.length() > 0 ? storedApPwd : "setupv1g2";
        Serial.println("[Settings] Migrating from v1 to v2 (password obfuscation)");
    }
    
    settings.apSSID = preferences.getString("apSSID", "V1-Simple");
    
    // WiFi client (STA) settings
    settings.wifiClientEnabled = preferences.getBool("wifiClientEn", false);
    settings.wifiClientSSID = preferences.getString("wifiClSSID", "");
    // Determine WiFi mode based on client enabled state
    settings.wifiMode = settings.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;
    
    // Debug: Log WiFi client settings on load
    Serial.printf("[Settings] WiFi client: enabled=%s, SSID='%s'\n",
                  settings.wifiClientEnabled ? "true" : "false",
                  settings.wifiClientSSID.c_str());
    
    settings.proxyBLE = preferences.getBool("proxyBLE", true);
    settings.proxyName = preferences.getString("proxyName", "V1-Proxy");
    settings.obdVwDataEnabled = preferences.getBool("obdVwData", true);
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
    settings.freqUseBandColor = preferences.getBool("freqBandCol", false);  // Use custom freq color by default
    settings.hideWifiIcon = preferences.getBool("hideWifi", false);
    settings.hideProfileIndicator = preferences.getBool("hideProfile", false);
    settings.hideBatteryIcon = preferences.getBool("hideBatt", false);
    settings.showBatteryPercent = preferences.getBool("battPct", false);
    settings.hideBleIcon = preferences.getBool("hideBle", false);
    settings.hideVolumeIndicator = preferences.getBool("hideVol", false);
    settings.hideRssiIndicator = preferences.getBool("hideRssi", false);
    settings.showRestTelemetryCards = preferences.getBool("restTelem", true);
    
    // Development/Debug settings
    settings.enableWifiAtBoot = preferences.getBool("wifiAtBoot", false);
    settings.enableDebugLogging = preferences.getBool("debugLog", false);  // Off by default for stability
    settings.logAlerts = preferences.getBool("logAlerts", false);
    settings.logWifi = preferences.getBool("logWifi", false);
    settings.logBle = preferences.getBool("logBle", false);
    settings.logSystem = preferences.getBool("logSystem", false);
    settings.logDisplay = preferences.getBool("logDisplay", false);
    settings.logPerfMetrics = preferences.getBool("logPerfMet", true);  // ON by default for stability monitoring
    settings.logAudio = preferences.getBool("logAudio", false);
    settings.logTouch = preferences.getBool("logTouch", false);
    
    // Voice alert settings - migrate from old boolean to new mode
    // If old voiceAlerts key exists, migrate it; otherwise use new defaults
    bool needsMigration = preferences.isKey("voiceAlerts");
    if (needsMigration) {
        // Migrate old setting: true -> BAND_FREQ, false -> DISABLED
        bool oldEnabled = preferences.getBool("voiceAlerts", true);
        settings.voiceAlertMode = oldEnabled ? VOICE_MODE_BAND_FREQ : VOICE_MODE_DISABLED;
        settings.voiceDirectionEnabled = true;  // Old behavior always included direction
    } else {
        settings.voiceAlertMode = (VoiceAlertMode)preferences.getUChar("voiceMode", VOICE_MODE_BAND_FREQ);
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
    settings.speedVolumeThresholdMph = preferences.getUChar("spdVolThr", 45);  // No upper bound needed for speed
    settings.speedVolumeBoost = std::min<uint8_t>(9, preferences.getUChar("spdVolBoost", 2));  // 0-9 (V1 volume range)
    
    // Low-speed mute settings
    settings.lowSpeedMuteEnabled = preferences.getBool("lowSpdMute", false);
    settings.lowSpeedMuteThresholdMph = preferences.getUChar("lowSpdThr", 5);  // No upper bound needed for speed;
    
    settings.autoPushEnabled = preferences.getBool("autoPush", true);  // Default to enabled for profiles to work
    settings.activeSlot = preferences.getInt("activeSlot", 0);
    if (settings.activeSlot < 0 || settings.activeSlot > 2) {
        settings.activeSlot = 0;
    }
    settings.slot0Name = preferences.getString("slot0name", "DEFAULT");
    settings.slot1Name = preferences.getString("slot1name", "HIGHWAY");
    settings.slot2Name = preferences.getString("slot2name", "COMFORT");
    settings.slot0Color = preferences.getUShort("slot0color", 0x400A);
    settings.slot1Color = preferences.getUShort("slot1color", 0x07E0);
    settings.slot2Color = preferences.getUShort("slot2color", 0x8410);
    settings.slot0Volume = preferences.getUChar("slot0vol", 0xFF);
    settings.slot1Volume = preferences.getUChar("slot1vol", 0xFF);
    settings.slot2Volume = preferences.getUChar("slot2vol", 0xFF);
    settings.slot0MuteVolume = preferences.getUChar("slot0mute", 0xFF);
    settings.slot1MuteVolume = preferences.getUChar("slot1mute", 0xFF);
    settings.slot2MuteVolume = preferences.getUChar("slot2mute", 0xFF);
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
    settings.slot0_default.profileName = preferences.getString("slot0prof", "");
    settings.slot0_default.mode = static_cast<V1Mode>(preferences.getInt("slot0mode", V1_MODE_UNKNOWN));
    settings.slot1_highway.profileName = preferences.getString("slot1prof", "");
    settings.slot1_highway.mode = static_cast<V1Mode>(preferences.getInt("slot1mode", V1_MODE_UNKNOWN));
    settings.slot2_comfort.profileName = preferences.getString("slot2prof", "");
    settings.slot2_comfort.mode = static_cast<V1Mode>(preferences.getInt("slot2mode", V1_MODE_UNKNOWN));
    settings.lastV1Address = preferences.getString("lastV1Addr", "");
    settings.autoPowerOffMinutes = preferences.getUChar("autoPwrOff", 0);
    settings.apTimeoutMinutes = preferences.getUChar("apTimeout", 0);  // Default: always on
    
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

void SettingsManager::setWiFiEnabled(bool enabled) {
    settings.enableWifi = enabled;
    save();
}

void SettingsManager::setAPCredentials(const String& ssid, const String& password) {
    settings.apSSID = ssid;
    settings.apPassword = password;
    save();
}

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
    settings.wifiClientSSID = ssid;
    settings.wifiClientEnabled = true;
    settings.wifiMode = V1_WIFI_APSTA;

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
    if (saveWifiClientSecretToSD(ssid, encodedPassword)) {
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

void SettingsManager::setProxyBLE(bool enabled) {
    settings.proxyBLE = enabled;
    save();
}

void SettingsManager::setProxyName(const String& name) {
    settings.proxyName = name;
    save();
}

void SettingsManager::setObdVwDataEnabled(bool enabled) {
    if (settings.obdVwDataEnabled == enabled) {
        return;
    }
    settings.obdVwDataEnabled = enabled;
    save();
}

void SettingsManager::setAutoPowerOffMinutes(uint8_t minutes) {
    settings.autoPowerOffMinutes = minutes;
    save();
}

void SettingsManager::setApTimeoutMinutes(uint8_t minutes) {
    settings.apTimeoutMinutes = minutes;
    save();
}

void SettingsManager::setBrightness(uint8_t brightness) {
    settings.brightness = brightness;
    save();
}

void SettingsManager::setDisplayOff(bool off) {
    settings.turnOffDisplay = off;
    save();
}

void SettingsManager::setAutoPushEnabled(bool enabled) {
    settings.autoPushEnabled = enabled;
    save();
}

void SettingsManager::setActiveSlot(int slot) {
    settings.activeSlot = std::max(0, std::min(2, slot));
    save();
}

void SettingsManager::setSlot(int slotNum, const String& profileName, V1Mode mode) {
    switch (slotNum) {
        case 0:
            settings.slot0_default.profileName = profileName;
            settings.slot0_default.mode = mode;
            break;
        case 1:
            settings.slot1_highway.profileName = profileName;
            settings.slot1_highway.mode = mode;
            break;
        case 2:
            settings.slot2_comfort.profileName = profileName;
            settings.slot2_comfort.mode = mode;
            break;
    }
    save();
}

void SettingsManager::setSlotName(int slotNum, const String& name) {
    // Convert to uppercase and limit to 20 characters for display consistency
    String upperName = name;
    upperName.toUpperCase();
    if (upperName.length() > 20) {
        upperName = upperName.substring(0, 20);
    }
    
    switch (slotNum) {
        case 0: settings.slot0Name = upperName; break;
        case 1: settings.slot1Name = upperName; break;
        case 2: settings.slot2Name = upperName; break;
    }
    save();
}

void SettingsManager::setSlotColor(int slotNum, uint16_t color) {
    switch (slotNum) {
        case 0: settings.slot0Color = color; break;
        case 1: settings.slot1Color = color; break;
        case 2: settings.slot2Color = color; break;
    }
    save();
}

void SettingsManager::setSlotVolumes(int slotNum, uint8_t volume, uint8_t muteVolume) {
    switch (slotNum) {
        case 0: settings.slot0Volume = volume; settings.slot0MuteVolume = muteVolume; break;
        case 1: settings.slot1Volume = volume; settings.slot1MuteVolume = muteVolume; break;
        case 2: settings.slot2Volume = volume; settings.slot2MuteVolume = muteVolume; break;
    }
    save();
}

void SettingsManager::setDisplayColors(uint16_t bogey, uint16_t freq, uint16_t arrowFront, uint16_t arrowSide, uint16_t arrowRear,
                                        uint16_t bandL, uint16_t bandKa, uint16_t bandK, uint16_t bandX, bool deferSave) {
    settings.colorBogey = bogey;
    settings.colorFrequency = freq;
    settings.colorArrowFront = arrowFront;
    settings.colorArrowSide = arrowSide;
    settings.colorArrowRear = arrowRear;
    settings.colorBandL = bandL;
    settings.colorBandKa = bandKa;
    settings.colorBandK = bandK;
    settings.colorBandX = bandX;
    if (!deferSave) save();
}

void SettingsManager::setWiFiIconColors(uint16_t icon, uint16_t connected) {
    settings.colorWiFiIcon = icon;
    settings.colorWiFiConnected = connected;
    save();
}

void SettingsManager::setBleIconColors(uint16_t connected, uint16_t disconnected) {
    settings.colorBleConnected = connected;
    settings.colorBleDisconnected = disconnected;
    save();
}

void SettingsManager::setSignalBarColors(uint16_t bar1, uint16_t bar2, uint16_t bar3, uint16_t bar4, uint16_t bar5, uint16_t bar6) {
    settings.colorBar1 = bar1;
    settings.colorBar2 = bar2;
    settings.colorBar3 = bar3;
    settings.colorBar4 = bar4;
    settings.colorBar5 = bar5;
    settings.colorBar6 = bar6;
    save();
}

void SettingsManager::setMutedColor(uint16_t color) {
    settings.colorMuted = color;
    save();
}

void SettingsManager::setBandPhotoColor(uint16_t color) {
    settings.colorBandPhoto = color;
    save();
}

void SettingsManager::setPersistedColor(uint16_t color) {
    settings.colorPersisted = color;
    save();
}

void SettingsManager::setVolumeMainColor(uint16_t color) {
    settings.colorVolumeMain = color;
    save();
}

void SettingsManager::setVolumeMuteColor(uint16_t color) {
    settings.colorVolumeMute = color;
    save();
}

void SettingsManager::setRssiV1Color(uint16_t color) {
    settings.colorRssiV1 = color;
    save();
}

void SettingsManager::setRssiProxyColor(uint16_t color) {
    settings.colorRssiProxy = color;
    save();
}

void SettingsManager::setFreqUseBandColor(bool use) {
    settings.freqUseBandColor = use;
    save();
}

void SettingsManager::setHideWifiIcon(bool hide) {
    settings.hideWifiIcon = hide;
    save();
}

void SettingsManager::setHideProfileIndicator(bool hide) {
    settings.hideProfileIndicator = hide;
    save();
}

void SettingsManager::setHideBatteryIcon(bool hide) {
    settings.hideBatteryIcon = hide;
    save();
}

void SettingsManager::setShowBatteryPercent(bool show) {
    settings.showBatteryPercent = show;
    save();
}

void SettingsManager::setHideBleIcon(bool hide) {
    settings.hideBleIcon = hide;
    save();
}

void SettingsManager::setHideVolumeIndicator(bool hide) {
    settings.hideVolumeIndicator = hide;
    save();
}

void SettingsManager::setHideRssiIndicator(bool hide) {
    settings.hideRssiIndicator = hide;
    save();
}

void SettingsManager::setShowRestTelemetryCards(bool show) {
    settings.showRestTelemetryCards = show;
    save();
}

void SettingsManager::setEnableWifiAtBoot(bool enable, bool deferSave) {
    settings.enableWifiAtBoot = enable;
    if (!deferSave) save();
}

void SettingsManager::setEnableDebugLogging(bool enable, bool deferSave) {
    settings.enableDebugLogging = enable;
    if (!deferSave) save();
}

void SettingsManager::setLogAlerts(bool enable, bool deferSave) {
    settings.logAlerts = enable;
    if (!deferSave) save();
}

void SettingsManager::setLogWifi(bool enable, bool deferSave) {
    settings.logWifi = enable;
    if (!deferSave) save();
}

void SettingsManager::setLogBle(bool enable, bool deferSave) {
    settings.logBle = enable;
    if (!deferSave) save();
}

void SettingsManager::setLogSystem(bool enable, bool deferSave) {
    settings.logSystem = enable;
    if (!deferSave) save();
}

void SettingsManager::setLogDisplay(bool enable, bool deferSave) {
    settings.logDisplay = enable;
    if (!deferSave) save();
}

void SettingsManager::setLogPerfMetrics(bool enable, bool deferSave) {
    settings.logPerfMetrics = enable;
    if (!deferSave) save();
}

void SettingsManager::setLogAudio(bool enable, bool deferSave) {
    settings.logAudio = enable;
    if (!deferSave) save();
}

void SettingsManager::setLogTouch(bool enable, bool deferSave) {
    settings.logTouch = enable;
    if (!deferSave) save();
}

void SettingsManager::setVoiceAlertMode(VoiceAlertMode mode) {
    settings.voiceAlertMode = mode;
    save();
}

void SettingsManager::setVoiceDirectionEnabled(bool enabled) {
    settings.voiceDirectionEnabled = enabled;
    save();
}

void SettingsManager::setAnnounceBogeyCount(bool enabled) {
    settings.announceBogeyCount = enabled;
    save();
}

void SettingsManager::setMuteVoiceIfVolZero(bool mute) {
    settings.muteVoiceIfVolZero = mute;
    save();
}

void SettingsManager::setAnnounceSecondaryAlerts(bool enabled) {
    settings.announceSecondaryAlerts = enabled;
    save();
}

void SettingsManager::setSecondaryLaser(bool enabled) {
    settings.secondaryLaser = enabled;
    save();
}

void SettingsManager::setSecondaryKa(bool enabled) {
    settings.secondaryKa = enabled;
    save();
}

void SettingsManager::setSecondaryK(bool enabled) {
    settings.secondaryK = enabled;
    save();
}

void SettingsManager::setSecondaryX(bool enabled) {
    settings.secondaryX = enabled;
    save();
}

void SettingsManager::setAlertVolumeFade(bool enabled, uint8_t delaySec, uint8_t volume) {
    settings.alertVolumeFadeEnabled = enabled;
    settings.alertVolumeFadeDelaySec = delaySec;
    settings.alertVolumeFadeVolume = volume;
    save();
}

void SettingsManager::setSpeedVolume(bool enabled, uint8_t thresholdMph, uint8_t boost) {
    settings.speedVolumeEnabled = enabled;
    settings.speedVolumeThresholdMph = thresholdMph;
    settings.speedVolumeBoost = boost;
    save();
}

void SettingsManager::setLowSpeedMute(bool enabled, uint8_t thresholdMph) {
    settings.lowSpeedMuteEnabled = enabled;
    settings.lowSpeedMuteThresholdMph = thresholdMph;
    save();
}

const AutoPushSlot& SettingsManager::getActiveSlot() const {
    switch (settings.activeSlot) {
        case 1: return settings.slot1_highway;
        case 2: return settings.slot2_comfort;
        default: return settings.slot0_default;
    }
}

const AutoPushSlot& SettingsManager::getSlot(int slotNum) const {
    switch (slotNum) {
        case 1: return settings.slot1_highway;
        case 2: return settings.slot2_comfort;
        default: return settings.slot0_default;
    }
}

uint8_t SettingsManager::getSlotVolume(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0Volume;
        case 1: return settings.slot1Volume;
        case 2: return settings.slot2Volume;
        default: return 0xFF;
    }
}

uint8_t SettingsManager::getSlotMuteVolume(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0MuteVolume;
        case 1: return settings.slot1MuteVolume;
        case 2: return settings.slot2MuteVolume;
        default: return 0xFF;
    }
}

bool SettingsManager::getSlotDarkMode(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0DarkMode;
        case 1: return settings.slot1DarkMode;
        case 2: return settings.slot2DarkMode;
        default: return false;
    }
}

bool SettingsManager::getSlotMuteToZero(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0MuteToZero;
        case 1: return settings.slot1MuteToZero;
        case 2: return settings.slot2MuteToZero;
        default: return false;
    }
}

uint8_t SettingsManager::getSlotAlertPersistSec(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0AlertPersist;
        case 1: return settings.slot1AlertPersist;
        case 2: return settings.slot2AlertPersist;
        default: return 0;
    }
}

void SettingsManager::setSlotDarkMode(int slotNum, bool darkMode) {
    switch (slotNum) {
        case 0: settings.slot0DarkMode = darkMode; break;
        case 1: settings.slot1DarkMode = darkMode; break;
        case 2: settings.slot2DarkMode = darkMode; break;
    }
    save();
}

void SettingsManager::setSlotMuteToZero(int slotNum, bool mz) {
    switch (slotNum) {
        case 0: settings.slot0MuteToZero = mz; break;
        case 1: settings.slot1MuteToZero = mz; break;
        case 2: settings.slot2MuteToZero = mz; break;
    }
    save();
}

void SettingsManager::setSlotAlertPersistSec(int slotNum, uint8_t seconds) {
    uint8_t clamped = std::min<uint8_t>(5, seconds);
    switch (slotNum) {
        case 0: settings.slot0AlertPersist = clamped; break;
        case 1: settings.slot1AlertPersist = clamped; break;
        case 2: settings.slot2AlertPersist = clamped; break;
        default: return;
    }
    save();
}

bool SettingsManager::getSlotPriorityArrowOnly(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0PriorityArrow;
        case 1: return settings.slot1PriorityArrow;
        case 2: return settings.slot2PriorityArrow;
        default: return false;
    }
}

void SettingsManager::setSlotPriorityArrowOnly(int slotNum, bool prioArrow) {
    switch (slotNum) {
        case 0: settings.slot0PriorityArrow = prioArrow; break;
        case 1: settings.slot1PriorityArrow = prioArrow; break;
        case 2: settings.slot2PriorityArrow = prioArrow; break;
    }
    save();
}

void SettingsManager::resetToDefaults() {
    settings = V1Settings();  // Reset to defaults
    save();
    Serial.println("Settings reset to defaults");
}

void SettingsManager::setLastV1Address(const String& addr) {
    if (addr != settings.lastV1Address) {
        settings.lastV1Address = addr;
        save();
        Serial.printf("Saved new V1 address: %s\n", addr.c_str());
    }
}
// Check if NVS appears to be in default state (likely erased during reflash)
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
    
    return false;
}

// Backup display/color settings to SD card
void SettingsManager::backupToSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return;  // SD not available, skip silently
    }
    
    // Acquire SD mutex to protect file I/O
    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
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
    doc["obdVwDataEnabled"] = settings.obdVwDataEnabled;
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
    doc["enableDebugLogging"] = settings.enableDebugLogging;
    doc["logAlerts"] = settings.logAlerts;
    doc["logWifi"] = settings.logWifi;
    doc["logBle"] = settings.logBle;
    doc["logSystem"] = settings.logSystem;
    doc["logDisplay"] = settings.logDisplay;
    doc["logPerfMetrics"] = settings.logPerfMetrics;
    doc["logAudio"] = settings.logAudio;
    doc["logTouch"] = settings.logTouch;
    
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
    
    // Write to file
    File file = fs->open(SETTINGS_BACKUP_PATH, FILE_WRITE);
    if (!file) {
        Serial.println("[Settings] Failed to create SD backup file");
        return;
    }
    
    serializeJson(doc, file);
    file.flush();
    file.close();
    
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
    
    // Check both old and new backup paths for compatibility
    const char* backupPath = SETTINGS_BACKUP_PATH;
    if (!fs->exists(backupPath)) {
        // Try legacy paths
        if (fs->exists("/v1simple_settings.json")) {
            backupPath = "/v1simple_settings.json";
        } else if (fs->exists("/v1settings_backup.json")) {
            backupPath = "/v1settings_backup.json";
        } else {
            Serial.println("[Settings] No SD backup found");
            return false;
        }
    }
    
    Serial.printf("[Settings] Using backup file: %s\n", backupPath);
    File file = fs->open(backupPath, FILE_READ);
    if (!file) {
        Serial.println("[Settings] Failed to open SD backup");
        return false;
    }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    
    if (err) {
        Serial.printf("[Settings] Failed to parse SD backup: %s\n", err.c_str());
        return false;
    }
    
    int backupVersion = doc["_version"] | doc["version"] | 1;
    Serial.printf("[Settings] Restoring from SD backup (version %d)\n", backupVersion);
    const bool hasAutoPush = doc["autoPushEnabled"].is<bool>();
    const bool backupAutoPush = hasAutoPush ? doc["autoPushEnabled"].as<bool>() : false;
    const char* backupSlot0 = doc["slot0ProfileName"].is<const char*>()
        ? doc["slot0ProfileName"].as<const char*>() : "";
    const int backupSlot0Mode = doc["slot0Mode"].is<int>() ? doc["slot0Mode"].as<int>() : -1;
    Serial.printf("[Settings] Backup fields: autoPush=%s slot0Profile='%s' slot0Mode=%d\n",
                  hasAutoPush ? (backupAutoPush ? "true" : "false") : "missing",
                  backupSlot0,
                  backupSlot0Mode);
    
    // === WiFi/Network Settings (v2+) ===
    // Note: AP password NOT restored from SD for security - user must re-enter after restore
    if (doc["enableWifi"].is<bool>()) settings.enableWifi = doc["enableWifi"];
    if (doc["wifiMode"].is<int>()) settings.wifiMode = (WiFiModeSetting)doc["wifiMode"].as<int>();
    if (doc["apSSID"].is<const char*>()) settings.apSSID = doc["apSSID"].as<String>();
    if (doc["wifiClientEnabled"].is<bool>()) settings.wifiClientEnabled = doc["wifiClientEnabled"];
    if (doc["wifiClientSSID"].is<const char*>()) settings.wifiClientSSID = doc["wifiClientSSID"].as<String>();
    if (doc["proxyBLE"].is<bool>()) settings.proxyBLE = doc["proxyBLE"];
    if (doc["proxyName"].is<const char*>()) settings.proxyName = doc["proxyName"].as<String>();
    if (doc["obdVwDataEnabled"].is<bool>()) settings.obdVwDataEnabled = doc["obdVwDataEnabled"];
    if (doc["lastV1Address"].is<const char*>()) settings.lastV1Address = doc["lastV1Address"].as<String>();
    if (doc["autoPowerOffMinutes"].is<int>()) settings.autoPowerOffMinutes = doc["autoPowerOffMinutes"];
    if (doc["apTimeoutMinutes"].is<int>()) settings.apTimeoutMinutes = doc["apTimeoutMinutes"];
    
    
    // === Display Settings ===
    if (doc["brightness"].is<int>()) settings.brightness = doc["brightness"];
    if (doc["turnOffDisplay"].is<bool>()) settings.turnOffDisplay = doc["turnOffDisplay"];
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
    if (doc["freqUseBandColor"].is<bool>()) settings.freqUseBandColor = doc["freqUseBandColor"];
    
    // === UI Toggles ===
    if (doc["hideWifiIcon"].is<bool>()) settings.hideWifiIcon = doc["hideWifiIcon"];
    if (doc["hideProfileIndicator"].is<bool>()) settings.hideProfileIndicator = doc["hideProfileIndicator"];
    if (doc["hideBatteryIcon"].is<bool>()) settings.hideBatteryIcon = doc["hideBatteryIcon"];
    if (doc["showBatteryPercent"].is<bool>()) settings.showBatteryPercent = doc["showBatteryPercent"];
    if (doc["hideBleIcon"].is<bool>()) settings.hideBleIcon = doc["hideBleIcon"];
    if (doc["hideVolumeIndicator"].is<bool>()) settings.hideVolumeIndicator = doc["hideVolumeIndicator"];
    if (doc["hideRssiIndicator"].is<bool>()) settings.hideRssiIndicator = doc["hideRssiIndicator"];
    if (doc["showRestTelemetryCards"].is<bool>()) settings.showRestTelemetryCards = doc["showRestTelemetryCards"];
    if (doc["enableWifiAtBoot"].is<bool>()) settings.enableWifiAtBoot = doc["enableWifiAtBoot"];
    if (doc["enableDebugLogging"].is<bool>()) settings.enableDebugLogging = doc["enableDebugLogging"];
    if (doc["logAlerts"].is<bool>()) settings.logAlerts = doc["logAlerts"];
    if (doc["logWifi"].is<bool>()) settings.logWifi = doc["logWifi"];
    if (doc["logBle"].is<bool>()) settings.logBle = doc["logBle"];
    if (doc["logSystem"].is<bool>()) settings.logSystem = doc["logSystem"];
    if (doc["logDisplay"].is<bool>()) settings.logDisplay = doc["logDisplay"];
    if (doc["logPerfMetrics"].is<bool>()) settings.logPerfMetrics = doc["logPerfMetrics"];
    if (doc["logAudio"].is<bool>()) settings.logAudio = doc["logAudio"];
    if (doc["logTouch"].is<bool>()) settings.logTouch = doc["logTouch"];
    
    // === Voice Settings ===
    if (doc["voiceAlertMode"].is<int>()) {
        settings.voiceAlertMode = (VoiceAlertMode)doc["voiceAlertMode"].as<int>();
    } else if (doc["voiceAlertsEnabled"].is<bool>()) {
        settings.voiceAlertMode = doc["voiceAlertsEnabled"].as<bool>() ? VOICE_MODE_BAND_FREQ : VOICE_MODE_DISABLED;
    }
    if (doc["voiceDirectionEnabled"].is<bool>()) settings.voiceDirectionEnabled = doc["voiceDirectionEnabled"];
    if (doc["announceBogeyCount"].is<bool>()) settings.announceBogeyCount = doc["announceBogeyCount"];
    if (doc["muteVoiceIfVolZero"].is<bool>()) settings.muteVoiceIfVolZero = doc["muteVoiceIfVolZero"];
    if (doc["voiceVolume"].is<int>()) settings.voiceVolume = doc["voiceVolume"];
    if (doc["announceSecondaryAlerts"].is<bool>()) settings.announceSecondaryAlerts = doc["announceSecondaryAlerts"];
    if (doc["secondaryLaser"].is<bool>()) settings.secondaryLaser = doc["secondaryLaser"];
    if (doc["secondaryKa"].is<bool>()) settings.secondaryKa = doc["secondaryKa"];
    if (doc["secondaryK"].is<bool>()) settings.secondaryK = doc["secondaryK"];
    if (doc["secondaryX"].is<bool>()) settings.secondaryX = doc["secondaryX"];
    if (doc["alertVolumeFadeEnabled"].is<bool>()) settings.alertVolumeFadeEnabled = doc["alertVolumeFadeEnabled"];
    if (doc["alertVolumeFadeDelaySec"].is<int>()) settings.alertVolumeFadeDelaySec = doc["alertVolumeFadeDelaySec"];
    if (doc["alertVolumeFadeVolume"].is<int>()) settings.alertVolumeFadeVolume = doc["alertVolumeFadeVolume"];
    if (doc["speedVolumeEnabled"].is<bool>()) settings.speedVolumeEnabled = doc["speedVolumeEnabled"];
    if (doc["speedVolumeThresholdMph"].is<int>()) settings.speedVolumeThresholdMph = doc["speedVolumeThresholdMph"];
    if (doc["speedVolumeBoost"].is<int>()) settings.speedVolumeBoost = doc["speedVolumeBoost"];
    if (doc["lowSpeedMuteEnabled"].is<bool>()) settings.lowSpeedMuteEnabled = doc["lowSpeedMuteEnabled"];
    if (doc["lowSpeedMuteThresholdMph"].is<int>()) settings.lowSpeedMuteThresholdMph = doc["lowSpeedMuteThresholdMph"];
    
    // === Auto-Push Settings (v2+) ===
    // Only allow backup to enable auto-push (avoid stale backups disabling it)
    if (doc["autoPushEnabled"].is<bool>() && doc["autoPushEnabled"].as<bool>()) {
        settings.autoPushEnabled = true;
    }
    if (doc["activeSlot"].is<int>()) settings.activeSlot = doc["activeSlot"];
    
    // === Slot 0 Full Settings ===
    if (doc["slot0Name"].is<const char*>()) settings.slot0Name = doc["slot0Name"].as<String>();
    if (doc["slot0Color"].is<int>()) settings.slot0Color = doc["slot0Color"];
    if (doc["slot0Volume"].is<int>()) settings.slot0Volume = doc["slot0Volume"];
    if (doc["slot0MuteVolume"].is<int>()) settings.slot0MuteVolume = doc["slot0MuteVolume"];
    if (doc["slot0DarkMode"].is<bool>()) settings.slot0DarkMode = doc["slot0DarkMode"];
    if (doc["slot0MuteToZero"].is<bool>()) settings.slot0MuteToZero = doc["slot0MuteToZero"];
    if (doc["slot0AlertPersist"].is<int>()) settings.slot0AlertPersist = doc["slot0AlertPersist"];
    if (doc["slot0PriorityArrow"].is<bool>()) settings.slot0PriorityArrow = doc["slot0PriorityArrow"];
    if (doc["slot0ProfileName"].is<const char*>()) settings.slot0_default.profileName = doc["slot0ProfileName"].as<String>();
    if (doc["slot0Mode"].is<int>()) settings.slot0_default.mode = static_cast<V1Mode>(doc["slot0Mode"].as<int>());
    
    // === Slot 1 Full Settings ===
    if (doc["slot1Name"].is<const char*>()) settings.slot1Name = doc["slot1Name"].as<String>();
    if (doc["slot1Color"].is<int>()) settings.slot1Color = doc["slot1Color"];
    if (doc["slot1Volume"].is<int>()) settings.slot1Volume = doc["slot1Volume"];
    if (doc["slot1MuteVolume"].is<int>()) settings.slot1MuteVolume = doc["slot1MuteVolume"];
    if (doc["slot1DarkMode"].is<bool>()) settings.slot1DarkMode = doc["slot1DarkMode"];
    if (doc["slot1MuteToZero"].is<bool>()) settings.slot1MuteToZero = doc["slot1MuteToZero"];
    if (doc["slot1AlertPersist"].is<int>()) settings.slot1AlertPersist = doc["slot1AlertPersist"];
    if (doc["slot1PriorityArrow"].is<bool>()) settings.slot1PriorityArrow = doc["slot1PriorityArrow"];
    if (doc["slot1ProfileName"].is<const char*>()) settings.slot1_highway.profileName = doc["slot1ProfileName"].as<String>();
    if (doc["slot1Mode"].is<int>()) settings.slot1_highway.mode = static_cast<V1Mode>(doc["slot1Mode"].as<int>());
    
    // === Slot 2 Full Settings ===
    if (doc["slot2Name"].is<const char*>()) settings.slot2Name = doc["slot2Name"].as<String>();
    if (doc["slot2Color"].is<int>()) settings.slot2Color = doc["slot2Color"];
    if (doc["slot2Volume"].is<int>()) settings.slot2Volume = doc["slot2Volume"];
    if (doc["slot2MuteVolume"].is<int>()) settings.slot2MuteVolume = doc["slot2MuteVolume"];
    if (doc["slot2DarkMode"].is<bool>()) settings.slot2DarkMode = doc["slot2DarkMode"];
    if (doc["slot2MuteToZero"].is<bool>()) settings.slot2MuteToZero = doc["slot2MuteToZero"];
    if (doc["slot2AlertPersist"].is<int>()) settings.slot2AlertPersist = doc["slot2AlertPersist"];
    if (doc["slot2PriorityArrow"].is<bool>()) settings.slot2PriorityArrow = doc["slot2PriorityArrow"];
    if (doc["slot2ProfileName"].is<const char*>()) settings.slot2_comfort.profileName = doc["slot2ProfileName"].as<String>();
    if (doc["slot2Mode"].is<int>()) settings.slot2_comfort.mode = static_cast<V1Mode>(doc["slot2Mode"].as<int>());

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
            profile.name = p["name"].as<String>();
            if (p["description"].is<const char*>()) profile.description = p["description"].as<String>();
            if (p["displayOn"].is<bool>()) profile.displayOn = p["displayOn"];
            if (p["mainVolume"].is<int>()) profile.mainVolume = p["mainVolume"];
            if (p["mutedVolume"].is<int>()) profile.mutedVolume = p["mutedVolume"];

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

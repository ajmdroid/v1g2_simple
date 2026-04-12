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
const size_t SETTINGS_BACKUP_MAX_BYTES = 512 * 1024;
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
static constexpr size_t MAX_OBD_SAVED_NAME_LEN = 32;
static constexpr uint32_t SETTINGS_DEFERRED_PERSIST_DEBOUNCE_MS = 750;
static constexpr uint32_t SETTINGS_DEFERRED_PERSIST_RETRY_BACKOFF_MS = 1000;

static bool isDeferredPersistDue(uint32_t nowMs, uint32_t targetMs) {
    return static_cast<int32_t>(nowMs - targetMs) >= 0;
}

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

String sanitizeObdSavedNameValue(const String& raw) {
    String value = clampStringLength(raw, MAX_OBD_SAVED_NAME_LEN);
    value.trim();
    return value;
}


// Global instance
SettingsManager settingsManager;

// XOR obfuscation key - deters casual reading but NOT cryptographically secure
// See security note above for rationale
const char XOR_KEY[] = "V1G2-S3cr3t-K3y!";
const char* OBFUSCATION_HEX_PREFIX = "hex:";


SettingsManager::SettingsManager() {}

void SettingsManager::bumpBackupRevision() {
    if (backupRevisionCounter_ == UINT32_MAX) {
        backupRevisionCounter_ = 1;
        return;
    }
    backupRevisionCounter_++;
}

void SettingsManager::clearDeferredPersistState() {
    deferredPersistPending_ = false;
    deferredPersistRetryScheduled_ = false;
    deferredPersistNextAttemptAtMs_ = 0;
}

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


void SettingsManager::load() {
    String activeNs = getActiveNamespace();
    if (!preferences_.begin(activeNs.c_str(), true)) {
        Serial.printf("[Settings] WARN: Failed to open namespace %s, falling back to legacy\n", activeNs.c_str());
        activeNs = SETTINGS_NS_LEGACY;
        if (!preferences_.begin(activeNs.c_str(), true)) {
            Serial.println("[Settings] ERROR: Failed to open preferences for reading!");
            return;
        }
    }

    // Check settings version for migration
    int storedVersion = preferences_.getInt(kNvsSettingsVer, 1);

    settings_.enableWifi = preferences_.getBool(kNvsEnableWifi, true);

    // Handle AP password storage - version 1 was plain text, version 2+ is obfuscated
    String storedApPwd = preferences_.getString(kNvsApPassword, "");

    if (storedVersion >= 2) {
        // Passwords are obfuscated - decode and sanitize them.
        settings_.apPassword = sanitizeApPasswordValue(
            storedApPwd.length() > 0 ? decodeObfuscatedFromStorage(storedApPwd) : "setupv1g2");
    } else {
        // Version 1 - passwords stored in plain text, use as-is then sanitize.
        settings_.apPassword = sanitizeApPasswordValue(storedApPwd.length() > 0 ? storedApPwd : "setupv1g2");
        Serial.println("[Settings] Migrating from v1 to v2 (password obfuscation)");
    }

    settings_.apSSID = sanitizeApSsidValue(preferences_.getString(kNvsApSsid, "V1-Simple"));

    // WiFi client (STA) settings
    const bool wifiClientEnabledKeyPresent = preferences_.isKey(kNvsWifiClientEnabled);
    const bool wifiClientSsidKeyPresent = preferences_.isKey(kNvsWifiClientSsid);
    settings_.wifiClientEnabled = preferences_.getBool(kNvsWifiClientEnabled, false);
    settings_.wifiClientSSID = sanitizeWifiClientSsidValue(preferences_.getString(kNvsWifiClientSsid, ""));

    // Self-healing: if a saved SSID exists, force wifiClientEnabled to true.
    // This covers cases where a backup restore set the SSID but wifiClientEnabled
    // was missing from the JSON, or where the two got out of sync.
    // Mirrors setWifiClientCredentials() which derives enabled from SSID length.
    if (!settings_.wifiClientEnabled && settings_.wifiClientSSID.length() > 0) {
        Serial.println("[Settings] HEAL: wifiClientEnabled was false but SSID is set — enabling");
        settings_.wifiClientEnabled = true;
    }

    // Determine WiFi mode based on client enabled state
    settings_.wifiMode = settings_.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;

    // Debug: Log WiFi client settings on load
    Serial.printf("[Settings] WiFi client keys: enabledKey=%s ssidKey=%s\n",
                  wifiClientEnabledKeyPresent ? "yes" : "no",
                  wifiClientSsidKeyPresent ? "yes" : "no");
    Serial.printf("[Settings] WiFi client: enabled=%s, SSID='%s'\n",
                  settings_.wifiClientEnabled ? "true" : "false",
                  settings_.wifiClientSSID.c_str());

    settings_.proxyBLE = preferences_.getBool(kNvsProxyBle, true);
    settings_.proxyName = sanitizeProxyNameValue(preferences_.getString(kNvsProxyName, "V1-Proxy"));
    settings_.turnOffDisplay = preferences_.getBool(kNvsDisplayOff, false);
    settings_.brightness = std::max<uint8_t>(1, preferences_.getUChar(kNvsBrightness, 200));  // Min 1 to avoid blank screen
    settings_.displayStyle = normalizeDisplayStyle(preferences_.getInt(kNvsDispStyle, DISPLAY_STYLE_CLASSIC));
    settings_.colorBogey = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBogey, 0xF800), 0xF800);
    settings_.colorFrequency = sanitizeRgb565Color(preferences_.getUShort(kNvsColorFreq, 0xF800), 0xF800);
    settings_.colorArrowFront = sanitizeRgb565Color(preferences_.getUShort(kNvsColorArrowFront, 0xF800), 0xF800);
    settings_.colorArrowSide = sanitizeRgb565Color(preferences_.getUShort(kNvsColorArrowSide, 0xF800), 0xF800);
    settings_.colorArrowRear = sanitizeRgb565Color(preferences_.getUShort(kNvsColorArrowRear, 0xF800), 0xF800);
    settings_.colorBandL = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBandLaser, 0x001F), 0x001F);
    settings_.colorBandKa = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBandKa, 0xF800), 0xF800);
    settings_.colorBandK = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBandK, 0x001F), 0x001F);
    settings_.colorBandX = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBandX, 0x07E0), 0x07E0);
    settings_.colorBandPhoto = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBandPhoto, 0x780F), 0x780F);  // Purple (photo radar)
    settings_.colorWiFiIcon = sanitizeRgb565Color(preferences_.getUShort(kNvsColorWifi, 0x07FF), 0x07FF);
    settings_.colorWiFiConnected = sanitizeRgb565Color(preferences_.getUShort(kNvsColorWifiConnected, 0x07E0), 0x07E0);
    settings_.colorBleConnected = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBleConnected, 0x07E0), 0x07E0);
    settings_.colorBleDisconnected = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBleDisconnected, 0x001F), 0x001F);
    settings_.colorBar1 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar1, 0x07E0), 0x07E0);
    settings_.colorBar2 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar2, 0x07E0), 0x07E0);
    settings_.colorBar3 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar3, 0xFFE0), 0xFFE0);
    settings_.colorBar4 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar4, 0xFFE0), 0xFFE0);
    settings_.colorBar5 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar5, 0xF800), 0xF800);
    settings_.colorBar6 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar6, 0xF800), 0xF800);
    settings_.colorMuted = sanitizeRgb565Color(preferences_.getUShort(kNvsColorMuted, 0x3186), 0x3186);  // Dark grey muted color
    settings_.colorPersisted = sanitizeRgb565Color(preferences_.getUShort(kNvsColorPersisted, 0x18C3), 0x18C3);  // Darker grey for persisted alerts
    settings_.colorVolumeMain = sanitizeRgb565Color(preferences_.getUShort(kNvsColorVolumeMain, 0xF800), 0xF800);  // Red for main volume
    settings_.colorVolumeMute = sanitizeRgb565Color(preferences_.getUShort(kNvsColorVolumeMute, 0x7BEF), 0x7BEF);  // Grey for mute volume
    settings_.colorRssiV1 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorRssiV1, 0x07E0), 0x07E0);       // Green for V1 RSSI label
    settings_.colorRssiProxy = sanitizeRgb565Color(preferences_.getUShort(kNvsColorRssiProxy, 0x001F), 0x001F);   // Blue for Proxy RSSI label
    settings_.colorObd = sanitizeRgb565Color(preferences_.getUShort(kNvsColorObd, 0x001F), 0x001F);              // Blue OBD badge color
    settings_.colorAlpConnected = sanitizeRgb565Color(preferences_.getUShort(kNvsColorAlpConn, 0x07E0), 0x07E0);  // Green ALP connected
    settings_.colorAlpScan = sanitizeRgb565Color(preferences_.getUShort(kNvsColorAlpScan, 0xFD20), 0xFD20);      // Orange ALP scanning
    settings_.colorAlpArmed = sanitizeRgb565Color(preferences_.getUShort(kNvsColorAlpArmed, 0x001F), 0x001F);    // Blue ALP armed
    settings_.freqUseBandColor = preferences_.getBool(kNvsFreqBandColor, false);  // Use custom freq color by default
    settings_.hideWifiIcon = preferences_.getBool(kNvsHideWifi, false);
    settings_.hideProfileIndicator = preferences_.getBool(kNvsHideProfile, false);
    settings_.hideBatteryIcon = preferences_.getBool(kNvsHideBattery, false);
    settings_.showBatteryPercent = preferences_.getBool(kNvsBatteryPercent, false);
    settings_.hideBleIcon = preferences_.getBool(kNvsHideBle, false);
    settings_.hideVolumeIndicator = preferences_.getBool(kNvsHideVolume, false);
    settings_.hideRssiIndicator = preferences_.getBool(kNvsHideRssi, false);

    // Development settings
    settings_.enableWifiAtBoot = preferences_.getBool(kNvsWifiAtBoot, false);

    // Voice alert settings - migrate from old boolean to new mode
    // If old voiceAlerts key exists, migrate it; otherwise use new defaults
    bool needsMigration = preferences_.isKey(kNvsVoiceAlertsLegacy);
    if (needsMigration) {
        // Migrate old setting: true -> BAND_FREQ, false -> DISABLED
        bool oldEnabled = preferences_.getBool(kNvsVoiceAlertsLegacy, true);
        settings_.voiceAlertMode = oldEnabled ? VOICE_MODE_BAND_FREQ : VOICE_MODE_DISABLED;
        settings_.voiceDirectionEnabled = true;  // Old behavior always included direction
    } else {
        settings_.voiceAlertMode = clampVoiceAlertModeValue(preferences_.getUChar(kNvsVoiceMode, VOICE_MODE_BAND_FREQ));
        settings_.voiceDirectionEnabled = preferences_.getBool(kNvsVoiceDirection, true);
    }

    // Close read-only preferences_ before migration cleanup
    if (needsMigration) {
        preferences_.end();
        // Re-open in write mode to remove old key
        if (preferences_.begin(activeNs.c_str(), false)) {
            preferences_.remove(kNvsVoiceAlertsLegacy);
            Serial.println("[Settings] Migrated voiceAlerts -> voiceMode");
            preferences_.end();
        }
        // Re-open in read-only to continue loading
        if (!preferences_.begin(activeNs.c_str(), true)) {
            Serial.println("[Settings] WARN: Failed to reopen namespace after migration");
            return;
        }
    }
    settings_.announceBogeyCount = preferences_.getBool(kNvsVoiceBogeys, true);
    settings_.muteVoiceIfVolZero = preferences_.getBool(kNvsMuteVoiceAtVol0, false);
    settings_.voiceVolume = std::min<uint8_t>(100, preferences_.getUChar(kNvsVoiceVolume, 75));  // 0-100%

    // Secondary alert settings
    settings_.announceSecondaryAlerts = preferences_.getBool(kNvsSecondaryAlerts, false);
    settings_.secondaryLaser = preferences_.getBool(kNvsSecondaryLaser, true);
    settings_.secondaryKa = preferences_.getBool(kNvsSecondaryKa, true);
    settings_.secondaryK = preferences_.getBool(kNvsSecondaryK, false);
    settings_.secondaryX = preferences_.getBool(kNvsSecondaryX, false);

    // Volume fade settings
    settings_.alertVolumeFadeEnabled = preferences_.getBool(kNvsVolFadeEnabled, false);
    settings_.alertVolumeFadeDelaySec = std::clamp<uint8_t>(preferences_.getUChar(kNvsVolFadeSeconds, 2), 1, 10);  // 1-10 seconds
    settings_.alertVolumeFadeVolume = std::clamp<uint8_t>(preferences_.getUChar(kNvsVolFadeVolume, 1), 1, 9);  // 1-9 (min 1 prevents V1 mute indicator feedback loop)

    // Speed-aware muting settings
    settings_.speedMuteEnabled = preferences_.getBool(kNvsSpeedMuteEnabled, false);
    settings_.speedMuteThresholdMph = std::clamp<uint8_t>(preferences_.getUChar(kNvsSpeedMuteThreshold, 25), 5, 60);
    settings_.speedMuteHysteresisMph = std::clamp<uint8_t>(preferences_.getUChar(kNvsSpeedMuteHysteresis, 3), 1, 10);
    {
        const uint8_t raw = preferences_.getUChar(kNvsSpeedMuteVolume, 0);
        if (raw == 0xFF) {
            // Migration: old 0xFF (voice-only) → volume 0 + voice suppression on
            settings_.speedMuteVolume = 0;
            settings_.speedMuteVoice = true;
        } else {
            settings_.speedMuteVolume = (raw <= 9) ? raw : 0;
            settings_.speedMuteVoice = preferences_.getBool(kNvsSpeedMuteVoice, true);
        }
    }

    settings_.autoPushEnabled = preferences_.getBool(kNvsAutoPush, true);  // Default to enabled for profiles to work
    settings_.activeSlot = preferences_.getInt(kNvsActiveSlot, 0);
    if (settings_.activeSlot < 0 || settings_.activeSlot > 2) {
        settings_.activeSlot = 0;
    }
    settings_.slot0Name = sanitizeSlotNameValue(preferences_.getString(kNvsSlot0Name, "DEFAULT"));
    settings_.slot1Name = sanitizeSlotNameValue(preferences_.getString(kNvsSlot1Name, "HIGHWAY"));
    settings_.slot2Name = sanitizeSlotNameValue(preferences_.getString(kNvsSlot2Name, "COMFORT"));
    settings_.slot0Color = sanitizeRgb565Color(preferences_.getUShort(kNvsSlot0Color, 0x400A), 0x400A);
    settings_.slot1Color = sanitizeRgb565Color(preferences_.getUShort(kNvsSlot1Color, 0x07E0), 0x07E0);
    settings_.slot2Color = sanitizeRgb565Color(preferences_.getUShort(kNvsSlot2Color, 0x8410), 0x8410);
    settings_.slot0Volume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot0Volume, 0xFF));
    settings_.slot1Volume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot1Volume, 0xFF));
    settings_.slot2Volume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot2Volume, 0xFF));
    settings_.slot0MuteVolume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot0MuteVolume, 0xFF));
    settings_.slot1MuteVolume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot1MuteVolume, 0xFF));
    settings_.slot2MuteVolume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot2MuteVolume, 0xFF));
    settings_.slot0DarkMode = preferences_.getBool(kNvsSlot0DarkMode, false);
    settings_.slot1DarkMode = preferences_.getBool(kNvsSlot1DarkMode, false);
    settings_.slot2DarkMode = preferences_.getBool(kNvsSlot2DarkMode, false);
    settings_.slot0MuteToZero = preferences_.getBool(kNvsSlot0MuteToZero, false);
    settings_.slot1MuteToZero = preferences_.getBool(kNvsSlot1MuteToZero, false);
    settings_.slot2MuteToZero = preferences_.getBool(kNvsSlot2MuteToZero, false);
    settings_.slot0AlertPersist = std::min<uint8_t>(5, preferences_.getUChar(kNvsSlot0Persistence, 0));
    settings_.slot1AlertPersist = std::min<uint8_t>(5, preferences_.getUChar(kNvsSlot1Persistence, 0));
    settings_.slot2AlertPersist = std::min<uint8_t>(5, preferences_.getUChar(kNvsSlot2Persistence, 0));
    settings_.slot0PriorityArrow = preferences_.getBool(kNvsSlot0PriorityArrow, false);
    settings_.slot1PriorityArrow = preferences_.getBool(kNvsSlot1PriorityArrow, false);
    settings_.slot2PriorityArrow = preferences_.getBool(kNvsSlot2PriorityArrow, false);
    settings_.slot0_default.profileName = sanitizeProfileNameValue(preferences_.getString(kNvsSlot0Profile, ""));
    settings_.slot0_default.mode = normalizeV1ModeValue(preferences_.getInt(kNvsSlot0Mode, V1_MODE_UNKNOWN));
    settings_.slot1_highway.profileName = sanitizeProfileNameValue(preferences_.getString(kNvsSlot1Profile, ""));
    settings_.slot1_highway.mode = normalizeV1ModeValue(preferences_.getInt(kNvsSlot1Mode, V1_MODE_UNKNOWN));
    settings_.slot2_comfort.profileName = sanitizeProfileNameValue(preferences_.getString(kNvsSlot2Profile, ""));
    settings_.slot2_comfort.mode = normalizeV1ModeValue(preferences_.getInt(kNvsSlot2Mode, V1_MODE_UNKNOWN));
    settings_.lastV1Address = sanitizeLastV1AddressValue(preferences_.getString(kNvsLastV1Address, ""));
    settings_.autoPowerOffMinutes = clampU8(preferences_.getUChar(kNvsAutoPowerOff, 0), 0, 60);
    settings_.apTimeoutMinutes = clampApTimeoutValue(preferences_.getUChar(kNvsApTimeout, 0));

    // OBD settings
    settings_.obdEnabled = preferences_.getBool(kNvsObdEnabled, false);
    settings_.obdSavedAddress = preferences_.getString(kNvsObdAddress, "");
    if (!isValidBleAddress(settings_.obdSavedAddress)) {
        Serial.printf("[Settings] WARN: Invalid OBD saved address in NVS: '%s' — clearing\n",
                      settings_.obdSavedAddress.c_str());
        settings_.obdSavedAddress = "";
    }
    settings_.obdSavedName = sanitizeObdSavedNameValue(preferences_.getString(kNvsObdName, ""));
    settings_.obdSavedAddrType = preferences_.getUChar(kNvsObdAddressType, 0);
    {
        const int rssi = static_cast<int>(preferences_.getChar(kNvsObdMinRssi, -90));
        settings_.obdMinRssi = static_cast<int8_t>(std::max(-100, std::min(rssi, -40)));
    }

    // ALP settings
    settings_.alpEnabled = preferences_.getBool(kNvsAlpEnabled, false);
    settings_.alpSdLogEnabled = preferences_.getBool(kNvsAlpSdLog, false);

    preferences_.end();

    Serial.printf("[Settings] OK wifi=%s proxy=%s bright=%d autoPush=%s\n",
                  settings_.enableWifi ? "on" : "off",
                  settings_.proxyBLE ? "on" : "off",
                  settings_.brightness,
                  settings_.autoPushEnabled ? "on" : "off");
}

void SettingsManager::save() {
    if (!persistSettingsAtomically()) {
        return;
    }

    clearDeferredPersistState();
    bumpBackupRevision();
    Serial.println("Settings saved atomically");

    // Backup display settings to SD card (survives reflash)
    backupToSD();
}

void SettingsManager::requestDeferredPersist() {
    deferredPersistPending_ = true;
    deferredPersistRetryScheduled_ = false;
    deferredPersistNextAttemptAtMs_ = millis() + SETTINGS_DEFERRED_PERSIST_DEBOUNCE_MS;
}

bool SettingsManager::deferredPersistPending() const {
    return deferredPersistPending_;
}

bool SettingsManager::deferredPersistRetryScheduled() const {
    return deferredPersistRetryScheduled_;
}

uint32_t SettingsManager::deferredPersistNextAttemptAtMs() const {
    return deferredPersistNextAttemptAtMs_;
}

void SettingsManager::serviceDeferredPersist(uint32_t nowMs) {
    if (!deferredPersistPending_) {
        return;
    }

    if (deferredPersistNextAttemptAtMs_ != 0 &&
        !isDeferredPersistDue(nowMs, deferredPersistNextAttemptAtMs_)) {
        return;
    }

    if (!persistSettingsAtomically()) {
        deferredPersistPending_ = true;
        deferredPersistRetryScheduled_ = true;
        deferredPersistNextAttemptAtMs_ = nowMs + SETTINGS_DEFERRED_PERSIST_RETRY_BACKOFF_MS;
        return;
    }

    clearDeferredPersistState();
    bumpBackupRevision();
    Serial.println("Settings saved atomically");
    requestDeferredBackupFromCurrentState();
}

// Check if NVS appears to be in default state (likely erased during reflash)

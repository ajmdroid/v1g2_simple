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
    int storedVersion = preferences_.getInt("settingsVer", 1);

    settings_.enableWifi = preferences_.getBool("enableWifi", true);

    // Handle AP password storage - version 1 was plain text, version 2+ is obfuscated
    String storedApPwd = preferences_.getString("apPassword", "");

    if (storedVersion >= 2) {
        // Passwords are obfuscated - decode and sanitize them.
        settings_.apPassword = sanitizeApPasswordValue(
            storedApPwd.length() > 0 ? decodeObfuscatedFromStorage(storedApPwd) : "setupv1g2");
    } else {
        // Version 1 - passwords stored in plain text, use as-is then sanitize.
        settings_.apPassword = sanitizeApPasswordValue(storedApPwd.length() > 0 ? storedApPwd : "setupv1g2");
        Serial.println("[Settings] Migrating from v1 to v2 (password obfuscation)");
    }

    settings_.apSSID = sanitizeApSsidValue(preferences_.getString("apSSID", "V1-Simple"));

    // WiFi client (STA) settings
    const bool wifiClientEnabledKeyPresent = preferences_.isKey("wifiClientEn");
    const bool wifiClientSsidKeyPresent = preferences_.isKey("wifiClSSID");
    settings_.wifiClientEnabled = preferences_.getBool("wifiClientEn", false);
    settings_.wifiClientSSID = sanitizeWifiClientSsidValue(preferences_.getString("wifiClSSID", ""));

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

    settings_.proxyBLE = preferences_.getBool("proxyBLE", true);
    settings_.proxyName = sanitizeProxyNameValue(preferences_.getString("proxyName", "V1-Proxy"));
    settings_.gpsEnabled = preferences_.getBool("gpsEn", false);
    settings_.gpsLockoutMode = clampLockoutRuntimeModeValue(
        preferences_.getUChar("gpsLkMode", static_cast<uint8_t>(LOCKOUT_RUNTIME_OFF)));
    settings_.gpsLockoutCoreGuardEnabled = preferences_.getBool("gpsLkGuard", true);
    settings_.gpsLockoutMaxQueueDrops = preferences_.getUShort("gpsLkQDrop", 0);
    settings_.gpsLockoutMaxPerfDrops = preferences_.getUShort("gpsLkPDrop", 0);
    settings_.gpsLockoutMaxEventBusDrops = preferences_.getUShort("gpsLkEBDrop", 0);
    settings_.gpsLockoutLearnerPromotionHits = clampLockoutLearnerHitsValue(
        preferences_.getUChar("gpsLkHits", LOCKOUT_LEARNER_HITS_DEFAULT));
    uint16_t learnerRadiusRaw = preferences_.getUShort("gpsLkRad", LOCKOUT_LEARNER_RADIUS_E5_DEFAULT);
    // Legacy radius values were written in a 10x scale (450..3600 intended as 50..400 m).
    // Normalize in-memory during load to preserve intended behavior without immediate NVS rewrite.
    if (storedVersion <= 2 && learnerRadiusRaw >= 450) {
        const uint16_t converted = static_cast<uint16_t>((learnerRadiusRaw + 5u) / 10u);
        Serial.printf("[Settings] Migrated legacy lockout radius %u -> %u\n",
                      static_cast<unsigned>(learnerRadiusRaw),
                      static_cast<unsigned>(converted));
        learnerRadiusRaw = converted;
    }
    settings_.gpsLockoutLearnerRadiusE5 = clampLockoutLearnerRadiusE5Value(learnerRadiusRaw);
    settings_.gpsLockoutLearnerFreqToleranceMHz = clampLockoutLearnerFreqTolValue(
        preferences_.getUShort("gpsLkFtol", LOCKOUT_LEARNER_FREQ_TOL_DEFAULT));
    settings_.gpsLockoutLearnerLearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
        preferences_.getUChar("gpsLkLInt", LOCKOUT_LEARNER_LEARN_INTERVAL_HOURS_DEFAULT));
    settings_.gpsLockoutLearnerUnlearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
        preferences_.getUChar("gpsLkUInt", LOCKOUT_LEARNER_UNLEARN_INTERVAL_HOURS_DEFAULT));
    settings_.gpsLockoutLearnerUnlearnCount = clampLockoutLearnerUnlearnCountValue(
        preferences_.getUChar("gpsLkUCnt", LOCKOUT_LEARNER_UNLEARN_COUNT_DEFAULT));
    settings_.gpsLockoutManualDemotionMissCount = clampLockoutManualDemotionMissCountValue(
        preferences_.getUChar("gpsLkMDCnt", LOCKOUT_MANUAL_DEMOTION_MISS_COUNT_DEFAULT));
    settings_.gpsLockoutKaLearningEnabled = preferences_.getBool("gpsLkKa", false);
    settings_.gpsLockoutKLearningEnabled = preferences_.getBool("gpsLkK", true);
    settings_.gpsLockoutXLearningEnabled = preferences_.getBool("gpsLkX", true);
    settings_.gpsLockoutPreQuiet = preferences_.getBool("gpsLkPQ", false);
    settings_.gpsLockoutPreQuietBufferE5 = clampLockoutPreQuietBufferE5Value(
        preferences_.getUShort("gpsLkPQBuf", LOCKOUT_PRE_QUIET_BUFFER_E5_DEFAULT));
    settings_.gpsLockoutMaxHdopX10 = clampLockoutGpsMaxHdopX10Value(
        preferences_.getUShort("gpsLkHdop", LOCKOUT_GPS_MAX_HDOP_X10_DEFAULT));
    settings_.gpsLockoutMinLearnerSpeedMph = clampLockoutGpsMinLearnerSpeedMphValue(
        preferences_.getUChar("gpsLkMinSpd", LOCKOUT_GPS_MIN_LEARNER_SPEED_MPH_DEFAULT));
    settings_.turnOffDisplay = preferences_.getBool("displayOff", false);
    settings_.brightness = std::max<uint8_t>(1, preferences_.getUChar("brightness", 200));  // Min 1 to avoid blank screen
    settings_.displayStyle = normalizeDisplayStyle(preferences_.getInt("dispStyle", DISPLAY_STYLE_CLASSIC));
    settings_.colorBogey = sanitizeRgb565Color(preferences_.getUShort("colorBogey", 0xF800), 0xF800);
    settings_.colorFrequency = sanitizeRgb565Color(preferences_.getUShort("colorFreq", 0xF800), 0xF800);
    settings_.colorArrowFront = sanitizeRgb565Color(preferences_.getUShort("colorArrF", 0xF800), 0xF800);
    settings_.colorArrowSide = sanitizeRgb565Color(preferences_.getUShort("colorArrS", 0xF800), 0xF800);
    settings_.colorArrowRear = sanitizeRgb565Color(preferences_.getUShort("colorArrR", 0xF800), 0xF800);
    settings_.colorBandL = sanitizeRgb565Color(preferences_.getUShort("colorBandL", 0x001F), 0x001F);
    settings_.colorBandKa = sanitizeRgb565Color(preferences_.getUShort("colorBandKa", 0xF800), 0xF800);
    settings_.colorBandK = sanitizeRgb565Color(preferences_.getUShort("colorBandK", 0x001F), 0x001F);
    settings_.colorBandX = sanitizeRgb565Color(preferences_.getUShort("colorBandX", 0x07E0), 0x07E0);
    settings_.colorBandPhoto = sanitizeRgb565Color(preferences_.getUShort("colorBandP", 0x780F), 0x780F);  // Purple (photo radar)
    settings_.colorWiFiIcon = sanitizeRgb565Color(preferences_.getUShort("colorWiFi", 0x07FF), 0x07FF);
    settings_.colorWiFiConnected = sanitizeRgb565Color(preferences_.getUShort("colorWiFiC", 0x07E0), 0x07E0);
    settings_.colorBleConnected = sanitizeRgb565Color(preferences_.getUShort("colorBleC", 0x07E0), 0x07E0);
    settings_.colorBleDisconnected = sanitizeRgb565Color(preferences_.getUShort("colorBleD", 0x001F), 0x001F);
    settings_.colorBar1 = sanitizeRgb565Color(preferences_.getUShort("colorBar1", 0x07E0), 0x07E0);
    settings_.colorBar2 = sanitizeRgb565Color(preferences_.getUShort("colorBar2", 0x07E0), 0x07E0);
    settings_.colorBar3 = sanitizeRgb565Color(preferences_.getUShort("colorBar3", 0xFFE0), 0xFFE0);
    settings_.colorBar4 = sanitizeRgb565Color(preferences_.getUShort("colorBar4", 0xFFE0), 0xFFE0);
    settings_.colorBar5 = sanitizeRgb565Color(preferences_.getUShort("colorBar5", 0xF800), 0xF800);
    settings_.colorBar6 = sanitizeRgb565Color(preferences_.getUShort("colorBar6", 0xF800), 0xF800);
    settings_.colorMuted = sanitizeRgb565Color(preferences_.getUShort("colorMuted", 0x3186), 0x3186);  // Dark grey muted color
    settings_.colorPersisted = sanitizeRgb565Color(preferences_.getUShort("colorPersist", 0x18C3), 0x18C3);  // Darker grey for persisted alerts
    settings_.colorVolumeMain = sanitizeRgb565Color(preferences_.getUShort("colorVolMain", 0xF800), 0xF800);  // Red for main volume
    settings_.colorVolumeMute = sanitizeRgb565Color(preferences_.getUShort("colorVolMute", 0x7BEF), 0x7BEF);  // Grey for mute volume
    settings_.colorRssiV1 = sanitizeRgb565Color(preferences_.getUShort("colorRssiV1", 0x07E0), 0x07E0);       // Green for V1 RSSI label
    settings_.colorRssiProxy = sanitizeRgb565Color(preferences_.getUShort("colorRssiPrx", 0x001F), 0x001F);   // Blue for Proxy RSSI label
    settings_.colorLockout = sanitizeRgb565Color(preferences_.getUShort("colorLockL", 0x07E0), 0x07E0);        // Green lockout badge color
    settings_.colorGps = sanitizeRgb565Color(preferences_.getUShort("colorGps", 0x07FF), 0x07FF);              // Cyan GPS badge color
    settings_.colorObd = sanitizeRgb565Color(preferences_.getUShort("colorObd", 0x001F), 0x001F);              // Blue OBD badge color
    settings_.freqUseBandColor = preferences_.getBool("freqBandCol", false);  // Use custom freq color by default
    settings_.hideWifiIcon = preferences_.getBool("hideWifi", false);
    settings_.hideProfileIndicator = preferences_.getBool("hideProfile", false);
    settings_.hideBatteryIcon = preferences_.getBool("hideBatt", false);
    settings_.showBatteryPercent = preferences_.getBool("battPct", false);
    settings_.hideBleIcon = preferences_.getBool("hideBle", false);
    settings_.hideVolumeIndicator = preferences_.getBool("hideVol", false);
    settings_.hideRssiIndicator = preferences_.getBool("hideRssi", false);

    // Development settings
    settings_.enableWifiAtBoot = preferences_.getBool("wifiAtBoot", false);
    settings_.enableSignalTraceLogging = preferences_.getBool("sigTraceLog", true);

    // Voice alert settings - migrate from old boolean to new mode
    // If old voiceAlerts key exists, migrate it; otherwise use new defaults
    bool needsMigration = preferences_.isKey("voiceAlerts");
    if (needsMigration) {
        // Migrate old setting: true -> BAND_FREQ, false -> DISABLED
        bool oldEnabled = preferences_.getBool("voiceAlerts", true);
        settings_.voiceAlertMode = oldEnabled ? VOICE_MODE_BAND_FREQ : VOICE_MODE_DISABLED;
        settings_.voiceDirectionEnabled = true;  // Old behavior always included direction
    } else {
        settings_.voiceAlertMode = clampVoiceAlertModeValue(preferences_.getUChar("voiceMode", VOICE_MODE_BAND_FREQ));
        settings_.voiceDirectionEnabled = preferences_.getBool("voiceDir", true);
    }

    // Close read-only preferences_ before migration cleanup
    if (needsMigration) {
        preferences_.end();
        // Re-open in write mode to remove old key
        if (preferences_.begin(activeNs.c_str(), false)) {
            preferences_.remove("voiceAlerts");
            Serial.println("[Settings] Migrated voiceAlerts -> voiceMode");
            preferences_.end();
        }
        // Re-open in read-only to continue loading
        if (!preferences_.begin(activeNs.c_str(), true)) {
            Serial.println("[Settings] WARN: Failed to reopen namespace after migration");
            return;
        }
    }
    settings_.announceBogeyCount = preferences_.getBool("voiceBogeys", true);
    settings_.muteVoiceIfVolZero = preferences_.getBool("muteVoiceVol0", false);
    settings_.voiceVolume = std::min<uint8_t>(100, preferences_.getUChar("voiceVol", 75));  // 0-100%

    // Secondary alert settings
    settings_.announceSecondaryAlerts = preferences_.getBool("secAlerts", false);
    settings_.secondaryLaser = preferences_.getBool("secLaser", true);
    settings_.secondaryKa = preferences_.getBool("secKa", true);
    settings_.secondaryK = preferences_.getBool("secK", false);
    settings_.secondaryX = preferences_.getBool("secX", false);

    // Volume fade settings
    settings_.alertVolumeFadeEnabled = preferences_.getBool("volFadeEn", false);
    settings_.alertVolumeFadeDelaySec = std::clamp<uint8_t>(preferences_.getUChar("volFadeSec", 2), 1, 10);  // 1-10 seconds
    settings_.alertVolumeFadeVolume = std::min<uint8_t>(9, preferences_.getUChar("volFadeVol", 1));  // 0-9 (V1 volume range)

    // Speed-aware muting settings
    settings_.speedMuteEnabled = preferences_.getBool("spdMuteEn", false);
    settings_.speedMuteThresholdMph = std::clamp<uint8_t>(preferences_.getUChar("spdMuteThr", 25), 5, 60);
    settings_.speedMuteHysteresisMph = std::clamp<uint8_t>(preferences_.getUChar("spdMuteHys", 3), 1, 10);
    {
        const uint8_t raw = preferences_.getUChar("spdMuteVol", 0xFF);
        settings_.speedMuteVolume = (raw <= 9 || raw == 0xFF) ? raw : 0xFF;
    }
    settings_.speedMuteRequireObd = preferences_.getBool("spdMuteObd", false);

    settings_.autoPushEnabled = preferences_.getBool("autoPush", true);  // Default to enabled for profiles to work
    settings_.activeSlot = preferences_.getInt("activeSlot", 0);
    if (settings_.activeSlot < 0 || settings_.activeSlot > 2) {
        settings_.activeSlot = 0;
    }
    settings_.slot0Name = sanitizeSlotNameValue(preferences_.getString("slot0name", "DEFAULT"));
    settings_.slot1Name = sanitizeSlotNameValue(preferences_.getString("slot1name", "HIGHWAY"));
    settings_.slot2Name = sanitizeSlotNameValue(preferences_.getString("slot2name", "COMFORT"));
    settings_.slot0Color = sanitizeRgb565Color(preferences_.getUShort("slot0color", 0x400A), 0x400A);
    settings_.slot1Color = sanitizeRgb565Color(preferences_.getUShort("slot1color", 0x07E0), 0x07E0);
    settings_.slot2Color = sanitizeRgb565Color(preferences_.getUShort("slot2color", 0x8410), 0x8410);
    settings_.slot0Volume = clampSlotVolumeValue(preferences_.getUChar("slot0vol", 0xFF));
    settings_.slot1Volume = clampSlotVolumeValue(preferences_.getUChar("slot1vol", 0xFF));
    settings_.slot2Volume = clampSlotVolumeValue(preferences_.getUChar("slot2vol", 0xFF));
    settings_.slot0MuteVolume = clampSlotVolumeValue(preferences_.getUChar("slot0mute", 0xFF));
    settings_.slot1MuteVolume = clampSlotVolumeValue(preferences_.getUChar("slot1mute", 0xFF));
    settings_.slot2MuteVolume = clampSlotVolumeValue(preferences_.getUChar("slot2mute", 0xFF));
    settings_.slot0DarkMode = preferences_.getBool("slot0dark", false);
    settings_.slot1DarkMode = preferences_.getBool("slot1dark", false);
    settings_.slot2DarkMode = preferences_.getBool("slot2dark", false);
    settings_.slot0MuteToZero = preferences_.getBool("slot0mz", false);
    settings_.slot1MuteToZero = preferences_.getBool("slot1mz", false);
    settings_.slot2MuteToZero = preferences_.getBool("slot2mz", false);
    settings_.slot0AlertPersist = std::min<uint8_t>(5, preferences_.getUChar("slot0persist", 0));
    settings_.slot1AlertPersist = std::min<uint8_t>(5, preferences_.getUChar("slot1persist", 0));
    settings_.slot2AlertPersist = std::min<uint8_t>(5, preferences_.getUChar("slot2persist", 0));
    settings_.slot0PriorityArrow = preferences_.getBool("slot0prio", false);
    settings_.slot1PriorityArrow = preferences_.getBool("slot1prio", false);
    settings_.slot2PriorityArrow = preferences_.getBool("slot2prio", false);
    settings_.slot0_default.profileName = sanitizeProfileNameValue(preferences_.getString("slot0prof", ""));
    settings_.slot0_default.mode = normalizeV1ModeValue(preferences_.getInt("slot0mode", V1_MODE_UNKNOWN));
    settings_.slot1_highway.profileName = sanitizeProfileNameValue(preferences_.getString("slot1prof", ""));
    settings_.slot1_highway.mode = normalizeV1ModeValue(preferences_.getInt("slot1mode", V1_MODE_UNKNOWN));
    settings_.slot2_comfort.profileName = sanitizeProfileNameValue(preferences_.getString("slot2prof", ""));
    settings_.slot2_comfort.mode = normalizeV1ModeValue(preferences_.getInt("slot2mode", V1_MODE_UNKNOWN));
    settings_.lastV1Address = sanitizeLastV1AddressValue(preferences_.getString("lastV1Addr", ""));
    settings_.autoPowerOffMinutes = clampU8(preferences_.getUChar("autoPwrOff", 0), 0, 60);
    settings_.apTimeoutMinutes = clampApTimeoutValue(preferences_.getUChar("apTimeout", 0));

    // OBD settings
    settings_.obdEnabled = preferences_.getBool("obdEn", false);
    settings_.obdSavedAddress = preferences_.getString("obdAddr", "");
    settings_.obdSavedName = sanitizeObdSavedNameValue(preferences_.getString("obdName", ""));
    settings_.obdSavedAddrType = preferences_.getUChar("obdAddrT", 0);
    settings_.obdMinRssi = static_cast<int8_t>(
        preferences_.getChar("obdMinRssi", -90));
    settings_.obdCachedVinPrefix11 = preferences_.getString("obdVin11", "");
    settings_.obdCachedEotProfileId = preferences_.getUChar("obdEotPid", 0);

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

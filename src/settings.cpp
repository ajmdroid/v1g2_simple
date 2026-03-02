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
const int SD_BACKUP_VERSION = 9;  // Increment when adding new fields to backup
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
const int SETTINGS_VERSION = 6;  // Increment when changing persisted settings schema
const char* OBFUSCATION_HEX_PREFIX = "hex:";


// --- NVS persistence helpers (attemptNvsRecovery, XOR crypto chain, WiFi SD secrets,
//     namespaceHealthScore, getActive/StagingNamespace, writeSettingsToNamespace,
//     persistSettingsAtomically) moved to settings_nvs.cpp ---

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
    const bool wifiClientEnabledKeyPresent = preferences.isKey("wifiClientEn");
    const bool wifiClientSsidKeyPresent = preferences.isKey("wifiClSSID");
    settings.wifiClientEnabled = preferences.getBool("wifiClientEn", false);
    settings.wifiClientSSID = sanitizeWifiClientSsidValue(preferences.getString("wifiClSSID", ""));

    // Self-healing: if a saved SSID exists, force wifiClientEnabled to true.
    // This covers cases where a backup restore set the SSID but wifiClientEnabled
    // was missing from the JSON, or where the two got out of sync.
    // Mirrors setWifiClientCredentials() which derives enabled from SSID length.
    if (!settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0) {
        Serial.println("[Settings] HEAL: wifiClientEnabled was false but SSID is set — enabling");
        settings.wifiClientEnabled = true;
    }

    // Determine WiFi mode based on client enabled state
    settings.wifiMode = settings.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;
    
    // Debug: Log WiFi client settings on load
    Serial.printf("[Settings] WiFi client keys: enabledKey=%s ssidKey=%s\n",
                  wifiClientEnabledKeyPresent ? "yes" : "no",
                  wifiClientSsidKeyPresent ? "yes" : "no");
    Serial.printf("[Settings] WiFi client: enabled=%s, SSID='%s'\n",
                  settings.wifiClientEnabled ? "true" : "false",
                  settings.wifiClientSSID.c_str());
    
    settings.proxyBLE = preferences.getBool("proxyBLE", true);
    settings.proxyName = sanitizeProxyNameValue(preferences.getString("proxyName", "V1-Proxy"));
    settings.obdEnabled = preferences.getBool("obdEn", false);
    settings.obdVwDataEnabled = preferences.getBool("obdVwData", true);
    settings.gpsEnabled = preferences.getBool("gpsEn", false);
    settings.cameraEnabled = preferences.getBool("camEn", true);
    settings.cameraAlertDistanceFt = clampCameraAlertDistanceFtValue(
        preferences.getUShort("camAlertFt", CAMERA_ALERT_DISTANCE_FT_DEFAULT));
    settings.cameraAlertPersistSec = clampCameraAlertPersistSecValue(
        preferences.getUChar("camAlertSec", CAMERA_ALERT_PERSIST_SEC_DEFAULT));
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
    settings.showRestTelemetryCards = preferences.getBool("restTelem", false);
    
    // Development settings
    settings.enableWifiAtBoot = preferences.getBool("wifiAtBoot", false);
    settings.enableSignalTraceLogging = preferences.getBool("sigTraceLog", true);
    
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


// --- WiFi client methods (getWifiClientPassword, setWifiClientEnabled,
//     setWifiClientCredentials, clearWifiClientCredentials) moved to settings_nvs.cpp ---
// --- Property setters (setProxyBLE through setLastV1Address), slot getters/setters,
//     resetToDefaults moved to settings_setters.cpp ---
// Check if NVS appears to be in default state (likely erased during reflash)

// --- checkNeedsRestore, backupToSD, restoreFromSD, validateProfileReferences
//     moved to settings_backup.cpp ---

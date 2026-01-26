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
static const int SD_BACKUP_VERSION = 2;  // Increment when adding new fields to backup
static const char* SETTINGS_NS_A = "v1settingsA";
static const char* SETTINGS_NS_B = "v1settingsB";
static const char* SETTINGS_NS_META = "v1settingsMeta";
static const char* SETTINGS_NS_LEGACY = "v1settings";

// Global instance
SettingsManager settingsManager;

// XOR obfuscation key - deters casual reading but NOT cryptographically secure
// See security note above for rationale
static const char XOR_KEY[] = "V1G2-S3cr3t-K3y!";
static const int SETTINGS_VERSION = 2;  // Increment when changing password encoding

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
    written += prefs.putString("apPassword", xorObfuscate(settings.apPassword));
    written += prefs.putBool("proxyBLE", settings.proxyBLE);
    written += prefs.putString("proxyName", settings.proxyName);
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
    written += prefs.putUShort("colorStGps", settings.colorStatusGps);
    written += prefs.putUShort("colorStGpsW", settings.colorStatusGpsWarn);
    written += prefs.putUShort("colorStCam", settings.colorStatusCam);
    written += prefs.putUShort("colorStObd", settings.colorStatusObd);
    written += prefs.putBool("freqBandCol", settings.freqUseBandColor);
    written += prefs.putBool("hideWifi", settings.hideWifiIcon);
    written += prefs.putBool("hideProfile", settings.hideProfileIndicator);
    written += prefs.putBool("hideBatt", settings.hideBatteryIcon);
    written += prefs.putBool("battPct", settings.showBatteryPercent);
    written += prefs.putBool("hideBle", settings.hideBleIcon);
    written += prefs.putBool("hideVol", settings.hideVolumeIndicator);
    written += prefs.putBool("hideRssi", settings.hideRssiIndicator);
    written += prefs.putBool("kittScanner", settings.kittScannerEnabled);
    written += prefs.putBool("wifiAtBoot", settings.enableWifiAtBoot);
    written += prefs.putBool("debugLog", settings.enableDebugLogging);
    written += prefs.putBool("logAlerts", settings.logAlerts);
    written += prefs.putBool("logWifi", settings.logWifi);
    written += prefs.putBool("logBle", settings.logBle);
    written += prefs.putBool("logGps", settings.logGps);
    written += prefs.putBool("logObd", settings.logObd);
    written += prefs.putBool("logSystem", settings.logSystem);
    written += prefs.putBool("logDisplay", settings.logDisplay);
    written += prefs.putBool("logPerfMet", settings.logPerfMetrics);
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
    written += prefs.putBool("gpsEnabled", settings.gpsEnabled);
    written += prefs.putBool("obdEnabled", settings.obdEnabled);
    written += prefs.putString("obdAddr", settings.obdDeviceAddress);
    written += prefs.putString("obdName", settings.obdDeviceName);
    written += prefs.putString("obdPin", settings.obdPin);
    written += prefs.putBool("lkoutEn", settings.lockoutEnabled);
    written += prefs.putBool("lkoutKaProt", settings.lockoutKaProtection);
    written += prefs.putBool("lkoutDirUnl", settings.lockoutDirectionalUnlearn);
    written += prefs.putUShort("lkoutFreqTol", settings.lockoutFreqToleranceMHz);
    written += prefs.putUChar("lkoutLearnCt", settings.lockoutLearnCount);
    written += prefs.putUChar("lkoutUnlCt", settings.lockoutUnlearnCount);
    written += prefs.putUChar("lkoutManDel", settings.lockoutManualDeleteCount);
    written += prefs.putUChar("lkoutLearnHr", settings.lockoutLearnIntervalHours);
    written += prefs.putUChar("lkoutUnlHr", settings.lockoutUnlearnIntervalHours);
    written += prefs.putUChar("lkoutMaxSig", settings.lockoutMaxSignalStrength);
    written += prefs.putUShort("lkoutMaxDist", settings.lockoutMaxDistanceM);
    
    // Camera alerts
    written += prefs.putBool("camEnabled", settings.cameraAlertsEnabled);
    written += prefs.putUShort("camAlertDist", settings.cameraAlertDistanceM);
    written += prefs.putBool("camRedLight", settings.cameraAlertRedLight);
    written += prefs.putBool("camSpeed", settings.cameraAlertSpeed);
    written += prefs.putBool("camALPR", settings.cameraAlertALPR);
    written += prefs.putBool("camAudio", settings.cameraAudioEnabled);
    written += prefs.putUShort("camColor", settings.colorCameraAlert);

    prefs.end();
    Serial.printf("[Settings] Wrote %d bytes to namespace %s\n", written, ns);
    return true;
}

bool SettingsManager::persistSettingsAtomically() {
    String activeNs = getActiveNamespace();
    String stagingNs = getStagingNamespace(activeNs);

    if (!writeSettingsToNamespace(stagingNs.c_str())) {
        Serial.println("[Settings] ERROR: Failed to write staging settings");
        return false;
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
    load();
    
    // Note: SD card may not be mounted yet during begin().
    // checkAndRestoreFromSD() should be called after storage is ready.
    // We still try here in case storage was already initialized.
    checkAndRestoreFromSD();
}

bool SettingsManager::checkAndRestoreFromSD() {
    // Check if NVS was erased (appears default) and backup exists on SD
    // This can be called after storage is mounted to retry the restore
    if (checkNeedsRestore()) {
        Serial.println("[Settings] NVS appears default, checking for SD backup...");
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
    settings.wifiMode = V1_WIFI_AP;  // Always AP-only mode
    
    // Handle AP password storage - version 1 was plain text, version 2+ is obfuscated
    String storedApPwd = preferences.getString("apPassword", "");
    
    if (storedVersion >= 2) {
        // Passwords are obfuscated - decode them
        settings.apPassword = storedApPwd.length() > 0 ? xorObfuscate(storedApPwd) : "setupv1g2";
    } else {
        // Version 1 - passwords stored in plain text, use as-is
        settings.apPassword = storedApPwd.length() > 0 ? storedApPwd : "setupv1g2";
        Serial.println("[Settings] Migrating from v1 to v2 (password obfuscation)");
    }
    
    settings.apSSID = preferences.getString("apSSID", "V1-Simple");
    
    settings.proxyBLE = preferences.getBool("proxyBLE", true);
    settings.proxyName = preferences.getString("proxyName", "V1-Proxy");
    settings.turnOffDisplay = preferences.getBool("displayOff", false);
    settings.brightness = preferences.getUChar("brightness", 200);
    settings.displayStyle = static_cast<DisplayStyle>(preferences.getInt("dispStyle", DISPLAY_STYLE_CLASSIC));
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
    settings.colorStatusGps = preferences.getUShort("colorStGps", 0x07E0);     // Green for GPS good
    settings.colorStatusGpsWarn = preferences.getUShort("colorStGpsW", 0xFD20); // Orange for GPS weak
    settings.colorStatusCam = preferences.getUShort("colorStCam", 0x07FF);     // Cyan for CAM
    settings.colorStatusObd = preferences.getUShort("colorStObd", 0x07E0);     // Green for OBD
    settings.freqUseBandColor = preferences.getBool("freqBandCol", false);  // Use custom freq color by default
    settings.hideWifiIcon = preferences.getBool("hideWifi", false);
    settings.hideProfileIndicator = preferences.getBool("hideProfile", false);
    settings.hideBatteryIcon = preferences.getBool("hideBatt", false);
    settings.showBatteryPercent = preferences.getBool("battPct", false);
    settings.hideBleIcon = preferences.getBool("hideBle", false);
    settings.hideVolumeIndicator = preferences.getBool("hideVol", false);
    settings.hideRssiIndicator = preferences.getBool("hideRssi", false);
    settings.kittScannerEnabled = preferences.getBool("kittScanner", false);
    
    // Development/Debug settings
    settings.enableWifiAtBoot = preferences.getBool("wifiAtBoot", false);
    settings.enableDebugLogging = preferences.getBool("debugLog", false);
    settings.logAlerts = preferences.getBool("logAlerts", true);
    settings.logWifi = preferences.getBool("logWifi", true);
    settings.logBle = preferences.getBool("logBle", false);
    settings.logGps = preferences.getBool("logGps", false);
    settings.logObd = preferences.getBool("logObd", false);
    settings.logSystem = preferences.getBool("logSystem", true);
    settings.logDisplay = preferences.getBool("logDisplay", false);
    settings.logPerfMetrics = preferences.getBool("logPerfMet", false);
    
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
    settings.voiceVolume = preferences.getUChar("voiceVol", 75);
    
    // Secondary alert settings
    settings.announceSecondaryAlerts = preferences.getBool("secAlerts", false);
    settings.secondaryLaser = preferences.getBool("secLaser", true);
    settings.secondaryKa = preferences.getBool("secKa", true);
    settings.secondaryK = preferences.getBool("secK", false);
    settings.secondaryX = preferences.getBool("secX", false);
    
    // Volume fade settings
    settings.alertVolumeFadeEnabled = preferences.getBool("volFadeEn", false);
    settings.alertVolumeFadeDelaySec = preferences.getUChar("volFadeSec", 2);
    settings.alertVolumeFadeVolume = preferences.getUChar("volFadeVol", 1);
    
    // Speed-based volume settings
    settings.speedVolumeEnabled = preferences.getBool("spdVolEn", false);
    settings.speedVolumeThresholdMph = preferences.getUChar("spdVolThr", 45);
    settings.speedVolumeBoost = preferences.getUChar("spdVolBoost", 2);
    
    // Low-speed mute settings
    settings.lowSpeedMuteEnabled = preferences.getBool("lowSpdMute", false);
    settings.lowSpeedMuteThresholdMph = preferences.getUChar("lowSpdThr", 5);
    
    settings.autoPushEnabled = preferences.getBool("autoPush", false);
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
    settings.gpsEnabled = preferences.getBool("gpsEnabled", false);  // Default: off (opt-in)
    settings.obdEnabled = preferences.getBool("obdEnabled", false);  // Default: off (opt-in)
    settings.obdDeviceAddress = preferences.getString("obdAddr", "");
    settings.obdDeviceName = preferences.getString("obdName", "");
    settings.obdPin = preferences.getString("obdPin", "1234");
    
    // Auto-lockout settings (JBV1-style)
    settings.lockoutEnabled = preferences.getBool("lkoutEn", true);
    settings.lockoutKaProtection = preferences.getBool("lkoutKaProt", true);
    settings.lockoutDirectionalUnlearn = preferences.getBool("lkoutDirUnl", true);
    settings.lockoutFreqToleranceMHz = preferences.getUShort("lkoutFreqTol", 8);
    settings.lockoutLearnCount = preferences.getUChar("lkoutLearnCt", 3);
    settings.lockoutUnlearnCount = preferences.getUChar("lkoutUnlCt", 5);
    settings.lockoutManualDeleteCount = preferences.getUChar("lkoutManDel", 25);
    settings.lockoutLearnIntervalHours = preferences.getUChar("lkoutLearnHr", 4);
    settings.lockoutUnlearnIntervalHours = preferences.getUChar("lkoutUnlHr", 4);
    settings.lockoutMaxSignalStrength = preferences.getUChar("lkoutMaxSig", 0);
    settings.lockoutMaxDistanceM = preferences.getUShort("lkoutMaxDist", 600);
    
    // Camera alerts
    settings.cameraAlertsEnabled = preferences.getBool("camEnabled", true);
    settings.cameraAlertDistanceM = preferences.getUShort("camAlertDist", 500);
    settings.cameraAlertRedLight = preferences.getBool("camRedLight", true);
    settings.cameraAlertSpeed = preferences.getBool("camSpeed", true);
    settings.cameraAlertALPR = preferences.getBool("camALPR", true);
    settings.cameraAudioEnabled = preferences.getBool("camAudio", true);
    settings.colorCameraAlert = preferences.getUShort("camColor", 0xFD20);
    
    preferences.end();
    
    Serial.println("Settings loaded:");
    Serial.printf("  WiFi enabled: %s\n", settings.enableWifi ? "yes" : "no");
    Serial.printf("  AP SSID: %s\n", settings.apSSID.c_str());
    // Note: Passwords not logged for security
    Serial.printf("  BLE proxy: %s\n", settings.proxyBLE ? "yes" : "no");
    Serial.printf("  Proxy name: %s\n", settings.proxyName.c_str());
    Serial.printf("  Brightness: %d\n", settings.brightness);
    Serial.printf("  Auto-push: %s (active slot: %d)\n", settings.autoPushEnabled ? "yes" : "no", settings.activeSlot);
    Serial.printf("  Slot0: %s (mode %d) darkMode=%s MZ=%s persist=%ds\n", settings.slot0_default.profileName.c_str(), settings.slot0_default.mode, settings.slot0DarkMode ? "yes" : "no", settings.slot0MuteToZero ? "yes" : "no", settings.slot0AlertPersist);
    Serial.printf("  Slot1: %s (mode %d) darkMode=%s MZ=%s persist=%ds\n", settings.slot1_highway.profileName.c_str(), settings.slot1_highway.mode, settings.slot1DarkMode ? "yes" : "no", settings.slot1MuteToZero ? "yes" : "no", settings.slot1AlertPersist);
    Serial.printf("  Slot2: %s (mode %d) darkMode=%s MZ=%s persist=%ds\n", settings.slot2_comfort.profileName.c_str(), settings.slot2_comfort.mode, settings.slot2DarkMode ? "yes" : "no", settings.slot2MuteToZero ? "yes" : "no", settings.slot2AlertPersist);
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

void SettingsManager::setProxyBLE(bool enabled) {
    settings.proxyBLE = enabled;
    save();
}

void SettingsManager::setProxyName(const String& name) {
    settings.proxyName = name;
    save();
}

void SettingsManager::setAutoPowerOffMinutes(uint8_t minutes) {
    settings.autoPowerOffMinutes = minutes;
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
                                        uint16_t bandL, uint16_t bandKa, uint16_t bandK, uint16_t bandX) {
    settings.colorBogey = bogey;
    settings.colorFrequency = freq;
    settings.colorArrowFront = arrowFront;
    settings.colorArrowSide = arrowSide;
    settings.colorArrowRear = arrowRear;
    settings.colorBandL = bandL;
    settings.colorBandKa = bandKa;
    settings.colorBandK = bandK;
    settings.colorBandX = bandX;
    save();
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

void SettingsManager::setStatusGpsColor(uint16_t color) {
    settings.colorStatusGps = color;
    save();
}

void SettingsManager::setStatusGpsWarnColor(uint16_t color) {
    settings.colorStatusGpsWarn = color;
    save();
}

void SettingsManager::setStatusCamColor(uint16_t color) {
    settings.colorStatusCam = color;
    save();
}

void SettingsManager::setStatusObdColor(uint16_t color) {
    settings.colorStatusObd = color;
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

void SettingsManager::setKittScannerEnabled(bool enabled) {
    settings.kittScannerEnabled = enabled;
    save();
}

void SettingsManager::setEnableWifiAtBoot(bool enable) {
    settings.enableWifiAtBoot = enable;
    save();
}

void SettingsManager::setEnableDebugLogging(bool enable) {
    settings.enableDebugLogging = enable;
    save();
}

void SettingsManager::setLogAlerts(bool enable) {
    settings.logAlerts = enable;
    save();
}

void SettingsManager::setLogWifi(bool enable) {
    settings.logWifi = enable;
    save();
}

void SettingsManager::setLogBle(bool enable) {
    settings.logBle = enable;
    save();
}

void SettingsManager::setLogGps(bool enable) {
    settings.logGps = enable;
    save();
}

void SettingsManager::setLogObd(bool enable) {
    settings.logObd = enable;
    save();
}

void SettingsManager::setLogSystem(bool enable) {
    settings.logSystem = enable;
    save();
}

void SettingsManager::setLogDisplay(bool enable) {
    settings.logDisplay = enable;
    save();
}

void SettingsManager::setLogPerfMetrics(bool enable) {
    settings.logPerfMetrics = enable;
    save();
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
    // If brightness is default (200) AND all colors are default, NVS was likely erased
    // We check multiple values to reduce false positives
    // Must check BOTH display settings AND slot settings - user may have customized
    // slots but not colors (or vice versa)
    
    // If ANY slot has a non-default profile name or mode, NVS has real data
    bool slotsAreDefault = 
        settings.slot0_default.profileName.isEmpty() &&
        settings.slot0_default.mode == V1_MODE_UNKNOWN &&
        settings.slot1_highway.profileName.isEmpty() &&
        settings.slot1_highway.mode == V1_MODE_UNKNOWN &&
        settings.slot2_comfort.profileName.isEmpty() &&
        settings.slot2_comfort.mode == V1_MODE_UNKNOWN &&
        settings.slot0DarkMode == false &&
        settings.slot1DarkMode == false &&
        settings.slot2DarkMode == false &&
        settings.slot0AlertPersist == 0 &&
        settings.slot1AlertPersist == 0 &&
        settings.slot2AlertPersist == 0;
    
    bool colorsAreDefault = 
        settings.brightness == 200 &&
        settings.colorBogey == 0xF800 &&
        settings.colorBandL == 0x001F &&
        settings.colorBar1 == 0x07E0 &&
        settings.hideWifiIcon == false &&
        settings.hideProfileIndicator == false &&
        settings.hideBatteryIcon == false;
    
    // Only restore if BOTH slots AND colors are at defaults
    // If either has been customized, NVS has real user data
    return slotsAreDefault && colorsAreDefault;
}

// Backup display/color settings to SD card
void SettingsManager::backupToSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return;  // SD not available, skip silently
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
    doc["apSSID"] = settings.apSSID;
    doc["proxyBLE"] = settings.proxyBLE;
    doc["proxyName"] = settings.proxyName;
    doc["lastV1Address"] = settings.lastV1Address;
    doc["autoPowerOffMinutes"] = settings.autoPowerOffMinutes;
    
    // === GPS/OBD Settings ===
    doc["gpsEnabled"] = settings.gpsEnabled;
    doc["obdEnabled"] = settings.obdEnabled;
    doc["obdDeviceAddress"] = settings.obdDeviceAddress;
    doc["obdDeviceName"] = settings.obdDeviceName;
    doc["obdPin"] = settings.obdPin;
    
    // === Auto-Lockout Settings (JBV1-style) ===
    doc["lockoutEnabled"] = settings.lockoutEnabled;
    doc["lockoutKaProtection"] = settings.lockoutKaProtection;
    doc["lockoutDirectionalUnlearn"] = settings.lockoutDirectionalUnlearn;
    doc["lockoutFreqToleranceMHz"] = settings.lockoutFreqToleranceMHz;
    doc["lockoutLearnCount"] = settings.lockoutLearnCount;
    doc["lockoutUnlearnCount"] = settings.lockoutUnlearnCount;
    doc["lockoutManualDeleteCount"] = settings.lockoutManualDeleteCount;
    doc["lockoutLearnIntervalHours"] = settings.lockoutLearnIntervalHours;
    doc["lockoutUnlearnIntervalHours"] = settings.lockoutUnlearnIntervalHours;
    doc["lockoutMaxSignalStrength"] = settings.lockoutMaxSignalStrength;
    doc["lockoutMaxDistanceM"] = settings.lockoutMaxDistanceM;
    
    // === Camera Alert Settings ===
    doc["cameraAlertsEnabled"] = settings.cameraAlertsEnabled;
    doc["cameraAlertDistanceM"] = settings.cameraAlertDistanceM;
    doc["cameraAlertRedLight"] = settings.cameraAlertRedLight;
    doc["cameraAlertSpeed"] = settings.cameraAlertSpeed;
    doc["cameraAlertALPR"] = settings.cameraAlertALPR;
    doc["cameraAudioEnabled"] = settings.cameraAudioEnabled;
    doc["colorCameraAlert"] = settings.colorCameraAlert;
    
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
    doc["colorStatusGps"] = settings.colorStatusGps;
    doc["colorStatusGpsWarn"] = settings.colorStatusGpsWarn;
    doc["colorStatusCam"] = settings.colorStatusCam;
    doc["colorStatusObd"] = settings.colorStatusObd;
    doc["freqUseBandColor"] = settings.freqUseBandColor;
    
    // === UI Toggle Settings ===
    doc["hideWifiIcon"] = settings.hideWifiIcon;
    doc["hideProfileIndicator"] = settings.hideProfileIndicator;
    doc["hideBatteryIcon"] = settings.hideBatteryIcon;
    doc["showBatteryPercent"] = settings.showBatteryPercent;
    doc["hideBleIcon"] = settings.hideBleIcon;
    doc["hideVolumeIndicator"] = settings.hideVolumeIndicator;
    doc["hideRssiIndicator"] = settings.hideRssiIndicator;
    doc["kittScannerEnabled"] = settings.kittScannerEnabled;
    doc["enableWifiAtBoot"] = settings.enableWifiAtBoot;
    doc["enableDebugLogging"] = settings.enableDebugLogging;
    doc["logAlerts"] = settings.logAlerts;
    doc["logWifi"] = settings.logWifi;
    doc["logBle"] = settings.logBle;
    doc["logGps"] = settings.logGps;
    doc["logObd"] = settings.logObd;
    doc["logSystem"] = settings.logSystem;
    doc["logDisplay"] = settings.logDisplay;
    doc["logPerfMetrics"] = settings.logPerfMetrics;
    
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
    
    // Write to file
    File file = fs->open(SETTINGS_BACKUP_PATH, FILE_WRITE);
    if (!file) {
        Serial.println("[Settings] Failed to create SD backup file");
        return;
    }
    
    serializeJson(doc, file);
    file.flush();
    file.close();
    
    Serial.println("[Settings] Full backup saved to SD card");
    Serial.printf("[Settings] Backed up: slot0Mode=%d, slot1Mode=%d, slot2Mode=%d\n",
                  settings.slot0_default.mode, settings.slot1_highway.mode, settings.slot2_comfort.mode);
}

// Restore ALL settings from SD card
bool SettingsManager::restoreFromSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }
    
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return false;
    
    // Check both old and new backup paths for compatibility
    const char* backupPath = SETTINGS_BACKUP_PATH;
    if (!fs->exists(backupPath)) {
        // Try legacy path
        if (fs->exists("/v1settings_backup.json")) {
            backupPath = "/v1settings_backup.json";
        } else {
            Serial.println("[Settings] No SD backup found");
            return false;
        }
    }
    
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
    
    // === WiFi/Network Settings (v2+) ===
    // Note: AP password NOT restored from SD for security - user must re-enter after restore
    if (doc["enableWifi"].is<bool>()) settings.enableWifi = doc["enableWifi"];
    if (doc["apSSID"].is<const char*>()) settings.apSSID = doc["apSSID"].as<String>();
    if (doc["proxyBLE"].is<bool>()) settings.proxyBLE = doc["proxyBLE"];
    if (doc["proxyName"].is<const char*>()) settings.proxyName = doc["proxyName"].as<String>();
    if (doc["lastV1Address"].is<const char*>()) settings.lastV1Address = doc["lastV1Address"].as<String>();
    if (doc["autoPowerOffMinutes"].is<int>()) settings.autoPowerOffMinutes = doc["autoPowerOffMinutes"];
    
    // === GPS/OBD Settings ===
    if (doc["gpsEnabled"].is<bool>()) settings.gpsEnabled = doc["gpsEnabled"];
    if (doc["obdEnabled"].is<bool>()) settings.obdEnabled = doc["obdEnabled"];
    if (doc["obdDeviceAddress"].is<const char*>()) settings.obdDeviceAddress = doc["obdDeviceAddress"].as<String>();
    if (doc["obdDeviceName"].is<const char*>()) settings.obdDeviceName = doc["obdDeviceName"].as<String>();
    if (doc["obdPin"].is<const char*>()) settings.obdPin = doc["obdPin"].as<String>();
    
    // === Auto-Lockout Settings (JBV1-style) ===
    if (doc["lockoutEnabled"].is<bool>()) settings.lockoutEnabled = doc["lockoutEnabled"];
    if (doc["lockoutKaProtection"].is<bool>()) settings.lockoutKaProtection = doc["lockoutKaProtection"];
    if (doc["lockoutDirectionalUnlearn"].is<bool>()) settings.lockoutDirectionalUnlearn = doc["lockoutDirectionalUnlearn"];
    if (doc["lockoutFreqToleranceMHz"].is<int>()) settings.lockoutFreqToleranceMHz = doc["lockoutFreqToleranceMHz"];
    if (doc["lockoutLearnCount"].is<int>()) settings.lockoutLearnCount = doc["lockoutLearnCount"];
    if (doc["lockoutUnlearnCount"].is<int>()) settings.lockoutUnlearnCount = doc["lockoutUnlearnCount"];
    if (doc["lockoutManualDeleteCount"].is<int>()) settings.lockoutManualDeleteCount = doc["lockoutManualDeleteCount"];
    if (doc["lockoutLearnIntervalHours"].is<int>()) settings.lockoutLearnIntervalHours = doc["lockoutLearnIntervalHours"];
    if (doc["lockoutUnlearnIntervalHours"].is<int>()) settings.lockoutUnlearnIntervalHours = doc["lockoutUnlearnIntervalHours"];
    if (doc["lockoutMaxSignalStrength"].is<int>()) settings.lockoutMaxSignalStrength = doc["lockoutMaxSignalStrength"];
    if (doc["lockoutMaxDistanceM"].is<int>()) settings.lockoutMaxDistanceM = doc["lockoutMaxDistanceM"];
    
    // === Camera Alert Settings ===
    if (doc["cameraAlertsEnabled"].is<bool>()) settings.cameraAlertsEnabled = doc["cameraAlertsEnabled"];
    if (doc["cameraAlertDistanceM"].is<int>()) settings.cameraAlertDistanceM = doc["cameraAlertDistanceM"];
    if (doc["cameraAlertRedLight"].is<bool>()) settings.cameraAlertRedLight = doc["cameraAlertRedLight"];
    if (doc["cameraAlertSpeed"].is<bool>()) settings.cameraAlertSpeed = doc["cameraAlertSpeed"];
    if (doc["cameraAlertALPR"].is<bool>()) settings.cameraAlertALPR = doc["cameraAlertALPR"];
    if (doc["cameraAudioEnabled"].is<bool>()) settings.cameraAudioEnabled = doc["cameraAudioEnabled"];
    if (doc["colorCameraAlert"].is<int>()) settings.colorCameraAlert = doc["colorCameraAlert"];
    
    // === Display Settings ===
    if (doc["brightness"].is<int>()) settings.brightness = doc["brightness"];
    if (doc["turnOffDisplay"].is<bool>()) settings.turnOffDisplay = doc["turnOffDisplay"];
    if (doc["displayStyle"].is<int>()) settings.displayStyle = static_cast<DisplayStyle>(doc["displayStyle"].as<int>());
    
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
    if (doc["colorStatusGps"].is<int>()) settings.colorStatusGps = doc["colorStatusGps"];
    if (doc["colorStatusGpsWarn"].is<int>()) settings.colorStatusGpsWarn = doc["colorStatusGpsWarn"];
    if (doc["colorStatusCam"].is<int>()) settings.colorStatusCam = doc["colorStatusCam"];
    if (doc["colorStatusObd"].is<int>()) settings.colorStatusObd = doc["colorStatusObd"];
    if (doc["freqUseBandColor"].is<bool>()) settings.freqUseBandColor = doc["freqUseBandColor"];
    
    // === UI Toggles ===
    if (doc["hideWifiIcon"].is<bool>()) settings.hideWifiIcon = doc["hideWifiIcon"];
    if (doc["hideProfileIndicator"].is<bool>()) settings.hideProfileIndicator = doc["hideProfileIndicator"];
    if (doc["hideBatteryIcon"].is<bool>()) settings.hideBatteryIcon = doc["hideBatteryIcon"];
    if (doc["showBatteryPercent"].is<bool>()) settings.showBatteryPercent = doc["showBatteryPercent"];
    if (doc["hideBleIcon"].is<bool>()) settings.hideBleIcon = doc["hideBleIcon"];
    if (doc["hideVolumeIndicator"].is<bool>()) settings.hideVolumeIndicator = doc["hideVolumeIndicator"];
    if (doc["hideRssiIndicator"].is<bool>()) settings.hideRssiIndicator = doc["hideRssiIndicator"];
    if (doc["kittScannerEnabled"].is<bool>()) settings.kittScannerEnabled = doc["kittScannerEnabled"];
    if (doc["enableWifiAtBoot"].is<bool>()) settings.enableWifiAtBoot = doc["enableWifiAtBoot"];
    if (doc["enableDebugLogging"].is<bool>()) settings.enableDebugLogging = doc["enableDebugLogging"];
    if (doc["logAlerts"].is<bool>()) settings.logAlerts = doc["logAlerts"];
    if (doc["logWifi"].is<bool>()) settings.logWifi = doc["logWifi"];
    if (doc["logBle"].is<bool>()) settings.logBle = doc["logBle"];
    if (doc["logGps"].is<bool>()) settings.logGps = doc["logGps"];
    if (doc["logObd"].is<bool>()) settings.logObd = doc["logObd"];
    if (doc["logSystem"].is<bool>()) settings.logSystem = doc["logSystem"];
    if (doc["logDisplay"].is<bool>()) settings.logDisplay = doc["logDisplay"];
    if (doc["logPerfMetrics"].is<bool>()) settings.logPerfMetrics = doc["logPerfMetrics"];
    
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
    if (doc["autoPushEnabled"].is<bool>()) settings.autoPushEnabled = doc["autoPushEnabled"];
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
    
    // Debug: log what modes were restored
    Serial.printf("[Settings] Restored modes from backup: slot0Mode=%d (in json: %s), slot1Mode=%d (in json: %s), slot2Mode=%d (in json: %s)\n",
                  settings.slot0_default.mode, doc["slot0Mode"].is<int>() ? "yes" : "NO",
                  settings.slot1_highway.mode, doc["slot1Mode"].is<int>() ? "yes" : "NO",
                  settings.slot2_comfort.mode, doc["slot2Mode"].is<int>() ? "yes" : "NO");
    
    if (!persistSettingsAtomically()) {
        Serial.println("[Settings] ERROR: Failed to persist restored settings");
        return false;
    }

    Serial.println("[Settings] ✅ Full restore from SD backup complete!");
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
        save();
        backupToSD();  // Also update SD backup
        Serial.println("[Settings] Cleared invalid profile references and saved");
    }

    // If any profiles are missing and auto-push was pointing to them, ensure OBD/GPS tasks remain unaffected
    // (no action needed here beyond clearing references; safety comment for maintainers)
}

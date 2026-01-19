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
#include <ArduinoJson.h>
#include <algorithm>

// SD backup file path
static const char* SETTINGS_BACKUP_PATH = "/v1simple_backup.json";
static const int SD_BACKUP_VERSION = 2;  // Increment when adding new fields to backup

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

SettingsManager::SettingsManager() {}

void SettingsManager::begin() {
    load();
    
    // Check if NVS was erased (appears default) and backup exists on SD
    if (checkNeedsRestore()) {
        Serial.println("[Settings] NVS appears default, checking for SD backup...");
        if (restoreFromSD()) {
            Serial.println("[Settings] Restored settings from SD backup!");
        }
    }
}

void SettingsManager::load() {
    preferences.begin("v1settings", true);  // Read-only mode
    
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
    settings.hideBleIcon = preferences.getBool("hideBle", false);
    settings.hideVolumeIndicator = preferences.getBool("hideVol", false);
    
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
        if (preferences.begin("v1settings", false)) {
            preferences.remove("voiceAlerts");
            Serial.println("[Settings] Migrated voiceAlerts -> voiceMode");
            preferences.end();
        }
        // Re-open in read-only to continue loading
        preferences.begin("v1settings", true);
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
    if (!preferences.begin("v1settings", false)) {  // Read-write mode
        Serial.println("ERROR: Failed to open preferences for writing!");
        return;
    }
    
    size_t written = 0;
    // Store settings version for migration handling
    written += preferences.putInt("settingsVer", SETTINGS_VERSION);
    written += preferences.putBool("enableWifi", settings.enableWifi);
    written += preferences.putInt("wifiMode", settings.wifiMode);
    written += preferences.putString("apSSID", settings.apSSID);
    // Obfuscate passwords before storing
    written += preferences.putString("apPassword", xorObfuscate(settings.apPassword));
    
    written += preferences.putBool("proxyBLE", settings.proxyBLE);
    written += preferences.putString("proxyName", settings.proxyName);
    written += preferences.putBool("displayOff", settings.turnOffDisplay);
    written += preferences.putUChar("brightness", settings.brightness);
    written += preferences.putInt("dispStyle", settings.displayStyle);
    written += preferences.putUShort("colorBogey", settings.colorBogey);
    written += preferences.putUShort("colorFreq", settings.colorFrequency);
    written += preferences.putUShort("colorArrF", settings.colorArrowFront);
    written += preferences.putUShort("colorArrS", settings.colorArrowSide);
    written += preferences.putUShort("colorArrR", settings.colorArrowRear);
    written += preferences.putUShort("colorBandL", settings.colorBandL);
    written += preferences.putUShort("colorBandKa", settings.colorBandKa);
    written += preferences.putUShort("colorBandK", settings.colorBandK);
    written += preferences.putUShort("colorBandX", settings.colorBandX);
    written += preferences.putUShort("colorWiFi", settings.colorWiFiIcon);
    written += preferences.putUShort("colorWiFiC", settings.colorWiFiConnected);
    written += preferences.putUShort("colorBleC", settings.colorBleConnected);
    written += preferences.putUShort("colorBleD", settings.colorBleDisconnected);
    written += preferences.putUShort("colorBar1", settings.colorBar1);
    written += preferences.putUShort("colorBar2", settings.colorBar2);
    written += preferences.putUShort("colorBar3", settings.colorBar3);
    written += preferences.putUShort("colorBar4", settings.colorBar4);
    written += preferences.putUShort("colorBar5", settings.colorBar5);
    written += preferences.putUShort("colorBar6", settings.colorBar6);
    written += preferences.putUShort("colorMuted", settings.colorMuted);
    written += preferences.putUShort("colorPersist", settings.colorPersisted);
    written += preferences.putUShort("colorVolMain", settings.colorVolumeMain);
    written += preferences.putUShort("colorVolMute", settings.colorVolumeMute);
    written += preferences.putUShort("colorRssiV1", settings.colorRssiV1);
    written += preferences.putUShort("colorRssiPrx", settings.colorRssiProxy);
    written += preferences.putBool("freqBandCol", settings.freqUseBandColor);
    written += preferences.putBool("hideWifi", settings.hideWifiIcon);
    written += preferences.putBool("hideProfile", settings.hideProfileIndicator);
    written += preferences.putBool("hideBatt", settings.hideBatteryIcon);
    written += preferences.putBool("hideBle", settings.hideBleIcon);
    written += preferences.putBool("hideVol", settings.hideVolumeIndicator);
    written += preferences.putUChar("voiceMode", (uint8_t)settings.voiceAlertMode);
    written += preferences.putBool("voiceDir", settings.voiceDirectionEnabled);
    written += preferences.putBool("voiceBogeys", settings.announceBogeyCount);
    written += preferences.putBool("muteVoiceVol0", settings.muteVoiceIfVolZero);
    written += preferences.putUChar("voiceVol", settings.voiceVolume);
    written += preferences.putBool("secAlerts", settings.announceSecondaryAlerts);
    written += preferences.putBool("secLaser", settings.secondaryLaser);
    written += preferences.putBool("secKa", settings.secondaryKa);
    written += preferences.putBool("secK", settings.secondaryK);
    written += preferences.putBool("secX", settings.secondaryX);
    written += preferences.putBool("autoPush", settings.autoPushEnabled);
    written += preferences.putInt("activeSlot", settings.activeSlot);
    written += preferences.putString("slot0name", settings.slot0Name);
    written += preferences.putString("slot1name", settings.slot1Name);
    written += preferences.putString("slot2name", settings.slot2Name);
    written += preferences.putUShort("slot0color", settings.slot0Color);
    written += preferences.putUShort("slot1color", settings.slot1Color);
    written += preferences.putUShort("slot2color", settings.slot2Color);
    written += preferences.putUChar("slot0vol", settings.slot0Volume);
    written += preferences.putUChar("slot1vol", settings.slot1Volume);
    written += preferences.putUChar("slot2vol", settings.slot2Volume);
    written += preferences.putUChar("slot0mute", settings.slot0MuteVolume);
    written += preferences.putUChar("slot1mute", settings.slot1MuteVolume);
    written += preferences.putUChar("slot2mute", settings.slot2MuteVolume);
    written += preferences.putBool("slot0dark", settings.slot0DarkMode);
    written += preferences.putBool("slot1dark", settings.slot1DarkMode);
    written += preferences.putBool("slot2dark", settings.slot2DarkMode);
    written += preferences.putBool("slot0mz", settings.slot0MuteToZero);
    written += preferences.putBool("slot1mz", settings.slot1MuteToZero);
    written += preferences.putBool("slot2mz", settings.slot2MuteToZero);
    written += preferences.putUChar("slot0persist", settings.slot0AlertPersist);
    written += preferences.putUChar("slot1persist", settings.slot1AlertPersist);
    written += preferences.putUChar("slot2persist", settings.slot2AlertPersist);
    written += preferences.putBool("slot0prio", settings.slot0PriorityArrow);
    written += preferences.putBool("slot1prio", settings.slot1PriorityArrow);
    written += preferences.putBool("slot2prio", settings.slot2PriorityArrow);
    written += preferences.putString("slot0prof", settings.slot0_default.profileName);
    written += preferences.putInt("slot0mode", settings.slot0_default.mode);
    written += preferences.putString("slot1prof", settings.slot1_highway.profileName);
    written += preferences.putInt("slot1mode", settings.slot1_highway.mode);
    written += preferences.putString("slot2prof", settings.slot2_comfort.profileName);
    written += preferences.putInt("slot2mode", settings.slot2_comfort.mode);
    written += preferences.putString("lastV1Addr", settings.lastV1Address);
    
    preferences.end();
    
    Serial.printf("Settings saved (%d bytes)\n", written);
    
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

void SettingsManager::setHideBleIcon(bool hide) {
    settings.hideBleIcon = hide;
    save();
}

void SettingsManager::setHideVolumeIndicator(bool hide) {
    settings.hideVolumeIndicator = hide;
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
    return settings.brightness == 200 &&
           settings.colorBogey == 0xF800 &&
           settings.colorBandL == 0x001F &&
           settings.colorBar1 == 0x07E0 &&
           settings.hideWifiIcon == false &&
           settings.hideProfileIndicator == false &&
           settings.hideBatteryIcon == false;
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
    doc["hideBleIcon"] = settings.hideBleIcon;
    doc["hideVolumeIndicator"] = settings.hideVolumeIndicator;
    
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
    if (doc["hideBleIcon"].is<bool>()) settings.hideBleIcon = doc["hideBleIcon"];
    if (doc["hideVolumeIndicator"].is<bool>()) settings.hideVolumeIndicator = doc["hideVolumeIndicator"];
    
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
    
    // Save ALL restored settings to NVS
    preferences.begin("v1settings", false);
    preferences.putInt("settingsVer", SETTINGS_VERSION);
    preferences.putBool("enableWifi", settings.enableWifi);
    preferences.putString("apSSID", settings.apSSID);
    preferences.putString("apPassword", xorObfuscate(settings.apPassword));
    preferences.putBool("proxyBLE", settings.proxyBLE);
    preferences.putString("proxyName", settings.proxyName);
    preferences.putString("lastV1Addr", settings.lastV1Address);
    preferences.putUChar("brightness", settings.brightness);
    preferences.putBool("displayOff", settings.turnOffDisplay);
    preferences.putInt("dispStyle", settings.displayStyle);
    preferences.putUShort("colorBogey", settings.colorBogey);
    preferences.putUShort("colorFreq", settings.colorFrequency);
    preferences.putUShort("colorArrF", settings.colorArrowFront);
    preferences.putUShort("colorArrS", settings.colorArrowSide);
    preferences.putUShort("colorArrR", settings.colorArrowRear);
    preferences.putUShort("colorBandL", settings.colorBandL);
    preferences.putUShort("colorBandKa", settings.colorBandKa);
    preferences.putUShort("colorBandK", settings.colorBandK);
    preferences.putUShort("colorBandX", settings.colorBandX);
    preferences.putUShort("colorWiFi", settings.colorWiFiIcon);
    preferences.putUShort("colorWiFiC", settings.colorWiFiConnected);
    preferences.putUShort("colorBleC", settings.colorBleConnected);
    preferences.putUShort("colorBleD", settings.colorBleDisconnected);
    preferences.putUShort("colorBar1", settings.colorBar1);
    preferences.putUShort("colorBar2", settings.colorBar2);
    preferences.putUShort("colorBar3", settings.colorBar3);
    preferences.putUShort("colorBar4", settings.colorBar4);
    preferences.putUShort("colorBar5", settings.colorBar5);
    preferences.putUShort("colorBar6", settings.colorBar6);
    preferences.putUShort("colorMuted", settings.colorMuted);
    preferences.putUShort("colorPersist", settings.colorPersisted);
    preferences.putUShort("colorVolMain", settings.colorVolumeMain);
    preferences.putUShort("colorVolMute", settings.colorVolumeMute);
    preferences.putUShort("colorRssiV1", settings.colorRssiV1);
    preferences.putUShort("colorRssiPrx", settings.colorRssiProxy);
    preferences.putBool("freqBandCol", settings.freqUseBandColor);
    preferences.putBool("hideWifi", settings.hideWifiIcon);
    preferences.putBool("hideProfile", settings.hideProfileIndicator);
    preferences.putBool("hideBatt", settings.hideBatteryIcon);
    preferences.putBool("hideBle", settings.hideBleIcon);
    preferences.putBool("hideVol", settings.hideVolumeIndicator);
    preferences.putUChar("voiceMode", (uint8_t)settings.voiceAlertMode);
    preferences.putBool("voiceDir", settings.voiceDirectionEnabled);
    preferences.putBool("voiceBogeys", settings.announceBogeyCount);
    preferences.putBool("muteVoiceVol0", settings.muteVoiceIfVolZero);
    preferences.putUChar("voiceVol", settings.voiceVolume);
    preferences.putBool("secAlerts", settings.announceSecondaryAlerts);
    preferences.putBool("secLaser", settings.secondaryLaser);
    preferences.putBool("secKa", settings.secondaryKa);
    preferences.putBool("secK", settings.secondaryK);
    preferences.putBool("secX", settings.secondaryX);
    preferences.putBool("autoPush", settings.autoPushEnabled);
    preferences.putInt("activeSlot", settings.activeSlot);
    preferences.putString("slot0name", settings.slot0Name);
    preferences.putString("slot1name", settings.slot1Name);
    preferences.putString("slot2name", settings.slot2Name);
    preferences.putUShort("slot0color", settings.slot0Color);
    preferences.putUShort("slot1color", settings.slot1Color);
    preferences.putUShort("slot2color", settings.slot2Color);
    preferences.putUChar("slot0vol", settings.slot0Volume);
    preferences.putUChar("slot1vol", settings.slot1Volume);
    preferences.putUChar("slot2vol", settings.slot2Volume);
    preferences.putUChar("slot0mute", settings.slot0MuteVolume);
    preferences.putUChar("slot1mute", settings.slot1MuteVolume);
    preferences.putUChar("slot2mute", settings.slot2MuteVolume);
    preferences.putBool("slot0dark", settings.slot0DarkMode);
    preferences.putBool("slot1dark", settings.slot1DarkMode);
    preferences.putBool("slot2dark", settings.slot2DarkMode);
    preferences.putBool("slot0mz", settings.slot0MuteToZero);
    preferences.putBool("slot1mz", settings.slot1MuteToZero);
    preferences.putBool("slot2mz", settings.slot2MuteToZero);
    preferences.putUChar("slot0persist", settings.slot0AlertPersist);
    preferences.putUChar("slot1persist", settings.slot1AlertPersist);
    preferences.putUChar("slot2persist", settings.slot2AlertPersist);
    preferences.putBool("slot0prio", settings.slot0PriorityArrow);
    preferences.putBool("slot1prio", settings.slot1PriorityArrow);
    preferences.putBool("slot2prio", settings.slot2PriorityArrow);
    preferences.putString("slot0prof", settings.slot0_default.profileName);
    preferences.putInt("slot0mode", settings.slot0_default.mode);
    preferences.putString("slot1prof", settings.slot1_highway.profileName);
    preferences.putInt("slot1mode", settings.slot1_highway.mode);
    preferences.putString("slot2prof", settings.slot2_comfort.profileName);
    preferences.putInt("slot2mode", settings.slot2_comfort.mode);
    preferences.end();
    
    Serial.println("[Settings] ✅ Full restore from SD backup complete!");
    return true;
}

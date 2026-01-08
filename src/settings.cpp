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
static const char* SETTINGS_BACKUP_PATH = "/v1settings_backup.json";

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
    settings.wifiMode = static_cast<WiFiModeSetting>(preferences.getInt("wifiMode", V1_WIFI_AP));
    settings.ssid = preferences.getString("ssid", "");
    
    // Handle password storage - version 1 was plain text, version 2+ is obfuscated
    String storedPwd = preferences.getString("password", "");
    String storedApPwd = preferences.getString("apPassword", "");
    
    if (storedVersion >= 2) {
        // Passwords are obfuscated - decode them
        settings.password = storedPwd.length() > 0 ? xorObfuscate(storedPwd) : "";
        settings.apPassword = storedApPwd.length() > 0 ? xorObfuscate(storedApPwd) : "setupv1g2";
    } else {
        // Version 1 - passwords stored in plain text, use as-is
        settings.password = storedPwd;
        settings.apPassword = storedApPwd.length() > 0 ? storedApPwd : "setupv1g2";
        Serial.println("[Settings] Migrating from v1 to v2 (password obfuscation)");
    }
    
    settings.apSSID = preferences.getString("apSSID", "V1-Simple");
    
    // Load multiple WiFi networks
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        String ssidKey = "wifiSSID" + String(i);
        String pwdKey = "wifiPwd" + String(i);
        String storedNetPwd = preferences.getString(pwdKey.c_str(), "");
        settings.wifiNetworks[i].ssid = preferences.getString(ssidKey.c_str(), "");
        if (storedVersion >= 2) {
            settings.wifiNetworks[i].password = storedNetPwd.length() > 0 ? xorObfuscate(storedNetPwd) : "";
        } else {
            settings.wifiNetworks[i].password = storedNetPwd;
        }
        Serial.printf("[Settings] Network[%d]: SSID='%s' (len=%d), PWD len=%d\n", 
                      i, settings.wifiNetworks[i].ssid.c_str(), 
                      settings.wifiNetworks[i].ssid.length(),
                      settings.wifiNetworks[i].password.length());
    }
    
    // Legacy migration: check if there's an old staSSID stored in preferences but wifiNetworks[0] is empty
    String legacyStaSSID = preferences.getString("staSSID", "");
    String legacyStaPwd = preferences.getString("staPassword", "");
    if (!settings.wifiNetworks[0].isValid() && legacyStaSSID.length() > 0) {
        settings.wifiNetworks[0].ssid = legacyStaSSID;
        settings.wifiNetworks[0].password = storedVersion >= 2 && legacyStaPwd.length() > 0 ? xorObfuscate(legacyStaPwd) : legacyStaPwd;
        Serial.println("[Settings] Migrated legacy staSSID to wifiNetworks[0]");
    }
    
    settings.proxyBLE = preferences.getBool("proxyBLE", true);
    settings.proxyName = preferences.getString("proxyName", "V1-Proxy");
    settings.turnOffDisplay = preferences.getBool("displayOff", false);
    settings.brightness = preferences.getUChar("brightness", 200);
    settings.colorTheme = static_cast<ColorTheme>(preferences.getInt("colorTheme", THEME_STANDARD));
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
    settings.colorBleConnected = preferences.getUShort("colorBleC", 0x07E0);
    settings.colorBleDisconnected = preferences.getUShort("colorBleD", 0x001F);
    settings.colorBar1 = preferences.getUShort("colorBar1", 0x07E0);
    settings.colorBar2 = preferences.getUShort("colorBar2", 0x07E0);
    settings.colorBar3 = preferences.getUShort("colorBar3", 0xFFE0);
    settings.colorBar4 = preferences.getUShort("colorBar4", 0xFFE0);
    settings.colorBar5 = preferences.getUShort("colorBar5", 0xF800);
    settings.colorBar6 = preferences.getUShort("colorBar6", 0xF800);
    settings.hideWifiIcon = preferences.getBool("hideWifi", false);
    settings.hideProfileIndicator = preferences.getBool("hideProfile", false);
    settings.hideBatteryIcon = preferences.getBool("hideBatt", false);
    settings.hideBleIcon = preferences.getBool("hideBle", false);
    settings.enableMultiAlert = preferences.getBool("multiAlert", true);
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
    Serial.printf("  WiFi mode: %d\n", settings.wifiMode);
    Serial.printf("  SSID: %s\n", settings.ssid.c_str());
    Serial.printf("  AP SSID: %s\n", settings.apSSID.c_str());
    // Note: Passwords not logged for security
    int validNetworks = 0;
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (settings.wifiNetworks[i].isValid()) validNetworks++;
    }
    Serial.printf("  WiFi networks: %d configured\n", validNetworks);
    Serial.printf("  BLE proxy: %s\n", settings.proxyBLE ? "yes" : "no");
    Serial.printf("  Proxy name: %s\n", settings.proxyName.c_str());
    Serial.printf("  Brightness: %d\n", settings.brightness);
    Serial.printf("  Color theme: %d\n", settings.colorTheme);
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
    written += preferences.putString("ssid", settings.ssid);
    // Obfuscate passwords before storing
    written += preferences.putString("password", xorObfuscate(settings.password));
    written += preferences.putString("apSSID", settings.apSSID);
    written += preferences.putString("apPassword", xorObfuscate(settings.apPassword));
    
    // Save multiple WiFi networks
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        String ssidKey = "wifiSSID" + String(i);
        String pwdKey = "wifiPwd" + String(i);
        written += preferences.putString(ssidKey.c_str(), settings.wifiNetworks[i].ssid);
        written += preferences.putString(pwdKey.c_str(), xorObfuscate(settings.wifiNetworks[i].password));
    }
    
    written += preferences.putBool("proxyBLE", settings.proxyBLE);
    written += preferences.putString("proxyName", settings.proxyName);
    written += preferences.putBool("displayOff", settings.turnOffDisplay);
    written += preferences.putUChar("brightness", settings.brightness);
    written += preferences.putInt("colorTheme", settings.colorTheme);
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
    written += preferences.putUShort("colorBleC", settings.colorBleConnected);
    written += preferences.putUShort("colorBleD", settings.colorBleDisconnected);
    written += preferences.putUShort("colorBar1", settings.colorBar1);
    written += preferences.putUShort("colorBar2", settings.colorBar2);
    written += preferences.putUShort("colorBar3", settings.colorBar3);
    written += preferences.putUShort("colorBar4", settings.colorBar4);
    written += preferences.putUShort("colorBar5", settings.colorBar5);
    written += preferences.putUShort("colorBar6", settings.colorBar6);
    written += preferences.putBool("hideWifi", settings.hideWifiIcon);
    written += preferences.putBool("hideProfile", settings.hideProfileIndicator);
    written += preferences.putBool("hideBatt", settings.hideBatteryIcon);
    written += preferences.putBool("hideBle", settings.hideBleIcon);
    written += preferences.putBool("multiAlert", settings.enableMultiAlert);
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

void SettingsManager::setWiFiMode(WiFiModeSetting mode) {
    settings.wifiMode = mode;
    save();
}

void SettingsManager::setWiFiCredentials(const String& ssid, const String& password) {
    settings.ssid = ssid;
    settings.password = password;
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

void SettingsManager::setColorTheme(ColorTheme theme) {
    settings.colorTheme = theme;
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

void SettingsManager::setWiFiIconColor(uint16_t color) {
    settings.colorWiFiIcon = color;
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

void SettingsManager::setEnableMultiAlert(bool enable) {
    settings.enableMultiAlert = enable;
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
    doc["version"] = 1;
    doc["timestamp"] = millis();
    
    // Display settings
    doc["brightness"] = settings.brightness;
    doc["turnOffDisplay"] = settings.turnOffDisplay;
    doc["colorTheme"] = static_cast<int>(settings.colorTheme);
    doc["displayStyle"] = static_cast<int>(settings.displayStyle);
    
    // All colors (RGB565)
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
    doc["colorBleConnected"] = settings.colorBleConnected;
    doc["colorBleDisconnected"] = settings.colorBleDisconnected;
    doc["colorBar1"] = settings.colorBar1;
    doc["colorBar2"] = settings.colorBar2;
    doc["colorBar3"] = settings.colorBar3;
    doc["colorBar4"] = settings.colorBar4;
    doc["colorBar5"] = settings.colorBar5;
    doc["colorBar6"] = settings.colorBar6;
    
    // Display toggles
    doc["hideWifiIcon"] = settings.hideWifiIcon;
    doc["hideProfileIndicator"] = settings.hideProfileIndicator;
    doc["hideBatteryIcon"] = settings.hideBatteryIcon;
    doc["hideBleIcon"] = settings.hideBleIcon;
    
    // Slot customizations
    doc["slot0Name"] = settings.slot0Name;
    doc["slot1Name"] = settings.slot1Name;
    doc["slot2Name"] = settings.slot2Name;
    doc["slot0Color"] = settings.slot0Color;
    doc["slot1Color"] = settings.slot1Color;
    doc["slot2Color"] = settings.slot2Color;
    
    // Write to file
    File file = fs->open(SETTINGS_BACKUP_PATH, FILE_WRITE);
    if (!file) {
        Serial.println("[Settings] Failed to create SD backup file");
        return;
    }
    
    serializeJson(doc, file);
    file.flush();
    file.close();
    
    Serial.println("[Settings] Backed up to SD card");
}

// Restore display/color settings from SD card
bool SettingsManager::restoreFromSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }
    
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return false;
    
    if (!fs->exists(SETTINGS_BACKUP_PATH)) {
        Serial.println("[Settings] No SD backup found");
        return false;
    }
    
    File file = fs->open(SETTINGS_BACKUP_PATH, FILE_READ);
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
    
    // Restore display settings (using is<T>() for ArduinoJson v7 compatibility)
    if (doc["brightness"].is<int>()) settings.brightness = doc["brightness"];
    if (doc["turnOffDisplay"].is<bool>()) settings.turnOffDisplay = doc["turnOffDisplay"];
    if (doc["colorTheme"].is<int>()) settings.colorTheme = static_cast<ColorTheme>(doc["colorTheme"].as<int>());
    if (doc["displayStyle"].is<int>()) settings.displayStyle = static_cast<DisplayStyle>(doc["displayStyle"].as<int>());
    
    // Restore all colors
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
    if (doc["colorBleConnected"].is<int>()) settings.colorBleConnected = doc["colorBleConnected"];
    if (doc["colorBleDisconnected"].is<int>()) settings.colorBleDisconnected = doc["colorBleDisconnected"];
    if (doc["colorBar1"].is<int>()) settings.colorBar1 = doc["colorBar1"];
    if (doc["colorBar2"].is<int>()) settings.colorBar2 = doc["colorBar2"];
    if (doc["colorBar3"].is<int>()) settings.colorBar3 = doc["colorBar3"];
    if (doc["colorBar4"].is<int>()) settings.colorBar4 = doc["colorBar4"];
    if (doc["colorBar5"].is<int>()) settings.colorBar5 = doc["colorBar5"];
    if (doc["colorBar6"].is<int>()) settings.colorBar6 = doc["colorBar6"];
    
    // Restore display toggles
    if (doc["hideWifiIcon"].is<bool>()) settings.hideWifiIcon = doc["hideWifiIcon"];
    if (doc["hideProfileIndicator"].is<bool>()) settings.hideProfileIndicator = doc["hideProfileIndicator"];
    if (doc["hideBatteryIcon"].is<bool>()) settings.hideBatteryIcon = doc["hideBatteryIcon"];
    if (doc["hideBleIcon"].is<bool>()) settings.hideBleIcon = doc["hideBleIcon"];
    
    // Restore slot customizations
    if (doc["slot0Name"].is<const char*>()) settings.slot0Name = doc["slot0Name"].as<String>();
    if (doc["slot1Name"].is<const char*>()) settings.slot1Name = doc["slot1Name"].as<String>();
    if (doc["slot2Name"].is<const char*>()) settings.slot2Name = doc["slot2Name"].as<String>();
    if (doc["slot0Color"].is<int>()) settings.slot0Color = doc["slot0Color"];
    if (doc["slot1Color"].is<int>()) settings.slot1Color = doc["slot1Color"];
    if (doc["slot2Color"].is<int>()) settings.slot2Color = doc["slot2Color"];
    
    // Save restored settings to NVS (without backing up again - avoid loop)
    preferences.begin("v1settings", false);
    preferences.putUChar("brightness", settings.brightness);
    preferences.putBool("displayOff", settings.turnOffDisplay);
    preferences.putInt("colorTheme", settings.colorTheme);
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
    preferences.putUShort("colorBleC", settings.colorBleConnected);
    preferences.putUShort("colorBleD", settings.colorBleDisconnected);
    preferences.putUShort("colorBar1", settings.colorBar1);
    preferences.putUShort("colorBar2", settings.colorBar2);
    preferences.putUShort("colorBar3", settings.colorBar3);
    preferences.putUShort("colorBar4", settings.colorBar4);
    preferences.putUShort("colorBar5", settings.colorBar5);
    preferences.putUShort("colorBar6", settings.colorBar6);
    preferences.putBool("hideWifi", settings.hideWifiIcon);
    preferences.putBool("hideProfile", settings.hideProfileIndicator);
    preferences.putBool("hideBatt", settings.hideBatteryIcon);
    preferences.putBool("hideBle", settings.hideBleIcon);
    preferences.putString("slot0name", settings.slot0Name);
    preferences.putString("slot1name", settings.slot1Name);
    preferences.putString("slot2name", settings.slot2Name);
    preferences.putUShort("slot0color", settings.slot0Color);
    preferences.putUShort("slot1color", settings.slot1Color);
    preferences.putUShort("slot2color", settings.slot2Color);
    preferences.end();
    
    Serial.println("[Settings] Restored from SD backup and saved to NVS");
    return true;
}

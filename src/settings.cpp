/**
 * Settings storage implementation
 */

#include "settings.h"

// Global instance
SettingsManager settingsManager;

SettingsManager::SettingsManager() {}

void SettingsManager::begin() {
    load();
}

void SettingsManager::load() {
    preferences.begin("v1settings", true);  // Read-only mode
    
    settings.enableWifi = preferences.getBool("enableWifi", true);
    settings.wifiMode = static_cast<WiFiModeSetting>(preferences.getInt("wifiMode", V1_WIFI_AP));
    settings.ssid = preferences.getString("ssid", "");
    settings.password = preferences.getString("password", "");
    settings.apSSID = preferences.getString("apSSID", "V1-Display");
    settings.apPassword = preferences.getString("apPassword", "valentine1");
    settings.proxyBLE = preferences.getBool("proxyBLE", true);
    settings.proxyName = preferences.getString("proxyName", "V1C-LE-S3");
    settings.turnOffDisplay = preferences.getBool("displayOff", false);
    settings.brightness = preferences.getUChar("brightness", 200);
    settings.colorTheme = static_cast<ColorTheme>(preferences.getInt("colorTheme", THEME_STANDARD));
    settings.colorBogey = preferences.getUShort("colorBogey", 0xF800);
    settings.colorFrequency = preferences.getUShort("colorFreq", 0xF800);
    settings.colorArrow = preferences.getUShort("colorArrow", 0xF800);
    settings.colorBandL = preferences.getUShort("colorBandL", 0x001F);
    settings.colorBandKa = preferences.getUShort("colorBandKa", 0xF800);
    settings.colorBandK = preferences.getUShort("colorBandK", 0x001F);
    settings.colorBandX = preferences.getUShort("colorBandX", 0x07E0);
    settings.autoPushEnabled = preferences.getBool("autoPush", false);
    settings.activeSlot = preferences.getInt("activeSlot", 0);
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
    settings.slot0_default.profileName = preferences.getString("slot0prof", "");
    settings.slot0_default.mode = static_cast<V1Mode>(preferences.getInt("slot0mode", V1_MODE_UNKNOWN));
    settings.slot1_highway.profileName = preferences.getString("slot1prof", "");
    settings.slot1_highway.mode = static_cast<V1Mode>(preferences.getInt("slot1mode", V1_MODE_UNKNOWN));
    settings.slot2_comfort.profileName = preferences.getString("slot2prof", "");
    settings.slot2_comfort.mode = static_cast<V1Mode>(preferences.getInt("slot2mode", V1_MODE_UNKNOWN));
    
    preferences.end();
    
    Serial.println("Settings loaded:");
    Serial.printf("  WiFi enabled: %s\n", settings.enableWifi ? "yes" : "no");
    Serial.printf("  WiFi mode: %d\n", settings.wifiMode);
    Serial.printf("  SSID: %s\n", settings.ssid.c_str());
    Serial.printf("  AP SSID: %s\n", settings.apSSID.c_str());
    Serial.printf("  BLE proxy: %s\n", settings.proxyBLE ? "yes" : "no");
    Serial.printf("  Proxy name: %s\n", settings.proxyName.c_str());
    Serial.printf("  Brightness: %d\n", settings.brightness);
    Serial.printf("  Color theme: %d\n", settings.colorTheme);
    Serial.printf("  Auto-push: %s (active slot: %d)\n", settings.autoPushEnabled ? "yes" : "no", settings.activeSlot);
    Serial.printf("  Slot0: %s (mode %d)\n", settings.slot0_default.profileName.c_str(), settings.slot0_default.mode);
    Serial.printf("  Slot1: %s (mode %d)\n", settings.slot1_highway.profileName.c_str(), settings.slot1_highway.mode);
    Serial.printf("  Slot2: %s (mode %d)\n", settings.slot2_comfort.profileName.c_str(), settings.slot2_comfort.mode);
}

void SettingsManager::save() {
    Serial.println("=== SettingsManager::save() starting ===");
    Serial.printf("  About to save - brightness: %d, wifiMode: %d\n", settings.brightness, settings.wifiMode);
    
    if (!preferences.begin("v1settings", false)) {  // Read-write mode
        Serial.println("ERROR: Failed to open preferences for writing!");
        return;
    }
    
    size_t written = 0;
    written += preferences.putBool("enableWifi", settings.enableWifi);
    written += preferences.putInt("wifiMode", settings.wifiMode);
    written += preferences.putString("ssid", settings.ssid);
    written += preferences.putString("password", settings.password);
    written += preferences.putString("apSSID", settings.apSSID);
    written += preferences.putString("apPassword", settings.apPassword);
    written += preferences.putBool("proxyBLE", settings.proxyBLE);
    written += preferences.putString("proxyName", settings.proxyName);
    written += preferences.putBool("displayOff", settings.turnOffDisplay);
    written += preferences.putUChar("brightness", settings.brightness);
    written += preferences.putInt("colorTheme", settings.colorTheme);
    written += preferences.putUShort("colorBogey", settings.colorBogey);
    written += preferences.putUShort("colorFreq", settings.colorFrequency);
    written += preferences.putUShort("colorArrow", settings.colorArrow);
    written += preferences.putUShort("colorBandL", settings.colorBandL);
    written += preferences.putUShort("colorBandKa", settings.colorBandKa);
    written += preferences.putUShort("colorBandK", settings.colorBandK);
    written += preferences.putUShort("colorBandX", settings.colorBandX);
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
    written += preferences.putString("slot0prof", settings.slot0_default.profileName);
    written += preferences.putInt("slot0mode", settings.slot0_default.mode);
    written += preferences.putString("slot1prof", settings.slot1_highway.profileName);
    written += preferences.putInt("slot1mode", settings.slot1_highway.mode);
    written += preferences.putString("slot2prof", settings.slot2_comfort.profileName);
    written += preferences.putInt("slot2mode", settings.slot2_comfort.mode);
    
    preferences.end();
    
    Serial.printf("Settings saved, bytes written: %d\n", written);
    
    // Verify by re-reading
    preferences.begin("v1settings", true);
    int verifyBrightness = preferences.getUChar("brightness", 0);
    int verifyMode = preferences.getInt("wifiMode", -1);
    preferences.end();
    Serial.printf("  Verify read-back - brightness: %d, wifiMode: %d\n", verifyBrightness, verifyMode);
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

void SettingsManager::setDisplayColors(uint16_t bogey, uint16_t freq, uint16_t arrow,
                                        uint16_t bandL, uint16_t bandKa, uint16_t bandK, uint16_t bandX) {
    settings.colorBogey = bogey;
    settings.colorFrequency = freq;
    settings.colorArrow = arrow;
    settings.colorBandL = bandL;
    settings.colorBandKa = bandKa;
    settings.colorBandK = bandK;
    settings.colorBandX = bandX;
    save();
}

const AutoPushSlot& SettingsManager::getActiveSlot() const {
    switch (settings.activeSlot) {
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

void SettingsManager::resetToDefaults() {
    settings = V1Settings();  // Reset to defaults
    save();
    Serial.println("Settings reset to defaults");
}

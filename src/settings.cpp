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
    settings.autoPushEnabled = preferences.getBool("autoPush", false);
    settings.activeSlot = preferences.getInt("activeSlot", 0);
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
    written += preferences.putBool("autoPush", settings.autoPushEnabled);
    written += preferences.putInt("activeSlot", settings.activeSlot);
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

AutoPushSlot SettingsManager::getActiveSlot() const {
    switch (settings.activeSlot) {
        case 1: return settings.slot1_highway;
        case 2: return settings.slot2_comfort;
        default: return settings.slot0_default;
    }
}

void SettingsManager::resetToDefaults() {
    settings = V1Settings();  // Reset to defaults
    save();
    Serial.println("Settings reset to defaults");
}

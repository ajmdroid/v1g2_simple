/**
 * Settings Storage for V1 Gen2 Display
 * Uses ESP32 Preferences API for persistent flash storage
 * 
 * Settings Categories:
 * - WiFi: Mode (Off/STA/AP/APSTA), credentials
 * - BLE Proxy: Enable/disable, device name
 * - Display: Brightness, color theme, resting mode
 * - Auto-Push: 3-slot profile system with modes
 * 
 * Auto-Push Slots:
 * - Slot 0: Default profile (🏠)
 * - Slot 1: Highway profile (🏎️)
 * - Slot 2: Passenger Comfort profile (👥)
 * Each slot stores: profile name + V1 operating mode
 * 
 * Thread Safety: Load/save operations should be called from main thread
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <Preferences.h>
#include "../include/color_themes.h"

// WiFi mode options (prefixed to avoid conflicts with ESP SDK)
enum WiFiModeSetting {
    V1_WIFI_OFF = 0,        // WiFi disabled
    V1_WIFI_STA = 1,        // Connect to existing network
    V1_WIFI_AP = 2,         // Create access point
    V1_WIFI_APSTA = 3       // Both modes
};

// V1 operating modes (from ESP library)
enum V1Mode {
    V1_MODE_UNKNOWN = 0x00,
    V1_MODE_ALL_BOGEYS = 0x01,    // All Bogeys (K+Ka) or Custom Sweeps
    V1_MODE_LOGIC = 0x02,         // Logic mode (Ka only)
    V1_MODE_ADVANCED_LOGIC = 0x03 // Advanced Logic
};

// Auto-push profile slot
struct AutoPushSlot {
    String profileName;
    V1Mode mode;
    
    AutoPushSlot() : profileName(""), mode(V1_MODE_UNKNOWN) {}
    AutoPushSlot(const String& name, V1Mode m) : profileName(name), mode(m) {}
};

// Settings structure
struct V1Settings {
    // WiFi settings
    bool enableWifi;
    WiFiModeSetting wifiMode;
    String ssid;
    String password;
    String apSSID;
    String apPassword;
    
    // BLE proxy settings
    bool proxyBLE;          // Enable BLE proxy for JBV1
    String proxyName;       // BLE device name when proxying
    
    // Display settings
    bool turnOffDisplay;
    uint8_t brightness;
    ColorTheme colorTheme;  // Color theme selection
    
    // Auto-push on connection settings
    bool autoPushEnabled;        // Enable auto-push profile on V1 connection
    int activeSlot;              // Which slot is active: 0=Default, 1=Highway, 2=Comfort
    AutoPushSlot slot0_default;
    AutoPushSlot slot1_highway;
    AutoPushSlot slot2_comfort;
    
    // Default constructor with sensible defaults
    V1Settings() : 
        enableWifi(true),
        wifiMode(V1_WIFI_AP),
        ssid(""),
        password(""),
        apSSID("V1-Display"),
        apPassword("valentine1"),
        proxyBLE(true),
        proxyName("V1C-LE-S3"),
        turnOffDisplay(false),
        brightness(200),
        colorTheme(THEME_STANDARD),
        autoPushEnabled(false),
        activeSlot(0),
        slot0_default(),
        slot1_highway(),
        slot2_comfort() {}
};

class SettingsManager {
public:
    SettingsManager();
    
    // Initialize and load settings
    void begin();
    
    // Get current settings (read-only)
    const V1Settings& get() const { return settings; }
    
    // Update settings (calls save automatically)
    void setWiFiEnabled(bool enabled);
    void setWiFiMode(WiFiModeSetting mode);
    void setWiFiCredentials(const String& ssid, const String& password);
    void setAPCredentials(const String& ssid, const String& password);
    void setProxyBLE(bool enabled);
    void setProxyName(const String& name);
    void setBrightness(uint8_t brightness);
    void setDisplayOff(bool off);
    void setColorTheme(ColorTheme theme);
    void setAutoPushEnabled(bool enabled);
    void setActiveSlot(int slot);
    void setSlot(int slotNum, const String& profileName, V1Mode mode);
    
    // Get active slot configuration
    AutoPushSlot getActiveSlot() const;
    
    // Batch update methods (don't auto-save, call save() after)
    void updateWiFiMode(WiFiModeSetting mode) { settings.wifiMode = mode; }
    void updateWiFiCredentials(const String& ssid, const String& password) { settings.ssid = ssid; settings.password = password; }
    void updateAPCredentials(const String& ssid, const String& password) { settings.apSSID = ssid; settings.apPassword = password; }
    void updateBrightness(uint8_t brightness) { settings.brightness = brightness; }
    void updateColorTheme(ColorTheme theme) { settings.colorTheme = theme; }
    
    // Save all settings to flash
    void save();
    
    // Load settings from flash (public for testing)
    void load();
    
    // Reset to defaults
    void resetToDefaults();

private:
    V1Settings settings;
    Preferences preferences;
};

// Global settings instance
extern SettingsManager settingsManager;

#endif // SETTINGS_H

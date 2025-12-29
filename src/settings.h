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
    
    // Custom display colors (RGB565 format)
    uint16_t colorBogey;         // Bogey counter color
    uint16_t colorFrequency;     // Frequency display color
    uint16_t colorArrow;         // Direction arrow color
    uint16_t colorBandL;         // Laser band color
    uint16_t colorBandKa;        // Ka band color
    uint16_t colorBandK;         // K band color
    uint16_t colorBandX;         // X band color
    
    // Auto-push on connection settings
    bool autoPushEnabled;        // Enable auto-push profile on V1 connection
    int activeSlot;              // Which slot is active: 0=Default, 1=Highway, 2=Comfort
    String slot0Name;            // Custom display name for slot 0 (default: "DEFAULT")
    String slot1Name;            // Custom display name for slot 1 (default: "HIGHWAY")
    String slot2Name;            // Custom display name for slot 2 (default: "COMFORT")
    uint16_t slot0Color;         // Custom color for slot 0 display (default: purple 0x780F)
    uint16_t slot1Color;         // Custom color for slot 1 display (default: green 0x07E0)
    uint16_t slot2Color;         // Custom color for slot 2 display (default: grey 0x8410)
    uint8_t slot0Volume;         // V1 main volume for slot 0 (0-9, 0xFF=no change)
    uint8_t slot1Volume;         // V1 main volume for slot 1 (0-9, 0xFF=no change)
    uint8_t slot2Volume;         // V1 main volume for slot 2 (0-9, 0xFF=no change)
    uint8_t slot0MuteVolume;     // V1 mute volume for slot 0 (0-9, 0xFF=no change)
    uint8_t slot1MuteVolume;     // V1 mute volume for slot 1 (0-9, 0xFF=no change)
    uint8_t slot2MuteVolume;     // V1 mute volume for slot 2 (0-9, 0xFF=no change)
    AutoPushSlot slot0_default;
    AutoPushSlot slot1_highway;
    AutoPushSlot slot2_comfort;
    
    String lastV1Address;  // Last known V1 BLE address for fast reconnect
    
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
        colorBogey(0xF800),      // Red (same as KA)
        colorFrequency(0xF800),  // Red (same as KA)
        colorArrow(0xF800),      // Red
        colorBandL(0x001F),      // Blue (laser)
        colorBandKa(0xF800),     // Red
        colorBandK(0x001F),      // Blue
        colorBandX(0x07E0),      // Green
        autoPushEnabled(false),
        activeSlot(0),
        slot0Name("DEFAULT"),
        slot1Name("HIGHWAY"),
        slot2Name("COMFORT"),
        slot0Color(0x400A),
        slot1Color(0x07E0),
        slot2Color(0x8410),
        slot0Volume(0xFF),
        slot1Volume(0xFF),
        slot2Volume(0xFF),
        slot0MuteVolume(0xFF),
        slot1MuteVolume(0xFF),
        slot2MuteVolume(0xFF),
        slot0_default(),
        slot1_highway(),
        slot2_comfort(),
        lastV1Address("") {}
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
    void setSlotName(int slotNum, const String& name);
    void setSlotColor(int slotNum, uint16_t color);
    void setSlotVolumes(int slotNum, uint8_t volume, uint8_t muteVolume);
    void setDisplayColors(uint16_t bogey, uint16_t freq, uint16_t arrow,
                          uint16_t bandL, uint16_t bandKa, uint16_t bandK, uint16_t bandX);
    void setLastV1Address(const String& addr);
    
    // Get active slot configuration
    const AutoPushSlot& getActiveSlot() const;
    
    // Get slot volume settings (returns 0xFF for "no change")
    uint8_t getSlotVolume(int slotNum) const;
    uint8_t getSlotMuteVolume(int slotNum) const;
    
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

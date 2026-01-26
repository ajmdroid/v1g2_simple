/**
 * Mock settings.h for native unit testing
 * Provides minimal Settings struct and SettingsManager stub
 */
#pragma once

#include "Arduino.h"
#include <cstdint>

// Font style enum
enum FontStyle : uint8_t {
    FONT_STYLE_CLASSIC = 0,   // 7-segment LED style
    FONT_STYLE_MODERN = 1,    // Montserrat Bold
    FONT_STYLE_HEMI = 2,      // Hemi Head (retro speedometer)
    FONT_STYLE_SERPENTINE = 3 // Serpentine Bold (JB's favorite)
};

// Theme names
enum ColorTheme : uint8_t {
    THEME_STANDARD = 0
};

// WiFi mode enum
enum V1WiFiMode : uint8_t {
    V1_WIFI_AP = 0,
    V1_WIFI_APSTA = 1
};

// Settings structure with display-related fields
struct Settings {
    // Display settings
    uint8_t brightness = 128;
    bool displayOn = true;
    FontStyle fontStyle = FONT_STYLE_CLASSIC;
    ColorTheme colorTheme = THEME_STANDARD;
    
    // Color settings (RGB565)
    uint16_t colorX = 0x07E0;      // Green for X band
    uint16_t colorK = 0x07FF;      // Cyan for K band  
    uint16_t colorKa = 0xF800;     // Red for Ka band
    uint16_t colorLaser = 0xFFFF;  // White for Laser
    uint16_t colorPhoto = 0xF81F;  // Magenta for Photo Radar
    uint16_t colorMuted = 0x8410;  // Gray for muted
    uint16_t colorBogey = 0xFFE0;  // Yellow for bogey counter
    
    // Audio settings
    uint8_t volume = 5;
    bool alertVolumeFadeEnabled = false;
    uint8_t alertVolumeFadeDelaySec = 5;
    
    // GPS settings
    bool gpsEnabled = true;
    
    // OBD settings
    bool obdEnabled = false;
    
    // BLE proxy settings
    bool bleProxyEnabled = true;
    
    // KITT scanner
    bool kittScannerEnabled = true;
};

// Settings manager stub
class SettingsManager {
public:
    Settings settings;
    
    void load() {}
    void save() {}
    void setDefaults() {}
    
    uint8_t getBrightness() const { return settings.brightness; }
    void setBrightness(uint8_t b) { settings.brightness = b; }
    
    bool isDisplayOn() const { return settings.displayOn; }
    void setDisplayOn(bool on) { settings.displayOn = on; }
    
    FontStyle getFontStyle() const { return settings.fontStyle; }
    void setFontStyle(FontStyle style) { settings.fontStyle = style; }
    
    // Color getters
    uint16_t getColorX() const { return settings.colorX; }
    uint16_t getColorK() const { return settings.colorK; }
    uint16_t getColorKa() const { return settings.colorKa; }
    uint16_t getColorLaser() const { return settings.colorLaser; }
    uint16_t getColorPhoto() const { return settings.colorPhoto; }
    uint16_t getColorMuted() const { return settings.colorMuted; }
    uint16_t getColorBogey() const { return settings.colorBogey; }
    
    bool isKittScannerEnabled() const { return settings.kittScannerEnabled; }
    bool isGpsEnabled() const { return settings.gpsEnabled; }
    bool isBleProxyEnabled() const { return settings.bleProxyEnabled; }
};

// Global settings instance
extern SettingsManager settingsManager;

#endif // settings mock

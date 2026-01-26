/**
 * Settings Storage for V1 Gen2 Display
 * Uses ESP32 Preferences API for persistent flash storage
 * 
 * Settings Categories:
 * - WiFi: Mode (Off/STA/AP/APSTA), credentials
 * - BLE Proxy: Enable/disable, device name
 * - Display: Brightness, custom colors, resting mode
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
#include <FS.h>
#include "../include/color_themes.h"

// Forward declaration
class V1ProfileManager;

// WiFi mode options (prefixed to avoid conflicts with ESP SDK)
enum WiFiModeSetting {
    V1_WIFI_OFF = 0,        // WiFi disabled
    V1_WIFI_STA = 1,        // Connect to existing network
    V1_WIFI_AP = 2,         // Create access point
    V1_WIFI_APSTA = 3       // Both modes
};

// Debug logging category configuration
struct DebugLogConfig {
    bool alerts;
    bool wifi;
    bool ble;
    bool gps;
    bool obd;
    bool system;
    bool display;
    bool perfMetrics;
    bool audio;
    bool camera;
    bool lockout;
    bool touch;
};

// V1 operating modes (from ESP library)
enum V1Mode {
    V1_MODE_UNKNOWN = 0x00,
    V1_MODE_ALL_BOGEYS = 0x01,    // All Bogeys (K+Ka) or Custom Sweeps
    V1_MODE_LOGIC = 0x02,         // Logic mode (Ka only)
    V1_MODE_ADVANCED_LOGIC = 0x03 // Advanced Logic
};

// Display style (font selection)
enum DisplayStyle {
    DISPLAY_STYLE_CLASSIC = 0,   // 7-segment style (original V1 look)
    DISPLAY_STYLE_MODERN = 1,    // Montserrat Bold font
    DISPLAY_STYLE_HEMI = 2,      // Hemi Head font (retro speedometer style)
    DISPLAY_STYLE_SERPENTINE = 3 // Serpentine font (JB's favorite)
};

// Voice alert content mode
enum VoiceAlertMode {
    VOICE_MODE_DISABLED = 0,     // Voice alerts disabled
    VOICE_MODE_BAND_ONLY = 1,    // Just band name ("Ka")
    VOICE_MODE_FREQ_ONLY = 2,    // Just frequency ("34.7")
    VOICE_MODE_BAND_FREQ = 3     // Band + frequency ("Ka 34.7")
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
    WiFiModeSetting wifiMode;  // V1_WIFI_AP (default) or V1_WIFI_APSTA (with client)
    String apSSID;           // AP mode SSID (device hotspot name)
    String apPassword;       // AP mode password
    
    // WiFi client (STA) settings - connect to external network
    bool wifiClientEnabled;  // Enable WiFi client mode (AP+STA dual mode)
    String wifiClientSSID;   // SSID of network to connect to
    // NOTE: wifiClientPassword stored separately in secure NVS namespace
    
    // BLE proxy settings
    bool proxyBLE;          // Enable BLE proxy for JBV1
    String proxyName;       // BLE device name when proxying
    
    // Display settings
    bool turnOffDisplay;
    uint8_t brightness;
    DisplayStyle displayStyle;  // Font style: classic 7-segment or modern Montserrat
    
    // Custom display colors (RGB565 format)
    uint16_t colorBogey;         // Bogey counter color
    uint16_t colorFrequency;     // Frequency display color
    uint16_t colorArrowFront;    // Front arrow color
    uint16_t colorArrowSide;     // Side arrow color
    uint16_t colorArrowRear;     // Rear arrow color
    uint16_t colorBandL;         // Laser band color
    uint16_t colorBandKa;        // Ka band color
    uint16_t colorBandK;         // K band color
    uint16_t colorBandX;         // X band color
    uint16_t colorBandPhoto;     // Photo radar color (when V1 sends 'P')
    uint16_t colorWiFiIcon;      // WiFi indicator icon color (no client)
    uint16_t colorWiFiConnected;  // WiFi icon when client connected
    uint16_t colorBleConnected;   // Bluetooth icon when client connected
    uint16_t colorBleDisconnected; // Bluetooth icon when no client
    uint16_t colorBar1;          // Signal bar 1 (bottom/weakest)
    uint16_t colorBar2;          // Signal bar 2
    uint16_t colorBar3;          // Signal bar 3
    uint16_t colorBar4;          // Signal bar 4
    uint16_t colorBar5;          // Signal bar 5
    uint16_t colorBar6;          // Signal bar 6 (top/strongest)
    uint16_t colorMuted;         // Muted alert color (shown when alerts are muted/grayed)
    uint16_t colorPersisted;     // Persisted alert color (shown after alert disappears)
    uint16_t colorVolumeMain;    // Volume indicator main volume color
    uint16_t colorVolumeMute;    // Volume indicator muted volume color
    uint16_t colorRssiV1;        // RSSI indicator V1 label color
    uint16_t colorRssiProxy;     // RSSI indicator Proxy label color
    uint16_t colorStatusGps;     // Status bar GPS color (good fix, >=4 sats)
    uint16_t colorStatusGpsWarn; // Status bar GPS color (weak fix, <4 sats)
    uint16_t colorStatusCam;     // Status bar CAM indicator color
    uint16_t colorStatusObd;     // Status bar OBD indicator color
    bool freqUseBandColor;       // Use band color for frequency display instead of custom freq color
    
    // Display visibility settings
    bool hideWifiIcon;           // Hide WiFi icon after brief display
    bool hideProfileIndicator;   // Hide profile indicator after brief display
    bool hideBatteryIcon;        // Hide battery icon
    bool showBatteryPercent;     // Show battery percentage text next to icon
    bool hideBleIcon;            // Hide BLE icon
    bool hideVolumeIndicator;    // Hide volume indicator (V1 firmware 4.1028+ only)
    bool hideRssiIndicator;      // Hide RSSI signal strength indicator
    bool kittScannerEnabled;     // KITT scanner animation on resting screen (easter egg)
    
    // Development/Debug settings
    bool enableWifiAtBoot;       // Start WiFi automatically on boot (bypasses BOOT button)
    bool enableDebugLogging;     // Write debug logs to SD card
        bool logAlerts;              // Include alert events in debug log
        bool logWifi;                // Include WiFi/AP events in debug log
        bool logBle;                 // Include BLE/proxy events in debug log
        bool logGps;                 // Include GPS events in debug log
        bool logObd;                 // Include OBD events in debug log
        bool logSystem;              // Include system/storage/events in debug log
        bool logDisplay;             // Include display latency events in debug log
        bool logPerfMetrics;         // Log BLE performance metrics periodically
        bool logAudio;               // Include audio/TTS playback events in debug log
        bool logCamera;              // Include camera alert events in debug log
        bool logLockout;             // Include auto-lockout events in debug log
        bool logTouch;               // Include touch input events in debug log
    
    // Voice alerts (when no app connected)
    VoiceAlertMode voiceAlertMode;  // What content to speak (disabled/band/freq/band+freq)
    bool voiceDirectionEnabled;     // Append direction ("ahead"/"side"/"behind") to voice
    bool announceBogeyCount;        // Announce bogey count after direction ("2 bogeys")
    bool muteVoiceIfVolZero;        // Mute voice alerts (not VOL0 warning) when V1 volume is 0
    uint8_t voiceVolume;            // Voice alert volume (0-100%)
    
    // Secondary alert announcements (non-priority alerts)
    bool announceSecondaryAlerts;   // Master toggle for secondary announcements
    bool secondaryLaser;            // Announce secondary Laser alerts
    bool secondaryKa;               // Announce secondary Ka alerts
    bool secondaryK;                // Announce secondary K alerts
    bool secondaryX;                // Announce secondary X alerts
    
    // Volume fade (reduce V1 volume after initial alert period)
    bool alertVolumeFadeEnabled;    // Enable volume fade feature
    uint8_t alertVolumeFadeDelaySec; // Seconds at full volume before fading (1-10)
    uint8_t alertVolumeFadeVolume;  // Volume to fade to (0-9)
    
    // Speed-based volume (boost V1 volume at highway speeds)
    bool speedVolumeEnabled;        // Enable speed-based volume boost
    uint8_t speedVolumeThresholdMph; // Speed threshold to trigger boost (default: 45 mph)
    uint8_t speedVolumeBoost;       // Volume levels to add when above threshold (1-5)
    
    // Low-speed mute (suppress voice at low speeds, e.g., parking lots)
    bool lowSpeedMuteEnabled;        // Enable low-speed voice muting
    uint8_t lowSpeedMuteThresholdMph; // Mute voice when below this speed (default: 5 mph)
    
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
    bool slot0DarkMode;          // V1 display off (dark mode) for slot 0
    bool slot1DarkMode;          // V1 display off (dark mode) for slot 1
    bool slot2DarkMode;          // V1 display off (dark mode) for slot 2
    bool slot0MuteToZero;        // Mute to zero for slot 0
    bool slot1MuteToZero;        // Mute to zero for slot 1
    bool slot2MuteToZero;        // Mute to zero for slot 2
    uint8_t slot0AlertPersist;   // Alert persistence (seconds) for slot 0 (0-5s)
    uint8_t slot1AlertPersist;   // Alert persistence (seconds) for slot 1 (0-5s)
    uint8_t slot2AlertPersist;   // Alert persistence (seconds) for slot 2 (0-5s)
    bool slot0PriorityArrow;     // Priority arrow only for slot 0
    bool slot1PriorityArrow;     // Priority arrow only for slot 1
    bool slot2PriorityArrow;     // Priority arrow only for slot 2
    AutoPushSlot slot0_default;
    AutoPushSlot slot1_highway;
    AutoPushSlot slot2_comfort;
    
    String lastV1Address;  // Last known V1 BLE address for fast reconnect
    
    // Auto power-off on V1 disconnect
    uint8_t autoPowerOffMinutes;  // Minutes to wait after V1 disconnect before power off (0=disabled)
    
    // GPS settings
    bool gpsEnabled;          // Enable GPS module (default: off, auto-disabled if not found)
    
    // OBD settings  
    bool obdEnabled;          // Enable OBD-II module (default: off, auto-disabled if not found)
    String obdDeviceAddress;  // Saved OBD device BLE address (e.g., "AA:BB:CC:DD:EE:FF")
    String obdDeviceName;     // Saved OBD device name (for display)
    String obdPin;            // PIN code for OBD adapter (typically "1234")
    
    // Auto-Lockout settings (JBV1-style)
    bool lockoutEnabled;            // Master enable for auto-lockout system
    bool lockoutKaProtection;       // Never auto-learn Ka band (real threats)
    bool lockoutDirectionalUnlearn; // Only unlearn when traveling same direction
    uint16_t lockoutFreqToleranceMHz;  // Frequency tolerance in MHz (default: 8)
    uint8_t lockoutLearnCount;      // Hits needed to promote (default: 3)
    uint8_t lockoutUnlearnCount;    // Misses to demote auto-lockouts (default: 5)
    uint8_t lockoutManualDeleteCount; // Misses to demote manual lockouts (default: 25)
    uint8_t lockoutLearnIntervalHours;   // Hours between counted hits (default: 4)
    uint8_t lockoutUnlearnIntervalHours; // Hours between counted misses (default: 4)
    uint8_t lockoutMaxSignalStrength;    // Don't learn signals >= this (0=disabled, default: 0)
    uint16_t lockoutMaxDistanceM;   // Max alert distance to learn (default: 600m)
    
    // Camera alerts settings (red light cameras, speed cameras, ALPR)
    bool cameraAlertsEnabled;          // Master enable for camera alerts
    uint16_t cameraAlertDistanceM;     // Alert distance in meters (default: 500m)
    bool cameraAlertRedLight;          // Alert on red light cameras
    bool cameraAlertSpeed;             // Alert on speed cameras
    bool cameraAlertALPR;              // Alert on ALPR cameras
    bool cameraAudioEnabled;           // Play audio for camera alerts
    uint16_t colorCameraAlert;         // Camera alert display color (default: orange)
    
    // Default constructor with sensible defaults
    V1Settings() : 
        enableWifi(true),
        wifiMode(V1_WIFI_AP),
        apSSID("V1-Simple"),
        apPassword("setupv1g2"),
        wifiClientEnabled(false),  // WiFi client disabled by default
        wifiClientSSID(""),        // No saved network
        proxyBLE(true),
        proxyName("V1C-LE-S3"),
        turnOffDisplay(false),
        brightness(200),
        displayStyle(DISPLAY_STYLE_CLASSIC),  // Default to classic 7-segment
        colorBogey(0xF800),      // Red (same as KA)
        colorFrequency(0xF800),  // Red (same as KA)
        colorArrowFront(0xF800), // Red (front)
        colorArrowSide(0xF800),  // Red (side)
        colorArrowRear(0xF800),  // Red (rear)
        colorBandL(0x001F),      // Blue (laser)
        colorBandKa(0xF800),     // Red
        colorBandK(0x001F),      // Blue
        colorBandX(0x07E0),      // Green
        colorBandPhoto(0x780F),  // Purple (photo radar)
        colorWiFiIcon(0x07FF),   // Cyan (WiFi icon, no client)
        colorWiFiConnected(0x07E0), // Green (WiFi client connected)
        colorBleConnected(0x07E0),   // Green (BLE connected)
        colorBleDisconnected(0x001F), // Blue (BLE disconnected)
        colorBar1(0x07E0),       // Green (weakest)
        colorBar2(0x07E0),       // Green
        colorBar3(0xFFE0),       // Yellow
        colorBar4(0xFFE0),       // Yellow
        colorBar5(0xF800),       // Red
        colorBar6(0xF800),       // Red (strongest)
        colorStatusGps(0x07E0),  // Green (GPS good)
        colorStatusGpsWarn(0xFD20), // Orange (GPS weak)
        colorStatusCam(0x07FF),  // Cyan (camera DB)
        colorStatusObd(0x07E0),  // Green (OBD connected)
        freqUseBandColor(false), // Use custom freq color by default
        hideWifiIcon(false),     // Show WiFi icon by default
        hideProfileIndicator(false), // Show profile indicator by default
        hideBatteryIcon(false),  // Show battery icon by default
        hideBleIcon(false),      // Show BLE icon by default
        hideVolumeIndicator(false), // Show volume indicator by default
        kittScannerEnabled(false),   // KITT scanner off by default (easter egg)
        voiceAlertMode(VOICE_MODE_BAND_FREQ),  // Full band+freq announcements by default
        voiceDirectionEnabled(true),           // Include direction by default
        announceBogeyCount(true),              // Announce bogey count by default
        muteVoiceIfVolZero(false), // Don't mute voice alerts at vol 0 by default
        voiceVolume(75),           // Voice alerts at 75% volume by default
        announceSecondaryAlerts(false),  // Secondary alerts off by default (opt-in)
        secondaryLaser(true),            // Laser always important
        secondaryKa(true),               // Ka usually real threats
        secondaryK(false),               // K has more false positives
        secondaryX(false),               // X is rare
        alertVolumeFadeEnabled(false),   // Volume fade disabled by default
        alertVolumeFadeDelaySec(2),      // 2 seconds at full volume before fade
        alertVolumeFadeVolume(1),        // Fade to volume 1 (quiet but audible)
        speedVolumeEnabled(false),       // Speed-based volume disabled by default
        speedVolumeThresholdMph(45),     // Boost above 45 mph (highway speeds)
        speedVolumeBoost(2),             // Add 2 volume levels when above threshold
        lowSpeedMuteEnabled(false),      // Low-speed voice mute disabled by default
        lowSpeedMuteThresholdMph(5),     // Mute voice below 5 mph (parking lot mode)
        logAlerts(true),                 // Alert logging on by default
        logWifi(true),                   // WiFi logging on by default
        logBle(false),                   // BLE logging off by default
        logGps(false),                   // GPS logging off by default
        logObd(false),                   // OBD logging off by default
        logSystem(true),                 // System/storage logging on by default
        logDisplay(false),               // Display latency logging off by default
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
        slot0DarkMode(false),
        slot1DarkMode(false),
        slot2DarkMode(false),
        slot0MuteToZero(false),
        slot1MuteToZero(false),
        slot2MuteToZero(false),
        slot0AlertPersist(0),
        slot1AlertPersist(0),
        slot2AlertPersist(0),
        slot0PriorityArrow(false),
        slot1PriorityArrow(false),
        slot2PriorityArrow(false),
        slot0_default(),
        slot1_highway(),
        slot2_comfort(),
        lastV1Address(""),
        autoPowerOffMinutes(0),  // Default: disabled
        gpsEnabled(false),       // GPS off by default (opt-in)
        obdEnabled(false),       // OBD off by default (opt-in)
        obdDeviceAddress(""),    // No saved OBD device
        obdDeviceName(""),       // No saved OBD device name
        obdPin("1234"),          // Default ELM327 PIN
        // Auto-lockout defaults (JBV1 defaults)
        lockoutEnabled(true),           // Auto-lockout enabled by default
        lockoutKaProtection(true),      // Never learn Ka (JBV1 default)
        lockoutDirectionalUnlearn(true),// Directional unlearn on (JBV1 default)
        lockoutFreqToleranceMHz(8),     // 8 MHz tolerance (JBV1 default)
        lockoutLearnCount(3),           // 3 hits to learn (JBV1 default)
        lockoutUnlearnCount(5),         // 5 misses to unlearn auto (JBV1 default)
        lockoutManualDeleteCount(25),   // 25 misses to unlearn manual (JBV1 default)
        lockoutLearnIntervalHours(4),   // 4 hours between hits (JBV1 default)
        lockoutUnlearnIntervalHours(4), // 4 hours between misses (JBV1 default)
        lockoutMaxSignalStrength(0),    // No max (JBV1 "None" default)
        lockoutMaxDistanceM(600),       // 600m max distance (JBV1 default)
        // Camera alert defaults
        cameraAlertsEnabled(true),      // Camera alerts on by default
        cameraAlertDistanceM(500),      // Alert 500m before camera
        cameraAlertRedLight(true),      // Red light cameras on
        cameraAlertSpeed(true),         // Speed cameras on
        cameraAlertALPR(true),          // ALPR cameras on
        cameraAudioEnabled(true),       // Audio alerts on
        colorCameraAlert(0xFD20) {}     // Orange (camera alert color)
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
    void setAPCredentials(const String& ssid, const String& password);
    void setProxyBLE(bool enabled);
    void setProxyName(const String& name);
    void setAutoPowerOffMinutes(uint8_t minutes);
    void setBrightness(uint8_t brightness);
    void setDisplayOff(bool off);
    void setAutoPushEnabled(bool enabled);
    void setActiveSlot(int slot);
    void setSlot(int slotNum, const String& profileName, V1Mode mode);
    void setSlotName(int slotNum, const String& name);
    void setSlotColor(int slotNum, uint16_t color);
    void setSlotVolumes(int slotNum, uint8_t volume, uint8_t muteVolume);
    void setDisplayColors(uint16_t bogey, uint16_t freq, uint16_t arrowFront, uint16_t arrowSide, uint16_t arrowRear,
                          uint16_t bandL, uint16_t bandKa, uint16_t bandK, uint16_t bandX);
    void setWiFiIconColors(uint16_t icon, uint16_t connected);
    void setBleIconColors(uint16_t connected, uint16_t disconnected);
    void setSignalBarColors(uint16_t bar1, uint16_t bar2, uint16_t bar3, uint16_t bar4, uint16_t bar5, uint16_t bar6);
    void setMutedColor(uint16_t color);
    void setBandPhotoColor(uint16_t color);
    void setPersistedColor(uint16_t color);
    void setVolumeMainColor(uint16_t color);
    void setVolumeMuteColor(uint16_t color);
    void setRssiV1Color(uint16_t color);
    void setRssiProxyColor(uint16_t color);
    void setStatusGpsColor(uint16_t color);
    void setStatusGpsWarnColor(uint16_t color);
    void setStatusCamColor(uint16_t color);
    void setStatusObdColor(uint16_t color);
    void setFreqUseBandColor(bool use);
    void setHideWifiIcon(bool hide);
    void setHideProfileIndicator(bool hide);
    void setHideBatteryIcon(bool hide);
    void setShowBatteryPercent(bool show);
    void setHideBleIcon(bool hide);
    void setHideVolumeIndicator(bool hide);
    void setHideRssiIndicator(bool hide);
    void setKittScannerEnabled(bool enabled);
    void setEnableWifiAtBoot(bool enable);
    void setEnableDebugLogging(bool enable);
    void setLogAlerts(bool enable);
    void setLogWifi(bool enable);
    void setLogBle(bool enable);
    void setLogGps(bool enable);
    void setLogObd(bool enable);
    void setLogSystem(bool enable);
    void setLogDisplay(bool enable);
    void setLogPerfMetrics(bool enable);
    void setLogAudio(bool enable);
    void setLogCamera(bool enable);
    void setLogLockout(bool enable);
    void setLogTouch(bool enable);
    DebugLogConfig getDebugLogConfig() const {
        return { settings.logAlerts, settings.logWifi, settings.logBle, settings.logGps, settings.logObd, settings.logSystem, settings.logDisplay, settings.logPerfMetrics, settings.logAudio, settings.logCamera, settings.logLockout, settings.logTouch };
    }
    void setVoiceAlertMode(VoiceAlertMode mode);
    void setVoiceDirectionEnabled(bool enabled);
    void setAnnounceBogeyCount(bool enabled);
    void setMuteVoiceIfVolZero(bool mute);
    void setAnnounceSecondaryAlerts(bool enabled);
    void setSecondaryLaser(bool enabled);
    void setSecondaryKa(bool enabled);
    void setSecondaryK(bool enabled);
    void setSecondaryX(bool enabled);
    void setAlertVolumeFade(bool enabled, uint8_t delaySec, uint8_t volume);
    void setSpeedVolume(bool enabled, uint8_t thresholdMph, uint8_t boost);
    void setLowSpeedMute(bool enabled, uint8_t thresholdMph);
    void setLastV1Address(const String& addr);
    
    // Get active slot configuration
    const AutoPushSlot& getActiveSlot() const;
    const AutoPushSlot& getSlot(int slotNum) const;
    
    // Get slot volume settings (returns 0xFF for "no change")
    uint8_t getSlotVolume(int slotNum) const;
    uint8_t getSlotMuteVolume(int slotNum) const;
    
    // Get slot dark mode and MZ settings
    bool getSlotDarkMode(int slotNum) const;
    bool getSlotMuteToZero(int slotNum) const;
    uint8_t getSlotAlertPersistSec(int slotNum) const;
    bool getSlotPriorityArrowOnly(int slotNum) const;
    void setSlotDarkMode(int slotNum, bool darkMode);
    void setSlotMuteToZero(int slotNum, bool mz);
    void setSlotAlertPersistSec(int slotNum, uint8_t seconds);
    void setSlotPriorityArrowOnly(int slotNum, bool prioArrow);
    
    // Batch update methods (don't auto-save, call save() after)
    void updateAPCredentials(const String& ssid, const String& password) { settings.apSSID = ssid; settings.apPassword = password; }
    void updateBrightness(uint8_t brightness) { settings.brightness = brightness; }
    void updateVoiceVolume(uint8_t volume) { settings.voiceVolume = volume; }
    void updateDisplayStyle(DisplayStyle style) { settings.displayStyle = style; }
    
    // Save all settings to flash
    void save();
    
    // Load settings from flash (public for testing)
    void load();
    
    // Reset to defaults
    void resetToDefaults();
    
    // GPS/OBD settings
    bool isGpsEnabled() const { return settings.gpsEnabled; }
    bool isObdEnabled() const { return settings.obdEnabled; }
    void setGpsEnabled(bool enabled) { settings.gpsEnabled = enabled; save(); }
    void setObdEnabled(bool enabled) { settings.obdEnabled = enabled; save(); }
    
    // OBD device settings
    const String& getObdDeviceAddress() const { return settings.obdDeviceAddress; }
    const String& getObdDeviceName() const { return settings.obdDeviceName; }
    const String& getObdPin() const { return settings.obdPin; }
    void setObdDevice(const String& address, const String& name) { 
        settings.obdDeviceAddress = address; 
        settings.obdDeviceName = name;
        save(); 
    }
    void setObdPin(const String& pin) { settings.obdPin = pin; save(); }
    
    // WiFi client (STA) settings - connect to external network
    bool isWifiClientEnabled() const { return settings.wifiClientEnabled; }
    const String& getWifiClientSSID() const { return settings.wifiClientSSID; }
    String getWifiClientPassword();  // Retrieves from secure NVS namespace
    void setWifiClientEnabled(bool enabled);
    void setWifiClientCredentials(const String& ssid, const String& password);
    void clearWifiClientCredentials();  // Forget saved network
    
    // Auto-lockout settings (batch update - call save() after)
    void updateLockoutEnabled(bool enabled) { settings.lockoutEnabled = enabled; }
    void updateLockoutKaProtection(bool enabled) { settings.lockoutKaProtection = enabled; }
    void updateLockoutDirectionalUnlearn(bool enabled) { settings.lockoutDirectionalUnlearn = enabled; }
    void updateLockoutFreqToleranceMHz(uint16_t mhz) { settings.lockoutFreqToleranceMHz = mhz; }
    void updateLockoutLearnCount(uint8_t count) { settings.lockoutLearnCount = count; }
    void updateLockoutUnlearnCount(uint8_t count) { settings.lockoutUnlearnCount = count; }
    void updateLockoutManualDeleteCount(uint8_t count) { settings.lockoutManualDeleteCount = count; }
    void updateLockoutLearnIntervalHours(uint8_t hours) { settings.lockoutLearnIntervalHours = hours; }
    void updateLockoutUnlearnIntervalHours(uint8_t hours) { settings.lockoutUnlearnIntervalHours = hours; }
    void updateLockoutMaxSignalStrength(uint8_t strength) { settings.lockoutMaxSignalStrength = strength; }
    void updateLockoutMaxDistanceM(uint16_t meters) { settings.lockoutMaxDistanceM = meters; }
    
    // Camera alert settings (batch update - call save() after)
    bool isCameraAlertsEnabled() const { return settings.cameraAlertsEnabled; }
    bool isCameraAudioEnabled() const { return settings.cameraAudioEnabled; }
    void updateCameraAlertsEnabled(bool enabled) { settings.cameraAlertsEnabled = enabled; }
    void updateCameraAudioEnabled(bool enabled) { settings.cameraAudioEnabled = enabled; }
    void updateCameraAlertDistanceM(uint16_t meters) { settings.cameraAlertDistanceM = meters; }
    
    // SD card backup/restore for display settings
    void backupToSD();
    bool restoreFromSD();
    bool checkAndRestoreFromSD();  // Call after storage is mounted to retry restore
    
    // Validate profile references exist - clear invalid ones
    void validateProfileReferences(V1ProfileManager& profileMgr);

private:
    V1Settings settings;
    Preferences preferences;
    bool persistSettingsAtomically();
    bool writeSettingsToNamespace(const char* ns);
    String getActiveNamespace();
    String getStagingNamespace(const String& activeNamespace);
    bool checkNeedsRestore();  // Returns true if NVS appears to be default/empty
};

// Global settings instance
extern SettingsManager settingsManager;

#endif // SETTINGS_H

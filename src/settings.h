/**
 * Settings Storage for V1 Gen2 Display
 * Uses ESP32 Preferences API for persistent flash storage
 *
 * Settings Categories:
 * - WiFi: Mode (Off/AP/APSTA), credentials
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

#pragma once
#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <FS.h>
#include <algorithm>
#include <cstdint>
#include "../include/color_themes.h"

// Forward declaration
class V1ProfileManager;

// WiFi mode options (prefixed to avoid conflicts with ESP SDK)
enum WiFiModeSetting {
    V1_WIFI_OFF = 0,        // WiFi disabled
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

// Display style (font selection)
enum DisplayStyle {
    DISPLAY_STYLE_CLASSIC = 0,   // 7-segment style (original V1 look)
    DISPLAY_STYLE_SERPENTINE = 3 // Serpentine font
};

// Project currently supports only Classic and Serpentine in active UI/boot paths.
inline DisplayStyle normalizeDisplayStyle(int rawStyle) {
    return (rawStyle == static_cast<int>(DISPLAY_STYLE_SERPENTINE))
        ? DISPLAY_STYLE_SERPENTINE
        : DISPLAY_STYLE_CLASSIC;
}

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
    bool proxyBLE;          // Enable BLE proxy for companion app
    String proxyName;       // BLE device name when proxying

    // Display settings
    bool turnOffDisplay;
    uint8_t brightness;
    DisplayStyle displayStyle;  // Active styles: classic 7-segment or serpentine

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
    uint16_t colorObd;           // OBD "OBD" status text color when connected
    bool freqUseBandColor;       // Use band color for frequency display instead of custom freq color

    // Display visibility settings
    bool hideWifiIcon;           // Hide WiFi icon after brief display
    bool hideProfileIndicator;   // Hide profile indicator after brief display
    bool hideBatteryIcon;        // Hide battery icon
    bool showBatteryPercent;     // Show battery percentage text next to icon
    bool hideBleIcon;            // Hide BLE icon
    bool hideVolumeIndicator;    // Hide volume indicator (V1 firmware 4.1028+ only)
    bool hideRssiIndicator;      // Hide RSSI signal strength indicator

    // Development settings
    bool enableWifiAtBoot;       // Start WiFi automatically on boot (bypasses BOOT button)

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

    // Speed-aware muting (suppress alerts below speed threshold)
    bool speedMuteEnabled;           // Enable speed-based auto-muting
    uint8_t speedMuteThresholdMph;   // Mute below this speed (5-60 mph)
    uint8_t speedMuteHysteresisMph;  // Unmute at threshold + hysteresis (1-10 mph)
    uint8_t speedMuteVolume;         // V1 volume when speed-muted (0-9, 0xFF = voice-only)

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

    struct AutoPushSlotView {
        String& name;
        uint16_t& color;
        uint8_t& volume;
        uint8_t& muteVolume;
        bool& darkMode;
        bool& muteToZero;
        uint8_t& alertPersist;
        bool& priorityArrow;
        AutoPushSlot& config;
    };

    struct ConstAutoPushSlotView {
        const String& name;
        const uint16_t& color;
        const uint8_t& volume;
        const uint8_t& muteVolume;
        const bool& darkMode;
        const bool& muteToZero;
        const uint8_t& alertPersist;
        const bool& priorityArrow;
        const AutoPushSlot& config;
    };

    String lastV1Address;  // Last known V1 BLE address for fast reconnect

    // Auto power-off on V1 disconnect
    uint8_t autoPowerOffMinutes;  // Minutes to wait after V1 disconnect before power off (0=disabled)
    uint8_t apTimeoutMinutes;       // Minutes before AP auto-stops (0=always on, 5-60)

    // OBD-II speed source settings
    bool obdEnabled;             // Enable OBD module
    String obdSavedAddress;      // Saved OBDLink CX BLE address for auto-reconnect
    String obdSavedName;         // Optional friendly name for the saved OBD adapter
    uint8_t obdSavedAddrType;    // Saved BLE address type (0=public, 1=random)
    int8_t obdMinRssi;           // Minimum RSSI for scan acceptance (dBm)
    String obdCachedVinPrefix11; // Last VIN prefix matched to cached EOT profile
    uint8_t obdCachedEotProfileId; // Cached EOT profile id for same-vehicle fast start

    // Default constructor with sensible defaults
    V1Settings() :
        enableWifi(true),
        wifiMode(V1_WIFI_AP),
        apSSID("V1-Simple"),
        apPassword("setupv1g2"),
        wifiClientEnabled(false),  // WiFi client disabled by default
        wifiClientSSID(""),        // No saved network
        proxyBLE(true),
        proxyName("V1-Proxy"),  // Must match NVS load() default
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
        colorMuted(0x3186),      // Dark grey (muted alerts) — matches NVS default
        colorPersisted(0x18C3),  // Darker grey (persisted alerts) — matches NVS default
        colorVolumeMain(0xF800), // Red (volume bar) — matches NVS default
        colorVolumeMute(0x7BEF), // Grey (muted volume) — matches NVS default
        colorRssiV1(0x07E0),     // Green (V1 RSSI label) — matches NVS default
        colorRssiProxy(0x001F),  // Blue (proxy RSSI label) — matches NVS default
        colorObd(0x001F),         // Blue OBD badge (matches existing BLE disconnected icon default)
        freqUseBandColor(false), // Use custom freq color by default
        hideWifiIcon(false),     // Show WiFi icon by default
        hideProfileIndicator(false), // Show profile indicator by default
        hideBatteryIcon(false),  // Show battery icon by default
        showBatteryPercent(false), // Hide battery % text by default — matches NVS default
        hideBleIcon(false),      // Show BLE icon by default
        hideVolumeIndicator(false), // Show volume indicator by default
        hideRssiIndicator(false),   // Show RSSI indicator by default — matches NVS default
        enableWifiAtBoot(false),    // WiFi off at boot by default — matches NVS default
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
        speedMuteEnabled(false),         // Speed mute disabled by default
        speedMuteThresholdMph(25),       // 25 mph default (city driving)
        speedMuteHysteresisMph(3),       // 3 mph hysteresis band
        speedMuteVolume(0xFF),           // Voice-only by default (no V1 volume change)
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
        apTimeoutMinutes(0),     // Default: always on (0=unlimited)
        obdEnabled(false),       // OBD disabled by default
        obdSavedAddress(""),     // No saved device
        obdSavedName(""),        // No friendly name
        obdSavedAddrType(0),     // Default PUBLIC address type
        obdMinRssi(-90),         // Default -90 dBm minimum RSSI
        obdCachedVinPrefix11(""),
        obdCachedEotProfileId(0) {}

    static uint8_t normalizeAutoPushSlotIndex(int slotNum) {
        return slotNum == 1 ? 1 : (slotNum == 2 ? 2 : 0);
    }

    AutoPushSlotView autoPushSlotView(int slotNum) {
        switch (normalizeAutoPushSlotIndex(slotNum)) {
            case 1:
                return AutoPushSlotView{
                    slot1Name,
                    slot1Color,
                    slot1Volume,
                    slot1MuteVolume,
                    slot1DarkMode,
                    slot1MuteToZero,
                    slot1AlertPersist,
                    slot1PriorityArrow,
                    slot1_highway,
                };
            case 2:
                return AutoPushSlotView{
                    slot2Name,
                    slot2Color,
                    slot2Volume,
                    slot2MuteVolume,
                    slot2DarkMode,
                    slot2MuteToZero,
                    slot2AlertPersist,
                    slot2PriorityArrow,
                    slot2_comfort,
                };
            default:
                return AutoPushSlotView{
                    slot0Name,
                    slot0Color,
                    slot0Volume,
                    slot0MuteVolume,
                    slot0DarkMode,
                    slot0MuteToZero,
                    slot0AlertPersist,
                    slot0PriorityArrow,
                    slot0_default,
                };
        }
    }

    ConstAutoPushSlotView autoPushSlotView(int slotNum) const {
        switch (normalizeAutoPushSlotIndex(slotNum)) {
            case 1:
                return ConstAutoPushSlotView{
                    slot1Name,
                    slot1Color,
                    slot1Volume,
                    slot1MuteVolume,
                    slot1DarkMode,
                    slot1MuteToZero,
                    slot1AlertPersist,
                    slot1PriorityArrow,
                    slot1_highway,
                };
            case 2:
                return ConstAutoPushSlotView{
                    slot2Name,
                    slot2Color,
                    slot2Volume,
                    slot2MuteVolume,
                    slot2DarkMode,
                    slot2MuteToZero,
                    slot2AlertPersist,
                    slot2PriorityArrow,
                    slot2_comfort,
                };
            default:
                return ConstAutoPushSlotView{
                    slot0Name,
                    slot0Color,
                    slot0Volume,
                    slot0MuteVolume,
                    slot0DarkMode,
                    slot0MuteToZero,
                    slot0AlertPersist,
                    slot0PriorityArrow,
                    slot0_default,
                };
        }
    }
};

struct SettingsBackupApplyResult {
    bool success = false;
    int profilesRestored = 0;
};

enum class SettingsPersistMode : uint8_t {
    Immediate,
    Deferred,
};

struct DeviceSettingsUpdate {
    bool hasApCredentials = false;
    String apSSID;
    String apPassword;

    bool hasProxyBLE = false;
    bool proxyBLE = false;

    bool hasProxyName = false;
    String proxyName;

    bool hasAutoPowerOffMinutes = false;
    uint8_t autoPowerOffMinutes = 0;

    bool hasApTimeoutMinutes = false;
    uint8_t apTimeoutMinutes = 0;

    bool hasEnableWifiAtBoot = false;
    bool enableWifiAtBoot = false;
};

struct AudioSettingsUpdate {
    bool hasVoiceAlertMode = false;
    VoiceAlertMode voiceAlertMode = VOICE_MODE_DISABLED;

    bool hasVoiceDirectionEnabled = false;
    bool voiceDirectionEnabled = false;

    bool hasAnnounceBogeyCount = false;
    bool announceBogeyCount = false;

    bool hasMuteVoiceIfVolZero = false;
    bool muteVoiceIfVolZero = false;

    bool hasVoiceVolume = false;
    uint8_t voiceVolume = 0;

    bool hasAnnounceSecondaryAlerts = false;
    bool announceSecondaryAlerts = false;

    bool hasSecondaryLaser = false;
    bool secondaryLaser = false;

    bool hasSecondaryKa = false;
    bool secondaryKa = false;

    bool hasSecondaryK = false;
    bool secondaryK = false;

    bool hasSecondaryX = false;
    bool secondaryX = false;

    bool hasAlertVolumeFadeEnabled = false;
    bool alertVolumeFadeEnabled = false;

    bool hasAlertVolumeFadeDelaySec = false;
    uint8_t alertVolumeFadeDelaySec = 0;

    bool hasAlertVolumeFadeVolume = false;
    uint8_t alertVolumeFadeVolume = 0;

    bool hasSpeedMuteEnabled = false;
    bool speedMuteEnabled = false;

    bool hasSpeedMuteThresholdMph = false;
    uint8_t speedMuteThresholdMph = 0;

    bool hasSpeedMuteHysteresisMph = false;
    uint8_t speedMuteHysteresisMph = 0;

    bool hasSpeedMuteVolume = false;
    uint8_t speedMuteVolume = 0xFF;      // 0xFF = voice-only (no V1 volume change)
};

struct DisplaySettingsUpdate {
    bool hasColorBogey = false;
    uint16_t colorBogey = 0;
    bool hasColorFrequency = false;
    uint16_t colorFrequency = 0;
    bool hasColorArrowFront = false;
    uint16_t colorArrowFront = 0;
    bool hasColorArrowSide = false;
    uint16_t colorArrowSide = 0;
    bool hasColorArrowRear = false;
    uint16_t colorArrowRear = 0;
    bool hasColorBandL = false;
    uint16_t colorBandL = 0;
    bool hasColorBandKa = false;
    uint16_t colorBandKa = 0;
    bool hasColorBandK = false;
    uint16_t colorBandK = 0;
    bool hasColorBandX = false;
    uint16_t colorBandX = 0;
    bool hasColorBandPhoto = false;
    uint16_t colorBandPhoto = 0;
    bool hasColorWiFiIcon = false;
    uint16_t colorWiFiIcon = 0;
    bool hasColorWiFiConnected = false;
    uint16_t colorWiFiConnected = 0;
    bool hasColorBleConnected = false;
    uint16_t colorBleConnected = 0;
    bool hasColorBleDisconnected = false;
    uint16_t colorBleDisconnected = 0;
    bool hasColorBar1 = false;
    uint16_t colorBar1 = 0;
    bool hasColorBar2 = false;
    uint16_t colorBar2 = 0;
    bool hasColorBar3 = false;
    uint16_t colorBar3 = 0;
    bool hasColorBar4 = false;
    uint16_t colorBar4 = 0;
    bool hasColorBar5 = false;
    uint16_t colorBar5 = 0;
    bool hasColorBar6 = false;
    uint16_t colorBar6 = 0;
    bool hasColorMuted = false;
    uint16_t colorMuted = 0;
    bool hasColorPersisted = false;
    uint16_t colorPersisted = 0;
    bool hasColorVolumeMain = false;
    uint16_t colorVolumeMain = 0;
    bool hasColorVolumeMute = false;
    uint16_t colorVolumeMute = 0;
    bool hasColorRssiV1 = false;
    uint16_t colorRssiV1 = 0;
    bool hasColorRssiProxy = false;
    uint16_t colorRssiProxy = 0;
    bool hasColorObd = false;
    uint16_t colorObd = 0;
    bool hasFreqUseBandColor = false;
    bool freqUseBandColor = false;
    bool hasHideWifiIcon = false;
    bool hideWifiIcon = false;
    bool hasHideProfileIndicator = false;
    bool hideProfileIndicator = false;
    bool hasHideBatteryIcon = false;
    bool hideBatteryIcon = false;
    bool hasShowBatteryPercent = false;
    bool showBatteryPercent = false;
    bool hasHideBleIcon = false;
    bool hideBleIcon = false;
    bool hasHideVolumeIndicator = false;
    bool hideVolumeIndicator = false;
    bool hasHideRssiIndicator = false;
    bool hideRssiIndicator = false;
    bool hasBrightness = false;
    uint8_t brightness = 0;
    bool hasDisplayStyle = false;
    DisplayStyle displayStyle = DISPLAY_STYLE_CLASSIC;
};

struct ObdSettingsUpdate {
    bool hasEnabled = false;
    bool enabled = false;

    bool hasMinRssi = false;
    int8_t minRssi = -80;

    bool hasSavedAddress = false;
    String savedAddress;

    bool hasSavedName = false;
    String savedName;

    bool hasSavedAddrType = false;
    uint8_t savedAddrType = 0;

    bool hasCachedVinPrefix11 = false;
    String cachedVinPrefix11;

    bool hasCachedEotProfileId = false;
    uint8_t cachedEotProfileId = 0;

    bool resetSavedNameOnAddressChange = false;
};

struct AutoPushSlotUpdate {
    int slot = 0;

    bool hasName = false;
    String name;

    bool hasColor = false;
    uint16_t color = 0;

    bool hasVolume = false;
    uint8_t volume = 0xFF;

    bool hasMuteVolume = false;
    uint8_t muteVolume = 0xFF;

    bool hasDarkMode = false;
    bool darkMode = false;

    bool hasMuteToZero = false;
    bool muteToZero = false;

    bool hasAlertPersist = false;
    uint8_t alertPersist = 0;

    bool hasPriorityArrowOnly = false;
    bool priorityArrowOnly = false;

    bool hasProfileName = false;
    String profileName;

    bool hasMode = false;
    V1Mode mode = V1_MODE_UNKNOWN;
};

struct AutoPushStateUpdate {
    bool hasActiveSlot = false;
    int activeSlot = 0;

    bool hasEnabled = false;
    bool enabled = false;
};

class SettingsManager {
public:
    SettingsManager();

    // Initialize and load settings
    void begin();

    // Get current settings (read-only)
    const V1Settings& get() const { return settings_; }
#ifdef UNIT_TEST
    // Test-only mutable access for fixture seeding.
    V1Settings& mutableSettings() { return settings_; }
#endif
    uint32_t backupRevision() const { return backupRevisionCounter_; }

    // Update settings (calls save automatically)
    void setWiFiEnabled(bool enabled);
    void setAPCredentials(const String& ssid, const String& password);
    void setProxyBLE(bool enabled);
    void setProxyName(const String& name);
    void setAutoPowerOffMinutes(uint8_t minutes);
    void setApTimeoutMinutes(uint8_t minutes);
    uint8_t getApTimeoutMinutes() const { return settings_.apTimeoutMinutes; }
    void setBrightness(uint8_t brightness);
    void setDisplayOff(bool off);
    void setAutoPushEnabled(bool enabled);
    void setActiveSlot(int slot);
    void setSlot(int slotNum, const String& profileName, V1Mode mode);
    void setSlotName(int slotNum, const String& name);
    void setSlotColor(int slotNum, uint16_t color);
    void setSlotVolumes(int slotNum, uint8_t volume, uint8_t muteVolume);
    void setDisplayColors(uint16_t bogey, uint16_t freq, uint16_t arrowFront, uint16_t arrowSide, uint16_t arrowRear,
                          uint16_t bandL, uint16_t bandKa, uint16_t bandK, uint16_t bandX, bool deferSave = false);
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
    void setFreqUseBandColor(bool use);
    void setHideWifiIcon(bool hide);
    void setHideProfileIndicator(bool hide);
    void setHideBatteryIcon(bool hide);
    void setShowBatteryPercent(bool show);
    void setHideBleIcon(bool hide);
    void setHideVolumeIndicator(bool hide);
    void setHideRssiIndicator(bool hide);
    void setEnableWifiAtBoot(bool enable, bool deferSave = false);
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
    void setSpeedMute(bool enabled, uint8_t thresholdMph, uint8_t hysteresisMph);
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

    void applyDeviceSettingsUpdate(const DeviceSettingsUpdate& update,
                                   SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    void applyAudioSettingsUpdate(const AudioSettingsUpdate& update,
                                  SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    void applyDisplaySettingsUpdate(const DisplaySettingsUpdate& update,
                                    SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    void resetDisplaySettings(SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    bool applyObdSettingsUpdate(const ObdSettingsUpdate& update,
                                SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    bool applyAutoPushSlotUpdate(const AutoPushSlotUpdate& update,
                                 SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    bool applyAutoPushStateUpdate(const AutoPushStateUpdate& update,
                                  SettingsPersistMode persistMode = SettingsPersistMode::Immediate);

    // Batch update methods (don't auto-save, call save() after)
    void updateBrightness(uint8_t brightness) { settings_.brightness = brightness; }
    void updateVoiceVolume(uint8_t volume) { settings_.voiceVolume = volume; }

    // Save all settings to flash
    void save();
    void saveDeferredBackup();
    void requestDeferredPersist();
    void serviceDeferredPersist(uint32_t nowMs);
    bool deferredPersistPending() const;
    bool deferredPersistRetryScheduled() const;
    uint32_t deferredPersistNextAttemptAtMs() const;

    // Load settings from flash (public for testing)
    void load();

    // WiFi client (STA) settings - connect to external network
    String getWifiClientPassword();  // Retrieves from secure NVS namespace
    void setWifiClientEnabled(bool enabled);
    void setWifiClientCredentials(const String& ssid, const String& password);
    void clearWifiClientCredentials();  // Forget saved network

    // SD card backup/restore for display settings
    bool backupToSD();
    void requestDeferredBackupFromCurrentState();
    void serviceDeferredBackup(uint32_t nowMs);
    bool deferredBackupPending() const;
    bool deferredBackupRetryScheduled() const;
    uint32_t deferredBackupNextAttemptAtMs() const;
    SettingsBackupApplyResult applyBackupDocument(const JsonDocument& doc,
                                                  bool deferBackupRewrite);
    bool restoreFromSD();
    bool checkAndRestoreFromSD();  // Call after storage is mounted to retry restore

    // Validate profile references exist - clear invalid ones
    void validateProfileReferences(V1ProfileManager& profileMgr);

private:
    V1Settings settings_;
    Preferences preferences_;
    uint32_t backupRevisionCounter_ = 1;
    bool deferredPersistPending_ = false;
    bool deferredPersistRetryScheduled_ = false;
    uint32_t deferredPersistNextAttemptAtMs_ = 0;
    void bumpBackupRevision();
    void clearDeferredPersistState();
    bool persistSettingsAtomically();
    bool writeSettingsToNamespace(const char* ns);
    String getActiveNamespace();
    String getStagingNamespace(const String& activeNamespace);
    bool checkNeedsRestore();  // Returns true if NVS appears to be default/empty
    void cleanupNamespacesIfNeeded(bool hasSdBackup);
};

// Global settings instance
extern SettingsManager settingsManager;
#endif  // SETTINGS_H

/**
 * Display Driver for V1 Gen2 Radar Detector Interface
 * Target: Waveshare ESP32-S3-Touch-LCD-3.49 (172x640 AMOLED, AXS15231B)
 *
 * Features:
 * - Multiple color themes (Standard/HighContrast/Stealth/Business)
 * - Custom 7-segment and 14-segment displays
 * - Live alert visualization with signal bars
 * - Status indicators (WiFi, BLE proxy, mute)
 *
 * Display Modes:
 * - Idle/Resting: Logo or blank screen
 * - Alert: Frequency, band, signal strength, direction
 * - Status: Connection info, bogey count
 *
 * Threading: All draw operations must be called from main thread
 * Display updates throttled to ~10 FPS max for performance
 *
 * Ownership: production firmware uses a single global V1Display instance.
 * Some hot-path render tracking is still singleton-scoped behind this class
 * so runtime behavior is singleton-oriented even though the API is object-shaped.
 */

#pragma once
#ifndef DISPLAY_H
#define DISPLAY_H

#include <cstdint>
#include <memory>

// Include display driver abstraction (Arduino_GFX only)
#include "display_driver.h"
#include "packet_parser.h"
#include "../include/color_themes.h"
#include "../include/display_layout.h"  // Centralized layout constants
#include "../include/display_ble_context.h"

enum class PerfDisplayScreen : uint8_t;

class V1Display {
public:
    V1Display();
    ~V1Display();

    // Initialize display
    bool begin();

    // Update display with current state
    void update(const DisplayState& state);
    // Multi-alert display: shows priority alert + secondary alert cards
    void update(const AlertData& priority, const AlertData* allAlerts, int alertCount, const DisplayState& state);

    // Lightweight frequency-only refresh (minimal redraw)
    void refreshFrequencyOnly(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar = false);
    // Lightweight secondary cards-only refresh (minimal redraw)
    void refreshSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted = false);

    // Persisted alert display (shows last alert in dark grey after V1 clears it)
    void updatePersisted(const AlertData& alert, const DisplayState& state);

    // Check if currently in persisted mode (for color selection)
    bool isPersistedMode() const { return persistedMode_; }

    // Show connection status
    void showDisconnected();
    void showResting(bool forceRedraw = false); // idle/rest screen
    void showScanning(); // scanning screen (like resting but with SCAN text)

    // Force next update() call to fully redraw (use after settings change)
    void forceNextRedraw();

    // Reset singleton-scoped render tracking (call on V1 disconnect to ensure
    // the single production display path reconnects with a clean redraw state).
    static void resetChangeTracking();

    void showBootSplash();
    void showShutdown();       // Shutdown screen with goodbye message
    void showLowBattery();     // Critical low battery warning

    // Set brightness (0-255)
    void setBrightness(uint8_t level);

    // Settings adjustment overlay (brightness + voice volume)
    void showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel); // Show both sliders
    void updateSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider);  // Update both sliders
    void hideBrightnessSlider();                                           // Hide slider and restore display
    int getActiveSliderFromTouch(int16_t touchY);                          // Returns 0=brightness, 1=volume, -1=none

    // Clear screen
    void clear();

    // Utility
    const char* bandToString(Band band);
    uint16_t getBandColor(Band band);


    // Color theme helpers
    void updateColorTheme();  // Update colors from settings
    const ColorPalette& getCurrentPalette() const { return currentPalette_; }

    // Profile indicator
    void drawProfileIndicator(int slot);  // 0=Default, 1=Highway, 2=Comfort
    void setProfileIndicatorSlot(int slot);

    // Battery indicator (only shows when on battery power)
    void drawBatteryIndicator();

    // Lockout indicator — shows "L" badge when enforcer matches a zone.
    // Called from lockout/display orchestration modules before display pipeline.
    void setLockoutIndicator(bool show);

    // GPS satellite indicator — shows "G" + sat count when GPS has fix.
    // State is refreshed by display lightweight update paths.
    void setGpsSatellites(bool enabled, bool hasFix, uint8_t satellites);

    // Pre-quiet active flag — suppresses VOL 0 warning when volume was
    // intentionally dropped by the lockout pre-quiet feature.
    void setPreQuietActive(bool active);

    // Speed-vol zero active flag — suppresses VOL 0 warning when speed mute
    // intentionally set the V1 volume to 0.
    void setSpeedVolZeroActive(bool active);

    // BLE context snapshot — populated by the loop orchestration path so display
    // files never depend on extern V1BLEClient. Freshness is tracked internally.
    void setBleContext(const DisplayBleContext& ctx);

    // BLE proxy indicator (blue = advertising/no client, green = client connected)
    // receivingData dims the icon when connected but no V1 packets received recently
    void setBLEProxyStatus(bool proxyEnabled, bool clientConnected, bool receivingData = true);

    // WiFi indicator (shows when connected to STA network)
    void drawWiFiIndicator();
    void refreshObdIndicator(uint32_t nowMs);
    void setObdAttention(bool attention);

    // Flush canvas to physical display
    void flush();
    void flushRegion(int16_t x, int16_t y, int16_t w, int16_t h);  // Partial flush to reduce SPI traffic

private:
    enum class ScreenMode { Unknown, Resting, Scanning, Disconnected, Live, Persisted };
    static PerfDisplayScreen perfScreenForMode(ScreenMode mode);

    // Display driver (Arduino_GFX)
    std::unique_ptr<Arduino_ESP32QSPI> bus_;
    std::unique_ptr<Arduino_AXS15231B> gfxPanel_;
    std::unique_ptr<Arduino_Canvas> tft_;  // Canvas for rotation/buffering

    DisplayState lastState_;
    AlertData lastAlert_;

    // Color palette
    ColorPalette currentPalette_;  // Store current theme palette

    // Drawing helpers
    void drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits = 0);


    void drawFrequency(uint32_t freqMHz, Band band = BAND_NONE, bool muted = false, bool isPhotoRadar = false);
    void drawFrequencyClassic(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar = false);   // 7-segment style
    void drawFrequencySerpentine(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar = false);// Serpentine font
    void markFrequencyDirtyRegion(int16_t x, int16_t y, int16_t w, int16_t h);
    void drawVolumeZeroWarning();  // Flash "VOL 0" warning when volume=0 and no app connected
    void drawStatusText(const char* text, uint16_t color);
    void drawBLEProxyIndicator();
    void drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits = 0, uint16_t frontColorOverride = 0);
    void drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band = BAND_KA, bool muted = false);
    void drawBaseFrame();
    void prepareFullRedrawNoClear();
    void drawTopCounter(char symbol, bool muted, bool showDot);
    void drawTopCounterClassic(char symbol, bool muted, bool showDot);       // 7-segment style (used for all styles)
    void drawStatusStrip(const DisplayState& state, char topChar, bool topMuted, bool topDot);
    void updateStatusStripIncremental(const DisplayState& state,
                                      char topChar,
                                      bool topMuted,
                                      bool topDot,
                                      bool volumeChanged,
                                      bool rssiNeedsUpdate,
                                      bool bogeyCounterChanged,
                                      uint8_t& lastMainVol,
                                      uint8_t& lastMuteVol,
                                      uint8_t& lastBogeyByte,
                                      unsigned long now,
                                      bool& flushLeftStrip,
                                      bool& flushRightStrip);
    void drawVolumeIndicator(uint8_t mainVol, uint8_t muteVol);              // "5V  0M" style
    void drawRssiIndicator(int rssi);                                         // BLE RSSI in dBm
    void drawMuteIcon(bool muted);
    void drawLockoutIndicator();
    void drawGpsIndicator();
    void drawObdIndicator();
    void syncTopIndicators(uint32_t nowMs);
    void setObdStatus(bool enabled, bool connected, bool scanAttention = false);
    bool hasFreshBleContext(uint32_t nowMs) const;
    int measureSevenSegmentText(const char* text, float scale) const;
    int drawSevenSegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);
    void drawSevenSegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    void draw14SegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    int draw14SegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);


    // Multi-alert card row
    void drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted = false);
    // Use centralized constant from display_layout.h
    static constexpr int SECONDARY_ROW_HEIGHT = DisplayLayout::SECONDARY_ROW_HEIGHT;

    int currentProfileSlot_ = 0;  // Track current profile for display
    ScreenMode currentScreen_ = ScreenMode::Unknown;  // Track current screen to avoid redundant full redraws
    uint32_t paletteRevision_ = 0;                    // Incremented on theme change to trigger redraws
    uint32_t lastRestingPaletteRevision_ = 0;         // Palette revision last used for resting screen
    int lastRestingProfileSlot_ = -1;                 // Last profile shown on resting screen

    // Visibility timeout tracking
    unsigned long wifiConnectedTime_ = 0;    // When WiFi became connected
    unsigned long profileChangedTime_ = 0;   // When profile was last changed
    bool wifiWasConnected_ = false;          // Track WiFi connection state changes
    int lastProfileSlot_ = -1;               // Track profile changes
    bool bleProxyEnabled_ = false;           // BLE proxy enabled flag
    bool bleProxyClientConnected_ = false;   // BLE proxy client connection flag
    bool bleReceivingData_ = true;           // True when V1 packets received recently (heartbeat)
    bool bleProxyDrawn_ = false;             // Track if icon has been drawn at least once
    bool multiAlertMode_ = false;            // True when showing secondary alert cards (reduces main area)
    bool persistedMode_ = false;              // True when drawing persisted alerts (uses PALETTE_PERSISTED)
    bool wasInMultiAlertMode_ = false;       // Track mode transitions for change detection
    bool frequencyRenderDirty_ = false;      // Set when drawFrequency changed pixels this call
    bool frequencyDirtyValid_ = false;       // True when a minimal dirty region is available
    int16_t frequencyDirtyX_ = 0;
    int16_t frequencyDirtyY_ = 0;
    int16_t frequencyDirtyW_ = 0;
    int16_t frequencyDirtyH_ = 0;
    bool secondaryCardsRenderDirty_ = false; // True when drawSecondaryAlertCards changed card-row pixels
    bool lockoutIndicatorShown_ = false;  // Current lockout indicator state (set by orchestration modules)
    bool preQuietActive_ = false;          // Suppress VOL 0 warning during lockout pre-quiet
    bool speedVolZeroActive_ = false;      // Suppress VOL 0 warning during speed-mute vol 0
    bool gpsSatEnabled_ = false;           // GPS module enabled
    bool gpsSatHasFix_ = false;            // GPS has satellite fix
    uint8_t gpsSatCount_ = 0;              // Satellite count for display
    bool obdEnabled_ = false;              // OBD module enabled
    bool obdConnected_ = false;            // OBD adapter connected
    bool obdScanAttention_ = false;        // Runtime manual scan / scan-pending state
    bool obdAttention_ = false;            // Temporary UI hold-time attention
    DisplayBleContext bleCtx_;              // BLE state snapshot for display DI
    uint32_t bleCtxUpdatedAtMs_ = 0;        // When setBleContext() last refreshed bleCtx_

    static const unsigned long HIDE_TIMEOUT_MS = 3000;  // 3 second display timeout

#ifdef UNIT_TEST
public:
    // Test seam: inject a recording canvas and expose it for assertions.
    // The display takes ownership; caller must allocate with new.
    void setTestCanvas(Arduino_Canvas* canvas) { tft_.reset(canvas); }
    Arduino_Canvas* testCanvas() { return tft_.get(); }
    // Public wrappers for private rendering methods (native integration tests only)
    void ut_drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits = 0) {
        drawBandIndicators(bandMask, muted, bandFlashBits);
    }
    void ut_drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength,
                                    Band band = BAND_KA, bool muted = false) {
        drawVerticalSignalBars(frontStrength, rearStrength, band, muted);
    }
    void ut_drawDirectionArrow(Direction dir, bool muted,
                               uint8_t flashBits = 0, uint16_t frontColorOverride = 0) {
        drawDirectionArrow(dir, muted, flashBits, frontColorOverride);
    }
    void ut_drawLockoutIndicator() { drawLockoutIndicator(); }
    void ut_drawGpsIndicator()     { drawGpsIndicator(); }
    void ut_drawObdIndicator()     { drawObdIndicator(); }
    void ut_drawBaseFrame()        { drawBaseFrame(); }
    void ut_setObdStatus(bool enabled, bool connected, bool scanAttention = false) {
        setObdStatus(enabled, connected, scanAttention);
    }
    void ut_drawVolumeIndicator(uint8_t mainVol, uint8_t muteVol) {
        drawVolumeIndicator(mainVol, muteVol);
    }
    void ut_drawRssiIndicator(int rssi) {
        drawRssiIndicator(rssi);
    }
    void ut_drawBatteryIndicator() { drawBatteryIndicator(); }
    void ut_drawBLEProxyIndicator() { drawBLEProxyIndicator(); }
    void ut_drawWiFiIndicator() { drawWiFiIndicator(); }
#endif
};

// Global display instance (defined in main.cpp)
extern V1Display display;

#endif // DISPLAY_H

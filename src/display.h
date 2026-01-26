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
 */

#ifndef DISPLAY_H
#define DISPLAY_H

// Include display driver abstraction (Arduino_GFX only)
#include "display_driver.h"
#include "packet_parser.h"
#include "../include/color_themes.h"

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
    
    // Persisted alert display (shows last alert in dark grey after V1 clears it)
    void updatePersisted(const AlertData& alert, const DisplayState& state);
    
    // Check if currently in persisted mode (for color selection)
    bool isPersistedMode() const { return persistedMode; }

    // Show connection status
    void showDisconnected();
    void showResting(bool forceRedraw = false); // idle/rest screen
    void showScanning(); // scanning screen (like resting but with SCAN text)
    
    // Force next update() call to fully redraw (use after settings change)
    void forceNextRedraw();
    
    // Reset change tracking statics (call on V1 disconnect to ensure clean state on reconnect)
    static void resetChangeTracking();

    // Demo mode
    void showDemo();
    void showBootSplash();
    void showShutdown();       // Shutdown screen with goodbye message
    void showLowBattery();     // Critical low battery warning
    
    // Set brightness (0-255)
    void setBrightness(uint8_t level);
    
    // Settings adjustment overlay (brightness + voice volume)
    void showBrightnessSlider(uint8_t brightnessLevel);                    // Show slider overlay
    void showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel); // Show both sliders
    void updateBrightnessSlider(uint8_t level);                            // Update brightness slider
    void updateSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider);  // Update both sliders
    void hideBrightnessSlider();                                           // Hide slider and restore display
    int getActiveSliderFromTouch(int16_t touchY);                          // Returns 0=brightness, 1=volume, -1=none
    
    // Get canvas for direct access (testing)
    Arduino_Canvas* getCanvas() { return tft; }
    
    // Clear screen
    void clear();
    
    // Utility
    const char* bandToString(Band band);
    uint16_t getBandColor(Band band);
    uint16_t getArrowColor(Direction dir);
    
    // Color theme helpers
    void updateColorTheme();  // Update colors from settings
    const ColorPalette& getCurrentPalette() const { return currentPalette; }
    
    // Profile indicator
    void drawProfileIndicator(int slot);  // 0=Default, 1=Highway, 2=Comfort
    
    // Battery indicator (only shows when on battery power)
    void drawBatteryIndicator();

    // Status bar at top of screen (GPS/CAM/OBD indicators)
    void drawStatusBar();

    // Camera alert indicator (shows camera type and distance)
    // When V1 has active alerts, camera shows as a secondary card; otherwise in main frequency area
    void updateCameraAlert(bool active, const char* typeName, float distance_m, bool approaching, uint16_t color, bool v1HasAlerts = false);
    void clearCameraAlert();
    
    // Set camera alert state for secondary card system (called before drawSecondaryAlertCards)
    void setCameraAlertState(bool active, const char* typeName, float distance_m, uint16_t color);

    // BLE proxy indicator (blue = advertising/no client, green = client connected)
    // receivingData dims the icon when connected but no V1 packets received recently
    void setBLEProxyStatus(bool proxyEnabled, bool clientConnected, bool receivingData = true);
    
    // WiFi indicator (shows when connected to STA network)
    void drawWiFiIndicator();
    
    // Lockout mute indicator - shows "LOCKOUT" instead of "MUTED" when V1 was auto-muted by GPS lockout
    void setLockoutMuted(bool lockout) { lockoutMuted = lockout; }
    bool isLockoutMuted() const { return lockoutMuted; }
    
    // Flush canvas to physical display
    void flush();
    void flushRegion(int16_t x, int16_t y, int16_t w, int16_t h);  // Partial flush to reduce SPI traffic

private:
    enum class ScreenMode { Unknown, Resting, Scanning, Disconnected, Live, Persisted };

    // Display driver (Arduino_GFX)
    Arduino_DataBus* bus = nullptr;
    Arduino_GFX* gfxPanel = nullptr;
    Arduino_Canvas* tft = nullptr;  // Canvas for rotation/buffering

    DisplayState lastState;
    AlertData lastAlert;
    
    // Color palette
    ColorPalette currentPalette;  // Store current theme palette
    
    // Drawing helpers
    void drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits = 0);
    void drawBandLabel(Band band, bool muted);
    void drawSignalBars(uint8_t bars);
    void drawFrequency(uint32_t freqMHz, Band band = BAND_NONE, bool muted = false, bool isPhotoRadar = false);
    void drawFrequencyClassic(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar = false);   // 7-segment style
    void drawFrequencyModern(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar = false);    // Montserrat Bold font
    void drawFrequencyHemi(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar = false);      // Hemi Head font (retro speedometer)
    void drawFrequencySerpentine(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar = false);// Serpentine font (JB's favorite)
    void drawVolumeZeroWarning();  // Flash "VOL 0" warning when volume=0 and no app connected
    void drawKittScanner();        // Knight Rider scanner animation for resting screen
    void drawStatusText(const char* text, uint16_t color);
    void drawBLEProxyIndicator();
    void drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits = 0);
    void drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band = BAND_KA, bool muted = false);
    void drawBandBadge(Band band);
    void drawBaseFrame();
    void drawTopCounter(char symbol, bool muted, bool showDot);
    void drawTopCounterClassic(char symbol, bool muted, bool showDot);       // 7-segment style (used for all styles)
    void drawVolumeIndicator(uint8_t mainVol, uint8_t muteVol);              // "5V  0M" style
    void drawRssiIndicator(int rssi);                                         // BLE RSSI in dBm
    void drawMuteIcon(bool muted);
    int measureSevenSegmentText(const char* text, float scale) const;
    int drawSevenSegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);
    void drawSevenSegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    void draw14SegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    int draw14SegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);
    Band pickDominantBand(uint8_t bandMask);
    
    // Multi-alert card row
    void drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted = false);
    static constexpr int SECONDARY_ROW_HEIGHT = 54;  // Height reserved for secondary alert cards (with signal meter)

    int currentProfileSlot = 0;  // Track current profile for display
    ScreenMode currentScreen = ScreenMode::Unknown;  // Track current screen to avoid redundant full redraws
    uint32_t paletteRevision = 0;                    // Incremented on theme change to trigger redraws
    uint32_t lastRestingPaletteRevision = 0;         // Palette revision last used for resting screen
    int lastRestingProfileSlot = -1;                 // Last profile shown on resting screen
    
    // Visibility timeout tracking
    unsigned long wifiConnectedTime = 0;    // When WiFi became connected
    unsigned long profileChangedTime = 0;   // When profile was last changed
    bool wifiWasConnected = false;          // Track WiFi connection state changes
    int lastProfileSlot = -1;               // Track profile changes
    bool bleProxyEnabled = false;           // BLE proxy enabled flag
    bool bleProxyClientConnected = false;   // BLE proxy client connection flag
    bool bleReceivingData = true;           // True when V1 packets received recently (heartbeat)
    bool bleProxyDrawn = false;             // Track if icon has been drawn at least once
    bool multiAlertMode = false;            // True when showing secondary alert cards (reduces main area)
    bool persistedMode = false;              // True when drawing persisted alerts (uses PALETTE_PERSISTED)
    bool lockoutMuted = false;               // True when V1 was muted by GPS lockout system
    bool secondaryCardsNeedRedraw = true;   // Force secondary cards redraw after screen clear
    bool wasInMultiAlertMode = false;       // Track mode transitions for change detection
    
    // Camera alert state for secondary card integration
    bool cameraCardActive = false;          // True when camera should show as secondary card
    char cameraCardTypeName[16] = {0};      // Camera type name for card
    float cameraCardDistance = 0.0f;        // Camera distance for card
    uint16_t cameraCardColor = 0;           // Camera card color
    
    // KITT scanner animation state
    float kittPosition = 0.0f;              // Current scanner position (0.0 to 1.0)
    int kittDirection = 1;                  // Scanner direction: 1=right, -1=left
    unsigned long lastKittUpdate = 0;       // Last scanner animation update time
    
    static const unsigned long HIDE_TIMEOUT_MS = 3000;  // 3 second display timeout
};

// Global display instance (defined in main.cpp)
extern V1Display display;

#endif // DISPLAY_H

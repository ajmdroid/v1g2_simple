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
    void showResting(); // idle/rest screen
    void showScanning(); // scanning screen (like resting but with SCAN text)
    
    // Reset change tracking statics (call on V1 disconnect to ensure clean state on reconnect)
    static void resetChangeTracking();

    // Demo mode
    void showDemo();
    void showBootSplash();
    void showShutdown();       // Shutdown screen with goodbye message
    void showLowBattery();     // Critical low battery warning
    
    // Set brightness (0-255)
    void setBrightness(uint8_t level);
    
    // Brightness adjustment overlay
    void showBrightnessSlider(uint8_t currentLevel);  // Show slider overlay
    void updateBrightnessSlider(uint8_t level);       // Update slider position
    void hideBrightnessSlider();                      // Hide slider and restore display
    
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

    // BLE proxy indicator (blue = advertising/no client, green = client connected)
    void setBLEProxyStatus(bool proxyEnabled, bool clientConnected);
    
    // WiFi indicator (shows when connected to STA network)
    void drawWiFiIndicator();
    
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
    void drawFrequency(uint32_t freqMHz, Band band = BAND_NONE, bool muted = false);
    void drawFrequencyClassic(uint32_t freqMHz, Band band, bool muted);   // 7-segment style
    void drawFrequencyModern(uint32_t freqMHz, Band band, bool muted);    // Montserrat Bold font
    void drawStatusText(const char* text, uint16_t color);
    void drawBLEProxyIndicator();
    void drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits = 0);
    void drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band = BAND_KA, bool muted = false);
    void drawBandBadge(Band band);
    void drawBaseFrame();
    void drawTopCounter(char symbol, bool muted, bool showDot);
    void drawTopCounterClassic(char symbol, bool muted, bool showDot);       // 7-segment style
    void drawTopCounterModern(char symbol, bool muted, bool showDot);        // Montserrat Bold font
    void drawMuteIcon(bool muted);
    int measureSevenSegmentText(const char* text, float scale) const;
    int drawSevenSegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);
    void drawSevenSegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    void draw14SegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    int draw14SegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);
    Band pickDominantBand(uint8_t bandMask);
    
    // Multi-alert card row
    void drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted = false);
    static constexpr int SECONDARY_ROW_HEIGHT = 30;  // Height reserved for secondary alert cards

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
    bool bleProxyDrawn = false;             // Track if icon has been drawn at least once
    bool multiAlertMode = false;            // True when showing secondary alert cards (reduces main area)
    bool persistedMode = false;              // True when drawing persisted alerts (uses PALETTE_PERSISTED)
    bool secondaryCardsNeedRedraw = true;   // Force secondary cards redraw after screen clear
    bool wasInMultiAlertMode = false;       // Track mode transitions for change detection
    static const unsigned long HIDE_TIMEOUT_MS = 3000;  // 3 second display timeout
};

// Global display instance (defined in main.cpp)
extern V1Display display;

#endif // DISPLAY_H

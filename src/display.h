/**
 * Display Driver for V1 Gen2 Radar Detector Interface
 * Target: Waveshare ESP32-S3-Touch-LCD-3.49 (172x640 AMOLED, AXS15231B)
 * 
 * Features:
 * - Multiple color themes (Standard/Dark/RDF/Classic)
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
    void update(const AlertData& alert, bool mutedFlag);
    void update(const AlertData& alert);
    void update(const AlertData& alert, const DisplayState& state, int alertCount);
    
    // Show connection status
    void showConnecting();
    void showConnected();
    void showDisconnected();
    void showResting(); // idle/rest screen

    // Demo mode
    void showDemo();
    void showBootSplash();
    
    // Show V1 Gen2 logo
    void showLogo();
    
    // Set brightness (0-255)
    void setBrightness(uint8_t level);
    void setBluetoothConnected(bool connected);
    
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

private:
    // Display driver (Arduino_GFX)
    Arduino_DataBus* bus = nullptr;
    Arduino_GFX* gfxPanel = nullptr;
    Arduino_Canvas* tft = nullptr;  // Canvas for rotation/buffering

    DisplayState lastState;
    AlertData lastAlert;
    
    // Color palette
    ColorPalette currentPalette;  // Store current theme palette
    
    // Drawing helpers
    void drawBandIndicators(uint8_t bandMask, bool muted);
    void drawBandLabel(Band band, bool muted);
    void drawArrows(Direction arrows);
    void drawSignalBars(uint8_t bars);
    void drawFrequency(uint32_t freqMHz, bool isLaser = false, bool muted = false);
    void drawV1TechLogo();
    void drawStatusText(const char* text, uint16_t color);
    void drawDirectionArrow(Direction dir, bool muted);
    void drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band = BAND_KA, bool muted = false);
    void drawBandBadge(Band band);
    void drawBaseFrame();
    void drawTopCounter(char symbol, bool muted, bool showDot);
    void drawMuteIcon(bool muted);
    int measureSevenSegmentText(const char* text, float scale) const;
    int drawSevenSegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);
    void drawSevenSegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    void draw14SegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    int draw14SegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);
    void drawBitmapLogo(); // New function declaration
    void drawTopHeader();
    void drawBluetoothIcon(bool connected);
    Band pickDominantBand(uint8_t bandMask);

    bool bluetoothConnected = false;
};

// Global display instance (defined in main.cpp)
extern V1Display display;

#endif // DISPLAY_H

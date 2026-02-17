/**
 * Display Driver for V1 Gen2 Display
 * Supports multiple hardware platforms
 */

#include "display.h"
#include "debug_logger.h"
#include "../include/config.h"
#include "../include/display_layout.h"  // Centralized layout constants
#include "../include/color_themes.h"
#include "../include/band_utils.h"
#include "../include/display_segments.h"  // 7/14-segment data tables
#include "v1simple_logo.h"  // Splash screen image (640x172)
#include "settings.h"
#include "battery_manager.h"
#include "wifi_manager.h"
#include "ble_client.h"
#include "obd_handler.h"
#include "storage_manager.h"
#include "audio_beep.h"
#include "perf_metrics.h"
#include <esp_heap_caps.h>
#include <cstring>
#include <algorithm>
#include "../include/FreeSansBold24pt7b.h"  // Custom font for band labels

// Display logging macro - logs to Serial AND debugLogger when Display category enabled
static constexpr bool DISPLAY_DEBUG_LOGS = false;  // Set true for verbose Serial logging
#if defined(DISABLE_DEBUG_LOGGER)
#define DISPLAY_LOG(...) do { } while(0)
#else
#define DISPLAY_LOG(...) do { \
    if (DISPLAY_DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    DBG_LOGF(DebugLogCategory::Display, __VA_ARGS__); \
} while(0)
#endif

// Font rendering — all OpenFontRender instances and caches are owned by
// DisplayFontManager (see display_font_manager.h).
#include "display_font_manager.h"

DisplayFontManager fontMgr;  // Definition; declared extern in display_font_manager.h

// Convenience aliases so that drawing code updates stay concise.
using TextWidthCacheEntry = DisplayFontManager::WidthCacheEntry;

// ============================================================================
// Dirty-flag aggregate — see include/display_dirty_flags.h for struct definition
// ============================================================================
#include "../include/display_dirty_flags.h"

DisplayDirtyFlags dirty;  // definition; header declares extern for sub-modules

// Use centralized constant from display_layout.h
using DisplayLayout::PRIMARY_ZONE_HEIGHT;

// ============================================================================
// Volume-zero warning state machine
// Shows a flashing "VOL 0" warning when the V1 volume is 0, no app is
// connected, and the lockout pre-quiet feature is not active.
// ============================================================================
struct VolumeZeroWarning {
    unsigned long detectedMs      = 0;      // When volume=0 was first detected
    unsigned long warningStartMs  = 0;      // When warning display actually started
    bool          shown           = false;
    bool          acknowledged    = false;

    static constexpr unsigned long DELAY_MS    = 15000;   // Wait before showing
    static constexpr unsigned long DURATION_MS = 10000;   // Show for this long

    /// Reset all state (call when volume goes non-zero or app connects/disconnects).
    void reset() {
        detectedMs     = 0;
        warningStartMs = 0;
        shown          = false;
        acknowledged   = false;
    }

    /// Evaluate the state machine.  Returns true when the warning should be drawn.
    /// @param volZero          true when mainVolume == 0 && hasVolumeData
    /// @param proxyConnected   true when BLE proxy client is connected
    /// @param preQuietActive   true when lockout pre-quiet is suppressing the warning
    /// @param playBeepFn       called once when the warning first appears
    bool evaluate(bool volZero, bool proxyConnected, bool preQuietActive,
                  void (*playBeepFn)() = nullptr) {
        if (!volZero || proxyConnected || preQuietActive) {
            reset();
            return false;
        }
        if (acknowledged) {
            return false;
        }

        const unsigned long now = millis();
        if (detectedMs == 0) {
            detectedMs = now;
        }

        if ((now - detectedMs) < DELAY_MS) {
            return false;
        }

        if (warningStartMs == 0) {
            warningStartMs = now;
            shown = true;
            if (playBeepFn) playBeepFn();
        }

        if ((now - warningStartMs) < DURATION_MS) {
            return true;   // Warning is active — caller should draw
        }

        // Duration expired
        acknowledged = true;
        shown = false;
        return false;
    }

    /// Return true when a flashing redraw is needed even on the incremental path.
    /// Used in the resting-screen early-exit check.
    bool needsFlashRedraw(bool volZero, bool proxyConnected, bool preQuietActive) const {
        if (!volZero || proxyConnected || preQuietActive || acknowledged) {
            return false;
        }
        if (detectedMs == 0) {
            return true;   // Timer not started yet — force full redraw to start it
        }
        if ((millis() - detectedMs) >= DELAY_MS) {
            if (warningStartMs == 0 || (millis() - warningStartMs) < DURATION_MS) {
                return true;
            }
        }
        return shown;
    }
};

static VolumeZeroWarning volZeroWarn;

// External reference to BLE client for checking proxy connection
extern V1BLEClient bleClient;

// getEffectiveScreenHeight() now lives in display_layout.h

// Debug timing for display operations (set to true to profile display)
static constexpr bool DISPLAY_PERF_TIMING = false;  // Disable for production
static unsigned long _dispPerfStart = 0;
#define DISP_PERF_START() do { if (DISPLAY_PERF_TIMING) _dispPerfStart = micros(); } while(0)
#define DISP_PERF_LOG(label) do { if (DISPLAY_PERF_TIMING) { \
    unsigned long _dur = micros() - _dispPerfStart; \
    if (_dur > 5000) Serial.printf("[DISP] %s: %luus\n", label, _dur); \
    _dispPerfStart = micros(); \
} } while(0)

#if defined(DISPLAY_USE_ARDUINO_GFX)
#define DISPLAY_FLUSH() do { \
    if (tft) { \
        uint32_t _start = PERF_TIMESTAMP_US(); \
        tft->flush(); \
        perfRecordFlushUs(PERF_TIMESTAMP_US() - _start); \
    } \
} while(0)
#else
#define DISPLAY_FLUSH() ((void)0)
#endif

// Drawing primitives, coordinate transforms, dimColor — see include/display_draw.h
#include "../include/display_draw.h"

// Platform-specific state kept in display.cpp
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // TFT_BL alias for backlight pin
    #define TFT_BL LCD_BL
#endif

// Global display instance reference — defined here, declared extern in display_palette.h
V1Display* g_displayInstance = nullptr;

// Palette helpers and colour macros — see include/display_palette.h
#include "../include/display_palette.h"

// Cross-platform text drawing helpers — see include/display_text.h
#include "../include/display_text.h"

using namespace DisplaySegments;

namespace {
const char* cameraTokenForType(uint8_t cameraType) {
    switch (cameraType) {
        case 1:  // redlight
            return "REDL";
        case 2:  // speed
            return "SPEED";
        case 3:  // redlight + speed
            return "RSPD";
        case 4:  // alpr
            return "ALPR";
        default:
            return "CAM";
    }
}

// TOP_COUNTER_* constants now live in display_layout.h
using DisplayLayout::TOP_COUNTER_FONT_SIZE;
using DisplayLayout::TOP_COUNTER_FIELD_X;
using DisplayLayout::TOP_COUNTER_FIELD_Y;
using DisplayLayout::TOP_COUNTER_FIELD_W;
using DisplayLayout::TOP_COUNTER_FIELD_H;
using DisplayLayout::TOP_COUNTER_TEXT_Y;
using DisplayLayout::TOP_COUNTER_PAD_RIGHT;
using DisplayLayout::TOP_COUNTER_FALLBACK_WIDTH;

} // namespace

V1Display::V1Display() {
    // Initialize with standard theme by default
    currentPalette = ColorThemes::STANDARD();
    // Set global instance for color palette access
    g_displayInstance = this;

    currentScreen = ScreenMode::Unknown;
    paletteRevision = 0;
    lastRestingPaletteRevision = 0;
    lastRestingProfileSlot = -1;
}

V1Display::~V1Display() {
#if defined(DISPLAY_USE_ARDUINO_GFX)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdelete-non-virtual-dtor"
    if (tft) delete tft;
    if (gfxPanel) delete gfxPanel;
    if (bus) delete bus;
#pragma GCC diagnostic pop
#endif
}

bool V1Display::begin() {
    Serial.printf("[Display] Init %s...\n", DISPLAY_NAME);
    const unsigned long beginStartMs = millis();
    unsigned long stageStartMs = beginStartMs;
    auto logDisplayStage = [&](const char* stageName) {
        const unsigned long now = millis();
        Serial.printf("[Display] Stage %s: %lu ms (total=%lu)\n",
                      stageName,
                      now - stageStartMs,
                      now - beginStartMs);
        stageStartMs = now;
    };
    
#if PIN_POWER_ON >= 0
    // Power was held low in setup(); bring it up now
    digitalWrite(PIN_POWER_ON, HIGH);
    delay(200);
#endif
    
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Arduino_GFX initialization for Waveshare 3.49"
    // Waveshare 3.49" has INVERTED backlight PWM: 0 = full brightness, 255 = off
    pinMode(LCD_BL, OUTPUT);
    analogWrite(LCD_BL, 255);  // Start with backlight off (inverted: 255=off)
    
    // Manual RST toggle with Waveshare timing BEFORE creating bus
    // This is critical - Waveshare examples do: HIGH(30ms) -> LOW(250ms) -> HIGH(30ms)
    pinMode(LCD_RST, OUTPUT);
    digitalWrite(LCD_RST, HIGH);
    delay(30);
    digitalWrite(LCD_RST, LOW);
    delay(250);
    digitalWrite(LCD_RST, HIGH);
    delay(30);
    
    // Create QSPI bus
    bus = new Arduino_ESP32QSPI(
        LCD_CS,    // CS
        LCD_SCLK,  // SCK
        LCD_DATA0, // D0
        LCD_DATA1, // D1
        LCD_DATA2, // D2
        LCD_DATA3  // D3
    );
    if (!bus) {
        Serial.println("[Display] ERROR: Failed to create bus!");
        return false;
    }
    
    // Create AXS15231B panel - native 172x640 portrait
    // Pass GFX_NOT_DEFINED for RST since we already did manual reset
    gfxPanel = new Arduino_AXS15231B(
        bus,               // bus
        GFX_NOT_DEFINED,   // RST - we already did manual reset
        0,                 // rotation (0 = no panel rotation)
        false,             // IPS
        172,               // width (Waveshare 3.49" is 172 wide)
        640,               // height
        0,                 // col_offset1
        0,                 // row_offset1
        0,                 // col_offset2
        0,                 // row_offset2
        axs15231b_180640_init_operations,   // init operations for this panel type
        sizeof(axs15231b_180640_init_operations)
    );
    if (!gfxPanel) {
        Serial.println("[Display] ERROR: Failed to create panel!");
        return false;
    }
    
    // Create canvas as 172x640 native with rotation=1 for landscape (90°)
    tft = new Arduino_Canvas(172, 640, gfxPanel, 0, 0, 1);
    
    if (!tft) {
        Serial.println("[Display] ERROR: Failed to create canvas!");
        return false;
    }
    
    if (!tft->begin()) {
        Serial.println("[Display] ERROR: tft->begin() failed!");
        return false;
    }
    
    tft->fillScreen(COLOR_BLACK);
    DISPLAY_FLUSH();
    
    // Turn on backlight (inverted: 0 = full brightness)
    analogWrite(LCD_BL, 0);  // Full brightness (inverted: 0=on)
    delay(30);
    
#else
    // TFT_eSPI initialization
    TFT_CALL(init)();
    delay(200);
    TFT_CALL(setRotation)(DISPLAY_ROTATION);
    TFT_CALL(fillScreen)(PALETTE_BG); // First clear
    delay(10);
    TFT_CALL(fillScreen)(PALETTE_BG); // Second clear to ensure no white flash
#endif

    delay(10); // Give hardware time to settle
    
#if defined(DISPLAY_USE_ARDUINO_GFX)
    tft->setTextColor(PALETTE_TEXT);
    tft->setTextSize(2);
#else
    TFT_CALL(setTextColor)(PALETTE_TEXT, PALETTE_BG);
    GFX_setTextDatum(MC_DATUM); // Middle center
    TFT_CALL(setTextSize)(2);
#endif

    DISPLAY_LOG("[DISPLAY] Initialized successfully %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    logDisplayStage("hw_init");
    
    // Initialize all OpenFontRender instances via the font manager.
    // Segment7 + TopCounter are loaded immediately; Serpentine is deferred.
    fontMgr.init(tft);
    logDisplayStage("font_init");

    // Debug: dump top-counter glyph bounds for a few reference digits.
    if (fontMgr.topCounterReady) {
        int oneMin = 0, oneMax = 0;
        int twoMin = 0, twoMax = 0;
        int eightMin = 0, eightMax = 0;
        if (fontMgr.getTopCounterBounds('1', false, oneMin, oneMax) &&
            fontMgr.getTopCounterBounds('2', false, twoMin, twoMax) &&
            fontMgr.getTopCounterBounds('8', false, eightMin, eightMax)) {
            Serial.printf("[Display] TopCounter OFR bounds @%dpx: '1'=%d..%d '2'=%d..%d '8'=%d..%d\n",
                          TOP_COUNTER_FONT_SIZE,
                          oneMin, oneMax, twoMin, twoMax, eightMin, eightMax);
        }
    }

    Serial.printf("[Display] OK %dx%d, fonts(seg7/top/serp)=%d/%d/%d\n",
                  SCREEN_WIDTH, SCREEN_HEIGHT,
                  fontMgr.segment7Ready, fontMgr.topCounterReady, fontMgr.serpentineReady);
    
    // Load color theme from settings
    updateColorTheme();
    logDisplayStage("ready");
    
    return true;
}

void V1Display::setBrightness(uint8_t level) {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // PWM brightness control for Arduino_GFX
    // Waveshare 3.49" has INVERTED backlight: 0=full on, 255=off
    #ifdef LCD_BL
    analogWrite(LCD_BL, 255 - level);  // Invert the level
    #endif
#else
    // Simple on/off control for TFT_eSPI (pin doesn't support PWM on all boards)
    #ifdef TFT_BL
    digitalWrite(TFT_BL, level > 0 ? HIGH : LOW);
    #endif
#endif
}

// Combined settings screen with brightness and voice volume sliders
void V1Display::showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel) {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Clear screen to dark background
    tft->fillScreen(0x0000);
    
    // Layout: 640x172 landscape - two horizontal sliders stacked
    const int sliderMargin = 40;
    const int sliderHeight = 10;
    const int sliderWidth = SCREEN_WIDTH - (sliderMargin * 2);  // 560 pixels
    const int sliderX = sliderMargin;
    
    // Brightness slider at top (y=45)
    const int brightnessY = 45;
    // Volume slider lower (y=115)
    const int volumeY = 115;
    
    // Title
    tft->setTextColor(0xFFFF);  // White
    tft->setTextSize(2);
    tft->setCursor((SCREEN_WIDTH - 120) / 2, 5);
    tft->print("SETTINGS");
    
    // === Brightness slider ===
    tft->setTextSize(1);
    tft->setTextColor(0xFFFF);
    tft->setCursor(sliderMargin, brightnessY - 16);
    tft->print("BRIGHTNESS");
    
    // Draw slider track
    tft->drawRect(sliderX - 2, brightnessY - 2, sliderWidth + 4, sliderHeight + 4, 0x4208);
    tft->fillRect(sliderX, brightnessY, sliderWidth, sliderHeight, 0x2104);
    
    // Fill based on brightness level (80-255 range)
    int brightnessFill = ((brightnessLevel - 80) * sliderWidth) / 175;
    tft->fillRect(sliderX, brightnessY, brightnessFill, sliderHeight, 0x07E0);  // Green
    
    // Thumb
    int brightThumbX = sliderX + brightnessFill - 4;
    if (brightThumbX < sliderX) brightThumbX = sliderX;
    if (brightThumbX > sliderX + sliderWidth - 8) brightThumbX = sliderX + sliderWidth - 8;
    tft->fillRect(brightThumbX, brightnessY - 4, 8, sliderHeight + 8, 0xFFFF);
    
    // Percentage text
    char brightStr[8];
    int brightPercent = ((brightnessLevel - 80) * 100) / 175;
    snprintf(brightStr, sizeof(brightStr), "%d%%", brightPercent);
    tft->setCursor(sliderX + sliderWidth + 8, brightnessY);
    tft->print(brightStr);
    
    // === Voice volume slider ===
    tft->setTextColor(0xFFFF);
    tft->setCursor(sliderMargin, volumeY - 16);
    tft->print("VOICE VOLUME");
    
    // Draw slider track
    tft->drawRect(sliderX - 2, volumeY - 2, sliderWidth + 4, sliderHeight + 4, 0x4208);
    tft->fillRect(sliderX, volumeY, sliderWidth, sliderHeight, 0x2104);
    
    // Fill based on volume level (0-100 range)
    int volumeFill = (volumeLevel * sliderWidth) / 100;
    tft->fillRect(sliderX, volumeY, volumeFill, sliderHeight, 0x001F);  // Blue for volume
    
    // Thumb
    int volThumbX = sliderX + volumeFill - 4;
    if (volThumbX < sliderX) volThumbX = sliderX;
    if (volThumbX > sliderX + sliderWidth - 8) volThumbX = sliderX + sliderWidth - 8;
    tft->fillRect(volThumbX, volumeY - 4, 8, sliderHeight + 8, 0xFFFF);
    
    // Percentage text
    char volStr[8];
    snprintf(volStr, sizeof(volStr), "%d%%", volumeLevel);
    tft->setCursor(sliderX + sliderWidth + 8, volumeY);
    tft->print(volStr);
    
    // Instructions at bottom
    tft->setTextSize(1);
    tft->setTextColor(0x8410);  // Gray
    tft->setCursor((SCREEN_WIDTH - 220) / 2, 155);
    tft->print("Touch sliders - BOOT to save");
    
    DISPLAY_FLUSH();
#endif
}

void V1Display::updateSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider) {
    // Apply brightness in real-time for visual feedback
    setBrightness(brightnessLevel);
    showSettingsSliders(brightnessLevel, volumeLevel);
}

// Returns which slider was touched: 0=brightness, 1=volume, -1=none
// Touch Y is inverted relative to display Y:
//   Low touch Y = bottom of display = volume slider
//   High touch Y = top of display = brightness slider
int V1Display::getActiveSliderFromTouch(int16_t touchY) {
    if (touchY <= 60) return 1;   // Volume (bottom of display)
    if (touchY >= 80) return 0;   // Brightness (top of display)
    return -1;  // Dead zone between sliders
}

void V1Display::hideBrightnessSlider() {
    // Just clear - caller will refresh normal display
    clear();
}

void V1Display::clear() {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    tft->fillScreen(PALETTE_BG);
    DISPLAY_FLUSH();
#else
    TFT_CALL(fillScreen)(PALETTE_BG);
#endif
    bleProxyDrawn = false;
}

void V1Display::setBLEProxyStatus(bool proxyEnabled, bool clientConnected, bool receivingData) {
#if defined(DISPLAY_WAVESHARE_349)
    // Detect app disconnect - was connected, now isn't
    // Reset VOL 0 warning state immediately so it can trigger again
    if (bleProxyClientConnected && !clientConnected) {
        volZeroWarn.reset();
    }
    
    // Check if proxy client connection changed - update RSSI display
    bool proxyChanged = (clientConnected != bleProxyClientConnected);
    
    // Check if receiving state changed (for heartbeat visual)
    bool receivingChanged = (receivingData != bleReceivingData);
    
    if (bleProxyDrawn &&
        proxyEnabled == bleProxyEnabled &&
        clientConnected == bleProxyClientConnected &&
        !receivingChanged) {
        return;  // No visual change needed
    }

    bleProxyEnabled = proxyEnabled;
    bleProxyClientConnected = clientConnected;
    bleReceivingData = receivingData;
    drawBLEProxyIndicator();
    
    // Update RSSI display when proxy connection changes
    if (proxyChanged) {
        drawRssiIndicator(bleClient.getConnectionRssi());
    }
    
    flush();
#endif
}

void V1Display::drawBaseFrame() {
    // Clean black background (t4s3-style)
    TFT_CALL(fillScreen)(PALETTE_BG);
    bleProxyDrawn = false;  // Force indicator redraw after full clears
    dirty.setAll();         // Invalidate every element cache after screen clear
    drawBLEProxyIndicator();  // Redraw BLE icon after screen clear
}

// drawSevenSegmentDigit(), measureSevenSegmentText(), drawSevenSegmentText(),
// draw14SegmentDigit(), draw14SegmentText(), drawTopCounterClassic(),
// drawTopCounter() moved to display_top_counter.cpp (Phase 2O)

void V1Display::drawVolumeIndicator(uint8_t mainVol, uint8_t muteVol) {
    // Draw volume indicator below bogey counter: "5V  0M" format
    const V1Settings& s = settingsManager.get();
    const int x = 8;
    // Evenly spaced layout from bogey(67) to bottom(172)
    const int y = 75;
    const int clearW = 75;
    const int clearH = 16;
    
    // Clear the area first - only clear what we need, BLE icon is at y=98
    FILL_RECT(x, y, clearW, clearH, PALETTE_BG);
    
    // Draw main volume in blue, mute volume in yellow (user-configurable colors)
    GFX_setTextDatum(TL_DATUM);  // Top-left alignment
    TFT_CALL(setTextSize)(2);  // Size 2 = ~16px height
    
    // Draw main volume "5V" in main volume color
    char mainBuf[5];  // allow up to three digits plus suffix and null
    snprintf(mainBuf, sizeof(mainBuf), "%dV", mainVol);
    TFT_CALL(setTextColor)(s.colorVolumeMain, PALETTE_BG);
    GFX_drawString(tft, mainBuf, x, y);
    
    // Draw mute volume "0M" in mute volume color, offset to the right
    char muteBuf[5];  // allow up to three digits plus suffix and null
    snprintf(muteBuf, sizeof(muteBuf), "%dM", muteVol);
    TFT_CALL(setTextColor)(s.colorVolumeMute, PALETTE_BG);
    GFX_drawString(tft, muteBuf, x + 36, y);  // Aligned with RSSI number
}

void V1Display::drawRssiIndicator(int rssi) {
    // Check if RSSI indicator is hidden
    const V1Settings& s = settingsManager.get();
    if (s.hideRssiIndicator) {
        return;  // Don't draw anything
    }
    
    // Draw BLE RSSI below volume indicator
    // Shows V1 RSSI and JBV1 RSSI (if connected) stacked vertically
    const int x = 8;
    // Evenly spaced: volume at y=75, height 16, gap 8 -> y=99
    const int y = 99;
    const int lineHeight = 22;  // Increased spacing between V and P lines
    const int clearW = 70;
    const int clearH = lineHeight * 2;  // Room for two lines
    
    // Clear the area first
    FILL_RECT(x, y, clearW, clearH, PALETTE_BG);
    
    // Get both RSSIs
    int v1Rssi = rssi;  // V1 RSSI passed in
    int jbv1Rssi = bleClient.getProxyClientRssi();  // JBV1/phone RSSI
    
    GFX_setTextDatum(TL_DATUM);
    TFT_CALL(setTextSize)(2);  // Match volume text size
    
    // Draw V1 RSSI if connected
    if (v1Rssi != 0) {
        // Draw "V " label with configurable color
        TFT_CALL(setTextColor)(s.colorRssiV1, PALETTE_BG);
        GFX_drawString(tft, "V ", x, y);
        
        // Color code RSSI value: green >= -60, yellow -60 to -80, red < -80
        uint16_t rssiColor;
        if (v1Rssi >= -60) {
            rssiColor = COLOR_GREEN;
        } else if (v1Rssi >= -80) {
            rssiColor = COLOR_YELLOW;
        } else {
            rssiColor = COLOR_RED;
        }
        
        TFT_CALL(setTextColor)(rssiColor, PALETTE_BG);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", v1Rssi);
        GFX_drawString(tft, buf, x + 24, y);  // Offset for "V " width
    }
    
    // Draw JBV1 RSSI below V1 RSSI if connected
    if (jbv1Rssi != 0) {
        // Draw "P " label with configurable color
        TFT_CALL(setTextColor)(s.colorRssiProxy, PALETTE_BG);
        GFX_drawString(tft, "P ", x, y + lineHeight);
        
        // Color code RSSI value
        uint16_t rssiColor;
        if (jbv1Rssi >= -60) {
            rssiColor = COLOR_GREEN;
        } else if (jbv1Rssi >= -80) {
            rssiColor = COLOR_YELLOW;
        } else {
            rssiColor = COLOR_RED;
        }
        
        TFT_CALL(setTextColor)(rssiColor, PALETTE_BG);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", jbv1Rssi);
        GFX_drawString(tft, buf, x + 24, y + lineHeight);  // Offset for "P " width
    }
}

// drawMuteIcon() moved to display_top_counter.cpp (Phase 2O)

void V1Display::setLockoutIndicator(bool show) {
    lockoutIndicatorShown_ = show;
}

void V1Display::setPreQuietActive(bool active) {
    preQuietActive_ = active;
}

void V1Display::drawLockoutIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    static bool lastShown = false;

    if (!dirty.lockout && lockoutIndicatorShown_ == lastShown) {
        return;
    }
    dirty.lockout = false;
    lastShown = lockoutIndicatorShown_;

    // Position: right of the mute badge area.
    // Mute badge:  X = 225..335,  Y = 5,  H = 26.
    // Lockout "L":  X = 340,  Y = 5,  26×26 square badge.
    const int x = 340;
    const int y = 5;
    const int sz = 26;

    if (lockoutIndicatorShown_) {
        // Draw a rounded-rect badge with configurable lockout color.
        const V1Settings& s = settingsManager.get();
        const uint16_t textColor = s.colorLockout;
        const uint16_t fillColor = dimColor(textColor, 45);
        FILL_ROUND_RECT(x, y, sz, sz, 5, fillColor);
        DRAW_ROUND_RECT(x, y, sz, sz, 5, textColor);
        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2);
        TFT_CALL(setTextColor)(textColor, fillColor);
        GFX_drawString(tft, "L", x + sz / 2, y + sz / 2);
    } else {
        FILL_RECT(x, y, sz, sz, PALETTE_BG);
    }
#endif
}

// ---------------------------------------------------------------------------
// GPS satellite indicator ("G" + sat count badge, left of MUTED)
// ---------------------------------------------------------------------------

void V1Display::setGpsSatellites(bool enabled, bool hasFix, uint8_t satellites) {
    gpsSatEnabled_ = enabled;
    gpsSatHasFix_  = hasFix;
    gpsSatCount_   = satellites;
}

void V1Display::drawGpsIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    // Build current desired state: show when GPS enabled and has fix.
    const bool wantShow = gpsSatEnabled_ && gpsSatHasFix_;
    const uint8_t curSats = wantShow ? gpsSatCount_ : 0;

    static bool lastShown = false;
    static uint8_t lastSats = 0;

    if (!dirty.gpsIndicator &&
        wantShow == lastShown && curSats == lastSats) {
        return;
    }
    dirty.gpsIndicator = false;
    lastShown = wantShow;
    lastSats  = curSats;

    // Position: just right of band column (120), left of MUTED (~225).
    const int x  = 125;
    const int y  = 5;
    const int h  = 26;
    const int w  = 50;  // Wide enough for "G" + 2-digit sat count

    if (wantShow) {
        // User-configurable GPS badge colour (matches lockout "L" style).
        const V1Settings& s = settingsManager.get();
        const uint16_t textColor = s.colorGps;
        const uint16_t fillColor = dimColor(textColor, 45);

        FILL_ROUND_RECT(x, y, w, h, 5, fillColor);
        DRAW_ROUND_RECT(x, y, w, h, 5, textColor);

        char buf[6];
        snprintf(buf, sizeof(buf), "G%u", curSats);

        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2);
        TFT_CALL(setTextColor)(textColor, fillColor);
        GFX_drawString(tft, buf, x + w / 2, y + h / 2);
    } else {
        FILL_RECT(x, y, w, h, PALETTE_BG);
    }
#endif
}

// ---------------------------------------------------------------------------
// OBD connected indicator ("OBD" badge, right of lockout "L")
// ---------------------------------------------------------------------------

void V1Display::setObdConnected(bool enabled, bool connected, bool hasData) {
    obdEnabled_   = enabled;
    obdConnected_ = connected;
    obdHasData_   = hasData;
}

void V1Display::drawObdIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const bool wantShow = obdEnabled_ && obdConnected_ && obdHasData_;

    static bool lastShown = false;

    if (!dirty.obdIndicator && wantShow == lastShown) {
        return;
    }
    dirty.obdIndicator = false;
    lastShown = wantShow;

    // Position: right of lockout "L" (ends at X=366), before signal bars (X=440).
    const int x = 375;
    const int y = 5;
    const int h = 26;
    const int w = 52;  // Wide enough for "OBD" at textSize(2)

    if (wantShow) {
        const V1Settings& s = settingsManager.get();
        const uint16_t textColor = s.colorObd;
        const uint16_t fillColor = dimColor(textColor, 45);

        FILL_ROUND_RECT(x, y, w, h, 5, fillColor);
        DRAW_ROUND_RECT(x, y, w, h, 5, textColor);

        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2);
        TFT_CALL(setTextColor)(textColor, fillColor);
        GFX_drawString(tft, "OBD", x + w / 2, y + h / 2);
    } else {
        FILL_RECT(x, y, w, h, PALETTE_BG);
    }
#endif
}

void V1Display::drawProfileIndicator(int slot) {
    // Get custom slot names and colors from settings
    extern SettingsManager settingsManager;
    const V1Settings& s = settingsManager.get();

    // Track current slot and timestamp for flash feature
    if (slot != lastProfileSlot) {
        lastProfileSlot = slot;
        profileChangedTime = millis();  // Record when profile changed
    }
    currentProfileSlot = slot;

    // Check if we're in the "flash" period after a profile change
    bool inFlashPeriod = (millis() - profileChangedTime) < HIDE_TIMEOUT_MS;
    
#if defined(DISPLAY_WAVESHARE_349)
    // On Waveshare: draw profile indicator under arrows (for autopush profiles)
    // Arrow center X = 564, profile goes below rear arrow
    int cx = SCREEN_WIDTH - 70 - 6;  // Same as arrow cx
    int y = 152;  // Below arrows
    int clearW = 130;  // Wide enough for profile names at size 2
    int clearH = 20;   // Tall enough for size 2 font
    
    // If user explicitly hides the indicator via web UI, only show during flash period
    if (s.hideProfileIndicator && !inFlashPeriod) {
        FILL_RECT(cx - clearW/2, y, clearW, clearH, PALETTE_BG);
        drawWiFiIndicator();
        drawBatteryIndicator();
        return;
    }

    // Use custom names, fallback to defaults (limited to 20 chars)
    const char* name;
    uint16_t color;
    switch (slot % 3) {
        case 0:
            name = s.slot0Name.length() > 0 ? s.slot0Name.c_str() : "DEFAULT";
            color = s.slot0Color;
            break;
        case 1:
            name = s.slot1Name.length() > 0 ? s.slot1Name.c_str() : "HIGHWAY";
            color = s.slot1Color;
            break;
        default:  // case 2
            name = s.slot2Name.length() > 0 ? s.slot2Name.c_str() : "COMFORT";
            color = s.slot2Color;
            break;
    }

    // Clear area under arrows
    FILL_RECT(cx - clearW/2, y, clearW, clearH, PALETTE_BG);

    // Use built-in font (OFR font subset doesn't have all letters for profile names)
    TFT_CALL(setTextSize)(2);  // Size 2 = ~12px per char
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    int16_t nameWidth = strlen(name) * 12;  // size 2 = ~12px per char
    int textX = cx - nameWidth / 2;
    GFX_setTextDatum(TL_DATUM);
    GFX_drawString(tft, name, textX, y);

    drawWiFiIndicator();
    drawBatteryIndicator();
    setBLEProxyStatus(bleProxyEnabled, bleProxyClientConnected);
    
#else
    // Original position for smaller displays (top area)
    // If user explicitly hides the indicator via web UI, only show during flash period
    if (s.hideProfileIndicator && !inFlashPeriod) {
        int y = 14;
        int clearStart = 120;  // Don't overlap band indicators
        int clearWidth = SCREEN_WIDTH - clearStart - 240;  // stop before signal bars
        FILL_RECT(clearStart, y - 2, clearWidth, 28, PALETTE_BG);

        drawWiFiIndicator();
        drawBatteryIndicator();
        return;
    }

    // Dimensions for clearing/profile centering
    const float freqScale = 1.7f;
    SegMetrics mFreq = segMetrics(freqScale);
    int freqWidth = measureSevenSegmentText("35.500", freqScale);
    const int rightMargin = 120;
    int maxWidth = SCREEN_WIDTH - rightMargin;
    int freqX = (maxWidth - freqWidth) / 2;
    if (freqX < 0) freqX = 0;
    int dotCenterX = freqX + 2 * mFreq.digitW + 2 * mFreq.spacing + mFreq.dot / 2;

    // Use custom names, fallback to defaults (limited to 20 chars)
    const char* name;
    uint16_t color;
    switch (slot % 3) {
        case 0:
            name = s.slot0Name.length() > 0 ? s.slot0Name.c_str() : "DEFAULT";
            color = s.slot0Color;
            break;
        case 1:
            name = s.slot1Name.length() > 0 ? s.slot1Name.c_str() : "HIGHWAY";
            color = s.slot1Color;
            break;
        default:  // case 2
            name = s.slot2Name.length() > 0 ? s.slot2Name.c_str() : "COMFORT";
            color = s.slot2Color;
            break;
    }

    // Measure the profile name text width
    GFX_setTextDatum(TL_DATUM);  // Top-left
    TFT_CALL(setTextSize)(2);    // Larger text for readability
    int16_t nameWidth = strlen(name) * 12;  // Approximate: size 2 = ~12px per char

    // Center the name over the dot position
    int x = dotCenterX - nameWidth / 2;
    if (x < 120) x = 120;  // Don't overlap with band indicators on left (L/Ka/K/X)

    int y = 14;

    // Clear area for profile name - use fixed wide area to prevent artifacts
    int clearEndX = SCREEN_WIDTH - 240;  // Stop before signal bars (at ~412)
    FILL_RECT(120, y - 2, clearEndX - 120, 28, PALETTE_BG);

    // Draw the profile name centered over the dot
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    GFX_drawString(tft, name, x, y);

    // Draw WiFi indicator (if connected to STA network)
    drawWiFiIndicator();

    // Draw battery indicator after profile name (if on battery)
    drawBatteryIndicator();

    // Draw BLE proxy indicator using the latest status
    setBLEProxyStatus(bleProxyEnabled, bleProxyClientConnected);
#endif
}

void V1Display::drawBatteryIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    extern BatteryManager batteryManager;
    extern SettingsManager settingsManager;
    const V1Settings& s = settingsManager.get();
    
    // Battery icon position - VERTICAL at bottom-right
    // Position to the right of profile indicator area, avoiding direction arrows
    const int battW = 14;   // Battery body width (was height when horizontal)
    const int battH = 28;   // Battery body height (was width when horizontal)
    const int battX = SCREEN_WIDTH - battW - 8;  // Right edge with margin
    const int battY = SCREEN_HEIGHT - battH - 8; // Bottom with margin (cap above)
    const int capW = 8;     // Positive terminal cap width (horizontal bar at top)
    const int capH = 3;     // Positive terminal cap height
    
    // Hide battery when on USB power (voltage near max)
    // Use hysteresis to prevent flickering: hide above 4125, show below 4095
    static bool showBatteryOnUSB = true;
    uint16_t voltage = batteryManager.getVoltageMillivolts();
    if (voltage > 4125) {
        showBatteryOnUSB = false;  // On USB or fully charged
    } else if (voltage < 4095) {
        showBatteryOnUSB = true;   // On battery, not full
    }
    // Between 4095-4125: keep previous state (hysteresis)
    
    // Get battery percentage for display
    uint8_t pct = batteryManager.getPercentage();
    
    // If percent is enabled, ONLY show percent (never icon)
    if (s.showBatteryPercent && !s.hideBatteryIcon && batteryManager.hasBattery()) {
        // Cache percent drawing to avoid heavy FreeType work every frame
        static int lastPctDrawn = -1;
        static bool lastPctVisible = false;
        static uint16_t lastPctColor = 0;
        static unsigned long lastPctDrawMs = 0;
        const unsigned long PCT_FORCE_REDRAW_MS = 60000;  // 60s safety refresh

        // Clear battery icon area (we never show icon when percent is enabled)
        FILL_RECT(battX - 2, battY - capH - 4, battW + 4, battH + capH + 6, PALETTE_BG);

        // Only draw percent if not on USB
        if (!showBatteryOnUSB) {
            // Clear percent area when not visible
            if (lastPctVisible) {
                FILL_RECT(SCREEN_WIDTH - 50, 0, 48, 30, PALETTE_BG);
                lastPctVisible = false;
                lastPctDrawn = -1;
            }
            return;  // No percent on USB/fully charged
        }

        // Choose color based on level
        uint16_t textColor;
        if (pct <= 20) {
            textColor = 0xF800;  // Red - critical
        } else if (pct <= 40) {
            textColor = 0xFD20;  // Orange - low
        } else {
            textColor = 0x07E0;  // Green - good
        }
        textColor = dimColor(textColor);

        // Decide if we actually need to redraw
        unsigned long nowMs = millis();
        bool needsRedraw = dirty.battery ||  // Screen was cleared
                           (!lastPctVisible) ||
                           (pct != lastPctDrawn) ||
                           (textColor != lastPctColor) ||
                           ((nowMs - lastPctDrawMs) >= PCT_FORCE_REDRAW_MS);

        if (!needsRedraw) {
            return;  // Skip expensive render when nothing changed
        }
        
        // Clear force flag - we're handling it
        dirty.battery = false;

        // Format percentage string (no % to save space)
        char pctStr[4];
        snprintf(pctStr, sizeof(pctStr), "%d", pct);

        // Always clear the text area before drawing (covers shorter numbers)
        // Positioned to avoid top arrow which extends to roughly SCREEN_WIDTH - 14
        FILL_RECT(SCREEN_WIDTH - 50, 0, 48, 30, PALETTE_BG);

        // Draw using OpenFontRender if available; fall back to built-in font otherwise
        if (fontMgr.modernReady) {
            const int fontSize = 20;  // Larger for better visibility
            uint8_t bgR = (PALETTE_BG >> 11) << 3;
            uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
            uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
            fontMgr.modern.setBackgroundColor(bgR, bgG, bgB);
            fontMgr.modern.setFontColor((textColor >> 11) << 3, ((textColor >> 5) & 0x3F) << 2, (textColor & 0x1F) << 3);
            fontMgr.modern.setFontSize(fontSize);

            FT_BBox bbox = fontMgr.modern.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, pctStr);
            int textW = bbox.xMax - bbox.xMin;
            [[maybe_unused]] int textH = bbox.yMax - bbox.yMin;

            // Position text at top-right corner - use fixed Y position near top
            int textX = SCREEN_WIDTH - textW - 4;
            int textY = 2;  // Fixed: 2 pixels from top (OFR draws from top of glyph, not baseline)

            fontMgr.modern.setCursor(textX, textY);
            fontMgr.modern.printf("%s", pctStr);
        } else {
            // Fallback: built-in font, right-aligned near top-right
            GFX_setTextDatum(TR_DATUM);
            TFT_CALL(setTextSize)(2);  // Larger for better visibility
            TFT_CALL(setTextColor)(textColor, PALETTE_BG);
            GFX_drawString(tft, pctStr, SCREEN_WIDTH - 4, 12);
        }

        // Update cache
        lastPctDrawn = pct;
        lastPctColor = textColor;
        lastPctVisible = true;
        lastPctDrawMs = nowMs;
        return;  // Never draw icon when percent is enabled
    }
    
    // Percent is disabled, show icon instead
    // Clear percent area (in case it was previously showing)
    FILL_RECT(SCREEN_WIDTH - 50, 0, 48, 30, PALETTE_BG);
    
    // Don't draw icon if no battery, user hides it, or on USB
    if (!batteryManager.hasBattery() || s.hideBatteryIcon || !showBatteryOnUSB) {
        FILL_RECT(battX - 2, battY - capH - 4, battW + 4, battH + capH + 6, PALETTE_BG);
        return;
    }
    
    const int padding = 2;  // Padding inside battery
    const int sections = 5; // Number of charge sections
    
    int filledSections = (pct + 10) / 20;  // 0-20%=1, 21-40%=2, etc. (min 1 if >0)
    if (pct == 0) filledSections = 0;
    if (filledSections > sections) filledSections = sections;
    
    // Choose color based on level
    uint16_t fillColor;
    if (pct <= 20) {
        fillColor = 0xF800;  // Red - critical
    } else if (pct <= 40) {
        fillColor = 0xFD20;  // Orange - low
    } else {
        fillColor = 0x07E0;  // Green - good
    }
    
    // Clear area (including cap above)
    FILL_RECT(battX - 2, battY - capH - 4, battW + 4, battH + capH + 6, PALETTE_BG);
    
    uint16_t outlineColor = dimColor(PALETTE_TEXT);

    // Draw battery outline (dimmed) - vertical orientation
    DRAW_RECT(battX, battY, battW, battH, outlineColor);  // Main body
    // Positive cap at top, centered
    FILL_RECT(battX + (battW - capW) / 2, battY - capH, capW, capH, outlineColor);
    
    // Draw charge sections (vertical - bottom to top, filled from bottom)
    int sectionH = (battH - 2 * padding - (sections - 1)) / sections;  // Height of each section with 1px gap
    for (int i = 0; i < sections; i++) {
        // Draw from bottom up: section 0 at bottom, section 4 at top
        int sx = battX + padding;
        int sy = battY + battH - padding - (i + 1) * sectionH - i;  // Bottom-up
        int sw = battW - 2 * padding;
        
        if (i < filledSections) {
            FILL_RECT(sx, sy, sw, sectionH, dimColor(fillColor));
        }
    }
#endif
}

void V1Display::drawBLEProxyIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    // Position to the right of WiFi indicator (side-by-side at bottom-left)
    // Evenly spaced with volume and RSSI above
    const int iconSize = 20;
    const int iconY = 145;  // Moved up 2px from 147
    const int iconGap = 6;  // Gap between icons
    
    const int wifiX = 14;
    const int bleX = wifiX + iconSize + iconGap;  // Right of WiFi icon
    const int bleY = iconY;

    // Always clear the area before drawing
    FILL_RECT(bleX - 2, bleY - 2, iconSize + 4, iconSize + 4, PALETTE_BG);

    if (!bleProxyEnabled) {
        bleProxyDrawn = false;
        return;
    }

    // Check if BLE icon should be hidden
    const V1Settings& s = settingsManager.get();
    if (s.hideBleIcon) {
        bleProxyDrawn = false;
        return;
    }

    // Icon color from settings: connected vs disconnected
    // When connected but not receiving data, dim further to show "stale" state
    uint16_t btColor;
    if (bleProxyClientConnected) {
        // Connected: bright green when receiving, dimmed when stale
        btColor = bleReceivingData ? dimColor(s.colorBleConnected, 85)
                                   : dimColor(s.colorBleConnected, 40);  // Much dimmer when no data
    } else {
        btColor = dimColor(s.colorBleDisconnected, 85);
    }

    // Draw Bluetooth rune - the bind rune of ᛒ (Berkanan) and ᚼ (Hagall)
    // Center point of the icon
    int cx = bleX + iconSize / 2;
    int cy = bleY + iconSize / 2;
    
    int h = iconSize - 2;      // Total height
    int top = cy - h / 2;
    int bot = cy + h / 2;
    int mid = cy;
    
    // Right chevron points - where the arrows reach on the right
    int rightX = cx + 5;
    int topChevronY = mid - 4;   // Upper right point
    int botChevronY = mid + 4;   // Lower right point
    
    // Left arrow endpoints
    int leftX = cx - 5;
    int topArrowY = mid - 4;     // Upper left point  
    int botArrowY = mid + 4;     // Lower left point
    
    // Vertical center line (thicker for visibility)
    FILL_RECT(cx - 1, top, 2, h, btColor);
    
    // === RIGHT SIDE: Two chevrons forming the "B" ===
    // Top chevron: top of line → right point → center (draw 3 lines for thickness)
    DRAW_LINE(cx - 1, top, rightX - 1, topChevronY, btColor);
    DRAW_LINE(cx, top, rightX, topChevronY, btColor);
    DRAW_LINE(cx + 1, top, rightX + 1, topChevronY, btColor);
    DRAW_LINE(rightX - 1, topChevronY, cx - 1, mid, btColor);
    DRAW_LINE(rightX, topChevronY, cx, mid, btColor);
    DRAW_LINE(rightX + 1, topChevronY, cx + 1, mid, btColor);
    
    // Bottom chevron: center → right point → bottom of line (draw 3 lines for thickness)
    DRAW_LINE(cx - 1, mid, rightX - 1, botChevronY, btColor);
    DRAW_LINE(cx, mid, rightX, botChevronY, btColor);
    DRAW_LINE(cx + 1, mid, rightX + 1, botChevronY, btColor);
    DRAW_LINE(rightX - 1, botChevronY, cx - 1, bot, btColor);
    DRAW_LINE(rightX, botChevronY, cx, bot, btColor);
    DRAW_LINE(rightX + 1, botChevronY, cx + 1, bot, btColor);
    
    // === LEFT SIDE: Two arrows forming the "X" through center ===
    // Upper-left arrow (draw 3 lines for thickness)
    DRAW_LINE(leftX - 1, topArrowY, cx - 1, mid, btColor);
    DRAW_LINE(leftX, topArrowY, cx, mid, btColor);
    DRAW_LINE(leftX + 1, topArrowY, cx + 1, mid, btColor);
    
    // Lower-left arrow (draw 3 lines for thickness)
    DRAW_LINE(leftX - 1, botArrowY, cx - 1, mid, btColor);
    DRAW_LINE(leftX, botArrowY, cx, mid, btColor);
    DRAW_LINE(leftX + 1, botArrowY, cx + 1, mid, btColor);

    bleProxyDrawn = true;
#endif
}

void V1Display::drawWiFiIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    extern WiFiManager wifiManager;
    extern SettingsManager settingsManager;
    const V1Settings& s = settingsManager.get();
    
    // WiFi icon position - evenly spaced below RSSI
    const int wifiX = 8;
    const int wifiSize = 20;
    const int wifiY = 145;  // Moved up 2px from 147
    
    // Check if user explicitly hides the WiFi icon
    if (s.hideWifiIcon) {
        FILL_RECT(wifiX - 2, wifiY - 2, wifiSize + 4, wifiSize + 4, PALETTE_BG);
        return;
    }
    
    // Show WiFi icon when Setup Mode (AP) is active
    bool isSetupMode = wifiManager.isSetupModeActive();
    
    if (!isSetupMode) {
        // Clear the WiFi icon area when Setup Mode is off
        FILL_RECT(wifiX - 2, wifiY - 2, wifiSize + 4, wifiSize + 4, PALETTE_BG);
        return;
    }
    
    // Check if any clients are connected to the AP
    bool hasClients = WiFi.softAPgetStationNum() > 0;
    
    // WiFi icon color: connected vs disconnected (like BLE icon)
    uint16_t wifiColor = hasClients ? dimColor(s.colorWiFiConnected, 85)
                                    : dimColor(s.colorWiFiIcon, 85);
    
    // Clear area first
    FILL_RECT(wifiX - 2, wifiY - 2, wifiSize + 4, wifiSize + 4, PALETTE_BG);
    
    // Center point for arcs (bottom center of icon area)
    int cx = wifiX + wifiSize / 2;
    int cy = wifiY + wifiSize - 3;
    
    // Draw center dot (the WiFi source point)
    FILL_RECT(cx - 2, cy - 2, 5, 5, wifiColor);
    
    // Draw 3 concentric arcs above the dot
    // Arc 1 (inner) - small arc
    for (int angle = -45; angle <= 45; angle += 15) {
        float rad = angle * 3.14159 / 180.0;
        int px = cx + (int)(5 * sin(rad));
        int py = cy - 5 - (int)(5 * cos(rad));
        FILL_RECT(px, py, 2, 2, wifiColor);
    }
    
    // Arc 2 (middle)
    for (int angle = -50; angle <= 50; angle += 12) {
        float rad = angle * 3.14159 / 180.0;
        int px = cx + (int)(9 * sin(rad));
        int py = cy - 5 - (int)(9 * cos(rad));
        FILL_RECT(px, py, 2, 2, wifiColor);
    }
    
    // Arc 3 (outer)
    for (int angle = -55; angle <= 55; angle += 10) {
        float rad = angle * 3.14159 / 180.0;
        int px = cx + (int)(13 * sin(rad));
        int py = cy - 5 - (int)(13 * cos(rad));
        FILL_RECT(px, py, 2, 2, wifiColor);
    }
#endif
}

void V1Display::flush() {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    DISPLAY_FLUSH();
#endif
}

void V1Display::flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Constrain region to framebuffer bounds
    if (!tft || !gfxPanel) return;
    int16_t maxW = tft->width();
    int16_t maxH = tft->height();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x >= maxW || y >= maxH) return;
    if (x + w > maxW) w = maxW - x;
    if (y + h > maxH) h = maxH - y;

    uint16_t* fb = tft->getFramebuffer();
    if (!fb) {
        DISPLAY_FLUSH();
        return;
    }

    int16_t stride = tft->width();
    for (int16_t row = 0; row < h; ++row) {
        uint16_t* rowPtr = fb + (y + row) * stride + x;
        gfxPanel->draw16bitRGBBitmap(x, y + row, rowPtr, w, 1);
    }
#else
    (void)x; (void)y; (void)w; (void)h;
#endif
}

void V1Display::markFrequencyDirtyRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (w <= 0 || h <= 0) return;

    // Clamp to screen bounds.
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w <= 0 || h <= 0) return;
    if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) return;
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    if (!frequencyDirtyValid) {
        frequencyDirtyX = x;
        frequencyDirtyY = y;
        frequencyDirtyW = w;
        frequencyDirtyH = h;
        frequencyDirtyValid = true;
    } else {
        const int16_t x1 = min(frequencyDirtyX, x);
        const int16_t y1 = min(frequencyDirtyY, y);
        const int16_t x2 = max(static_cast<int16_t>(frequencyDirtyX + frequencyDirtyW), static_cast<int16_t>(x + w));
        const int16_t y2 = max(static_cast<int16_t>(frequencyDirtyY + frequencyDirtyH), static_cast<int16_t>(y + h));
        frequencyDirtyX = x1;
        frequencyDirtyY = y1;
        frequencyDirtyW = x2 - x1;
        frequencyDirtyH = y2 - y1;
    }

    frequencyRenderDirty = true;
}

void V1Display::showDisconnected() {
    drawBaseFrame();
    drawStatusText("Disconnected", 0xF800);  // Red
    drawWiFiIndicator();
    drawBatteryIndicator();
}

void V1Display::showResting(bool forceRedraw) {
    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    multiAlertMode = false;

    // Save the last known bogey counter before potentially resetting
    // This preserves the mode indicator (A/L/c) when V1 is connected
    char savedBogeyChar = lastState.bogeyCounterChar;
    bool savedBogeyDot = lastState.bogeyCounterDot;
    
    // Avoid redundant full-screen clears/flushes when already resting and nothing changed
    bool paletteChanged = (lastRestingPaletteRevision != paletteRevision);
    bool screenChanged = (currentScreen != ScreenMode::Resting);
    int profileSlot = currentProfileSlot;
    bool profileChanged = (profileSlot != lastRestingProfileSlot);
    
    if (forceRedraw || screenChanged || paletteChanged) {
        // Full redraw when forced, coming from another screen, or after theme change
        TFT_CALL(fillScreen)(PALETTE_BG);
        drawBaseFrame();
        
        // Draw idle state: if V1 is connected, show last known mode; otherwise show "0"
        char topChar = '0';
        bool topDot = true;
        if (bleClient.isConnected() && savedBogeyChar != 0) {
            topChar = savedBogeyChar;
            topDot = savedBogeyDot;
        }
        drawTopCounter(topChar, false, topDot);
        // Volume indicator not shown in resting state (no DisplayState available)
        
        // Band indicators all dimmed (no active bands)
        drawBandIndicators(0, false);
        
        // Signal bars all empty
        drawVerticalSignalBars(0, 0, BAND_KA, false);
        
        // Direction arrows all dimmed
        drawDirectionArrow(DIR_NONE, false);
        
        // Frequency display
        drawFrequency(0, BAND_NONE);
        
        // Mute indicator off
        drawMuteIcon(false);
        drawLockoutIndicator();
        drawGpsIndicator();
        drawObdIndicator();
        
        // Profile indicator
        drawProfileIndicator(profileSlot);
        
        // Reset secondary alert card state, then draw resting telemetry cards.
        AlertData emptyPriority;
        drawSecondaryAlertCards(nullptr, 0, emptyPriority, false);
        drawRestTelemetryCards(true);

        lastRestingPaletteRevision = paletteRevision;
        lastRestingProfileSlot = profileSlot;
        
        // Log screen mode transition for debugging display refresh issues
        if (currentScreen != ScreenMode::Resting) {
            DISPLAY_LOG("[DISP] Screen mode: %d -> Resting (showResting)\n", (int)currentScreen);
            perfRecordDisplayScreenTransition(
                static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
                PerfDisplayScreen::Resting,
                millis());
        }
        currentScreen = ScreenMode::Resting;

#if defined(DISPLAY_USE_ARDUINO_GFX)
    DISPLAY_FLUSH();
#endif
    } else if (profileChanged) {
        // Only the profile changed while already resting; redraw just the indicator
        drawProfileIndicator(profileSlot);
        lastRestingProfileSlot = profileSlot;
#if defined(DISPLAY_USE_ARDUINO_GFX)
        // Push only the regions touched by profile/WiFi/BLE/battery indicators
        const int profileFlushY = 8;
        const int profileFlushH = 36;
        flushRegion(100, profileFlushY, SCREEN_WIDTH - 160, profileFlushH);

        const int leftColWidth = 64;
        const int leftColHeight = 96;
        flushRegion(0, SCREEN_HEIGHT - leftColHeight, leftColWidth, leftColHeight);
    #else
        flush();
#endif
    }

    // Reset lastState so next update() detects changes from this "resting" state
    lastState = DisplayState();  // All defaults: bands=0, arrows=0, bars=0, hasMode=false, modeChar=0
}

void V1Display::forceNextRedraw() {
    // Reset lastState to force next update() to detect all changes and redraw
    lastState = DisplayState();
    // Set screen mode to Unknown so any next update/showResting detects a screen change
    currentScreen = ScreenMode::Unknown;
    // Reset all static change tracking variables (volume, mode, arrows, etc.)
    // This ensures the next update() does a full redraw with fresh data
    resetChangeTracking();
}

void V1Display::showScanning() {
    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    
    // Get settings for display style
    const V1Settings& s = settingsManager.get();

    // Clear and draw the base frame
    TFT_CALL(fillScreen)(PALETTE_BG);
    drawBaseFrame();
    
    // Draw idle state elements
    drawTopCounter('0', false, true);
    // Volume indicator not shown in scanning state (no DisplayState available)
    drawBandIndicators(0, false);
    drawVerticalSignalBars(0, 0, BAND_KA, false);
    drawDirectionArrow(DIR_NONE, false);
    drawMuteIcon(false);
    drawLockoutIndicator();
    drawGpsIndicator();
    drawObdIndicator();
    drawProfileIndicator(currentProfileSlot);
    
    // Draw "SCAN" in frequency area - match display style
    if (s.displayStyle == DISPLAY_STYLE_SERPENTINE) {
        fontMgr.ensureSerpentineLoaded(tft);
    }
    if (s.displayStyle == DISPLAY_STYLE_SERPENTINE && fontMgr.serpentineReady) {
        // Serpentine style: JB's favorite font
        const int fontSize = 65;
        fontMgr.serpentine.setFontColor(s.colorBandKa, PALETTE_BG);
        fontMgr.serpentine.setFontSize(fontSize);
        
        const char* text = "SCAN";
        FT_BBox bbox = fontMgr.serpentine.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, text);
        int textWidth = bbox.xMax - bbox.xMin;
        int textHeight = bbox.yMax - bbox.yMin;
        
        const int leftMargin = 120;
        const int rightMargin = 200;
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - textWidth) / 2;
        int y = getEffectiveScreenHeight() - 72;
        
        FILL_RECT(x - 4, y - textHeight - 4, textWidth + 8, textHeight + 12, PALETTE_BG);
        fontMgr.serpentine.setCursor(x, y);
        fontMgr.serpentine.printf("%s", text);
    } else if (fontMgr.segment7Ready) {
        // Classic style: use Segment7 TTF font (JBV1 style)
        const int fontSize = 65;
        const int leftMargin = 135;  // Match frequency positioning
        const int rightMargin = 200;
        const int muteIconBottom = 33;
        int effectiveHeight = getEffectiveScreenHeight();
        int y = muteIconBottom + (effectiveHeight - muteIconBottom - fontSize) / 2 + 8;
        
        const char* text = "SCAN";
        int approxWidth = 4 * 32;  // 4 chars ~32px each
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - approxWidth) / 2;
        
        FILL_RECT(x - 5, y - 5, approxWidth + 10, fontSize + 10, PALETTE_BG);
        
        // Convert color for OpenFontRender
        uint8_t bgR = (PALETTE_BG >> 11) << 3;
        uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
        uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
        fontMgr.segment7.setBackgroundColor(bgR, bgG, bgB);
        fontMgr.segment7.setFontSize(fontSize);
        fontMgr.segment7.setFontColor((s.colorBandKa >> 11) << 3, ((s.colorBandKa >> 5) & 0x3F) << 2, (s.colorBandKa & 0x1F) << 3);
        fontMgr.segment7.setCursor(x, y);
        fontMgr.segment7.printf("%s", text);
    } else {
        // Fallback: software 14-segment display
#if defined(DISPLAY_WAVESHARE_349)
        const float scale = 2.3f;  // Match frequency scale
#else
        const float scale = 1.7f;
#endif
        SegMetrics m = segMetrics(scale);
        
        // Position to match frequency display (centered between mute area and bottom)
        const int muteIconBottom = 33;
        int effectiveHeight = getEffectiveScreenHeight();
        int y = muteIconBottom + (effectiveHeight - muteIconBottom - m.digitH) / 2 + 5;
        
        const char* text = "SCAN";
        int width = measureSevenSegmentText(text, scale);  // Same measurement for 14-seg
        
        // Center between band indicators and signal bars
        const int leftMargin = 120;   // After band indicators
        const int rightMargin = 200;  // Before signal bars
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - width) / 2;
        if (x < leftMargin) x = leftMargin;
        
        FILL_RECT(x - 4, y - 4, width + 8, m.digitH + 8, PALETTE_BG);
        draw14SegmentText(text, x, y, scale, s.colorBandKa, PALETTE_BG);
    }
    
    // Reset lastState
    lastState = DisplayState();
    
#if defined(DISPLAY_USE_ARDUINO_GFX)
    DISPLAY_FLUSH();
#endif

    if (currentScreen != ScreenMode::Scanning) {
        perfRecordDisplayScreenTransition(
            static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
            PerfDisplayScreen::Scanning,
            millis());
    }
    currentScreen = ScreenMode::Scanning;
    lastRestingProfileSlot = -1;
}

// RSSI periodic update timer (shared between resting and alert modes)
static unsigned long s_lastRssiUpdateMs = 0;
static constexpr unsigned long RSSI_UPDATE_INTERVAL_MS = 2000;  // Update RSSI every 2 seconds

void V1Display::resetChangeTracking() {
    dirty.resetTracking = true;
}

void V1Display::showDemo() {
    TFT_CALL(fillScreen)(PALETTE_BG); // Clear screen to prevent artifacts
    clear();

    // Show a MUTED K-band alert to demonstrate the muted color
    AlertData demoAlert;
    demoAlert.band = BAND_K;
    demoAlert.direction = DIR_FRONT;
    demoAlert.frontStrength = 4;
    demoAlert.rearStrength = 0;
    demoAlert.frequency = 24150;  // MHz (24.150 GHz)
    demoAlert.isValid = true;

    // Create a demo display state
    DisplayState demoState;
    demoState.activeBands = BAND_K;
    demoState.arrows = DIR_FRONT;
    demoState.signalBars = 4;
    demoState.muted = true;

    // Draw the alert in MUTED state using multi-alert display
    update(demoAlert, &demoAlert, 1, demoState);
    lastState.signalBars = 1;
    
    // Also draw profile indicator and WiFi icon during demo so user can see hide toggle effect
    drawProfileIndicator(0);  // Show slot 0 profile indicator (unless hidden)
    drawWiFiIndicator();      // Show WiFi icon (unless hidden)
    
    // Flush to display
    flush();
}

void V1Display::showBootSplash() {
    const unsigned long splashStartMs = millis();
    TFT_CALL(fillScreen)(PALETTE_BG); // Clear screen to prevent artifacts
    drawBaseFrame();

    // Draw the V1 Simple logo at 1:1 (image is pre-sized to 640x172)
    // Use row-level bulk blit on Arduino_GFX to reduce draw call overhead.
    const unsigned long logoStartMs = millis();
#if defined(DISPLAY_USE_ARDUINO_GFX)
    uint16_t rowBuffer[V1SIMPLE_LOGO_WIDTH];
    for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
        const int rowOffset = sy * V1SIMPLE_LOGO_WIDTH;
        for (int sx = 0; sx < V1SIMPLE_LOGO_WIDTH; sx++) {
            rowBuffer[sx] = pgm_read_word(&v1simple_logo_rgb565[rowOffset + sx]);
        }
        TFT_CALL(draw16bitRGBBitmap)(0, sy, rowBuffer, V1SIMPLE_LOGO_WIDTH, 1);
    }
#else
    for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
        for (int sx = 0; sx < V1SIMPLE_LOGO_WIDTH; sx++) {
            uint16_t pixel = pgm_read_word(&v1simple_logo_rgb565[sy * V1SIMPLE_LOGO_WIDTH + sx]);
            TFT_CALL(drawPixel)(sx, sy, pixel);
        }
    }
#endif
    const unsigned long logoMs = millis() - logoStartMs;
    
    // Draw version number in bottom-right corner
    GFX_setTextDatum(BR_DATUM);  // Bottom-right alignment
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(0x7BEF, PALETTE_BG);  // Gray text (mid-gray RGB565)
    GFX_drawString(tft, "v" FIRMWARE_VERSION, SCREEN_WIDTH - 8, SCREEN_HEIGHT - 6);

#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Flush canvas to display before enabling backlight
    const unsigned long flushStartMs = millis();
    DISPLAY_FLUSH();
    const unsigned long flushMs = millis() - flushStartMs;
#else
    const unsigned long flushMs = 0;
#endif

    // Turn on backlight now that splash is drawn
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Waveshare 3.49" has INVERTED backlight: 0=full on, 255=off
    analogWrite(LCD_BL, 0);  // Full brightness (inverted)
#else
    digitalWrite(TFT_BL, HIGH);
#endif
    Serial.println("Backlight ON (post-splash, inverted)");
    Serial.printf("[BootTiming] splash total=%lu logo=%lu flush=%lu\n",
                  millis() - splashStartMs,
                  logoMs,
                  flushMs);
}

void V1Display::showShutdown() {
    // Clear screen
    TFT_CALL(fillScreen)(PALETTE_BG);
    
    // Draw "GOODBYE" message centered
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(3);
    TFT_CALL(setTextColor)(PALETTE_TEXT, PALETTE_BG);
    GFX_drawString(tft, "GOODBYE", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 20);
    
    // Draw smaller "Powering off..." below
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(PALETTE_GRAY, PALETTE_BG);
    GFX_drawString(tft, "Powering off...", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 20);
    
    // Flush to display
#if defined(DISPLAY_USE_ARDUINO_GFX)
    DISPLAY_FLUSH();
#endif
}

void V1Display::showLowBattery() {
    // Clear screen
    TFT_CALL(fillScreen)(PALETTE_BG);
    
    // Draw large battery outline in center
    const int battW = 120;
    const int battH = 60;
    const int battX = (SCREEN_WIDTH - battW) / 2;
    const int battY = (SCREEN_HEIGHT - battH) / 2 - 20;
    const int capW = 12;
    const int capH = 24;
    
    // Draw battery outline in red
    uint16_t redColor = 0xF800;
    DRAW_RECT(battX, battY, battW, battH, redColor);
    FILL_RECT(battX + battW, battY + (battH - capH) / 2, capW, capH, redColor);
    
    // Draw single bar (low)
    const int padding = 8;
    FILL_RECT(battX + padding, battY + padding, 20, battH - 2 * padding, redColor);
    
    // Draw "LOW BATTERY" text below
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(redColor, PALETTE_BG);
    GFX_drawString(tft, "LOW BATTERY", SCREEN_WIDTH / 2, battY + battH + 30);
    
    // Flush to display
#if defined(DISPLAY_USE_ARDUINO_GFX)
    DISPLAY_FLUSH();
#endif
}

void V1Display::drawStatusText(const char* text, uint16_t color) {
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    GFX_drawString(tft, text, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
}

void V1Display::update(const DisplayState& state) {
    // Track if we're transitioning FROM persisted mode (need full redraw)
    bool wasPersistedMode = persistedMode;
    persistedMode = false;  // Not in persisted mode
    
    // Don't process resting update if we're in Scanning mode - wait for showResting() to be called
    if (currentScreen == ScreenMode::Scanning) {
        return;
    }
    
    static bool firstUpdate = true;
    static bool wasInFlashPeriod = false;
    
    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    multiAlertMode = false;  // No cards to draw in resting state
    
    // Check if profile flash period just expired (needs redraw to clear)
    bool inFlashPeriod = (millis() - profileChangedTime) < HIDE_TIMEOUT_MS;
    bool flashJustExpired = wasInFlashPeriod && !inFlashPeriod;
    wasInFlashPeriod = inFlashPeriod;

    // Band debouncing: keep bands visible for a short grace period to prevent flicker
    static unsigned long restingBandLastSeen[4] = {0, 0, 0, 0};  // L, Ka, K, X
    static uint8_t restingDebouncedBands = 0;
    const unsigned long BAND_GRACE_MS = 100;  // Reduced from 200ms for snappier response
    unsigned long now = millis();
    
    if (state.activeBands & BAND_LASER) restingBandLastSeen[0] = now;
    if (state.activeBands & BAND_KA)    restingBandLastSeen[1] = now;
    if (state.activeBands & BAND_K)     restingBandLastSeen[2] = now;
    if (state.activeBands & BAND_X)     restingBandLastSeen[3] = now;
    
    restingDebouncedBands = state.activeBands;
    if ((now - restingBandLastSeen[0]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_LASER;
    if ((now - restingBandLastSeen[1]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_KA;
    if ((now - restingBandLastSeen[2]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_K;
    if ((now - restingBandLastSeen[3]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_X;

    // In resting mode (no alerts), never show muted visual - just normal display
    // Apps commonly set main volume to 0 when idle, adjusting on new alerts
    // The muted state should only affect active alert display, not resting
    bool effectiveMuted = false;

    // Track last debounced bands for change detection
    static uint8_t lastRestingDebouncedBands = 0;
    static uint8_t lastRestingSignalBars = 0;
    static uint8_t lastRestingArrows = 0;
    static uint8_t lastRestingMainVol = 255;
    static uint8_t lastRestingMuteVol = 255;
    static uint8_t lastRestingBogeyByte = 0;  // Track V1's bogey counter for change detection
    
    // Reset resting statics when change tracking reset is requested (on V1 disconnect)
    if (dirty.resetTracking) {
        firstUpdate = true;
        lastRestingDebouncedBands = 0;
        lastRestingSignalBars = 0;
        lastRestingArrows = 0;
        lastRestingMainVol = 255;
        lastRestingMuteVol = 255;
        lastRestingBogeyByte = 0;
        s_lastRssiUpdateMs = 0;
        // Don't clear the flag here - let the alert update() clear it
    }
    
    // Check if RSSI needs periodic refresh (every 2 seconds)
    bool rssiNeedsUpdate = (now - s_lastRssiUpdateMs) >= RSSI_UPDATE_INTERVAL_MS;
    
    // Check if transitioning from a non-resting visual mode.
    bool leavingLiveMode = (currentScreen == ScreenMode::Live);
    bool leavingCameraMode = (currentScreen == ScreenMode::Camera);
    
    // Separate full redraw triggers from incremental updates
    bool needsFullRedraw =
        firstUpdate ||
        flashJustExpired ||
        wasPersistedMode ||  // Force full redraw when leaving persisted mode
        leavingLiveMode ||   // Force full redraw when alerts end (clear cards/frequency)
        leavingCameraMode || // Force full redraw when camera banner clears
        restingDebouncedBands != lastRestingDebouncedBands ||
        effectiveMuted != lastState.muted;
    
    bool arrowsChanged = (state.arrows != lastRestingArrows);
    bool signalBarsChanged = (state.signalBars != lastRestingSignalBars);
    bool volumeChanged = (state.mainVolume != lastRestingMainVol || state.muteVolume != lastRestingMuteVol);
    bool bogeyCounterChanged = (state.bogeyCounterByte != lastRestingBogeyByte);
    
    // Check if volume zero warning needs a flashing redraw
    bool currentProxyConnected = bleClient.isProxyClientConnected();
    bool volZero = (state.mainVolume == 0 && state.hasVolumeData);
    if (volZeroWarn.needsFlashRedraw(volZero, currentProxyConnected, preQuietActive_)) {
        needsFullRedraw = true;
    }
    
    if (!needsFullRedraw && !arrowsChanged && !signalBarsChanged && !volumeChanged && !bogeyCounterChanged && !rssiNeedsUpdate) {
        if (drawRestTelemetryCards(false)) {
            flushRegion(DisplayLayout::CONTENT_LEFT_MARGIN,
                        SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT,
                        DisplayLayout::CONTENT_AVAILABLE_WIDTH,
                        SECONDARY_ROW_HEIGHT);
        }
        return;
    }
    
    if (!needsFullRedraw && (arrowsChanged || signalBarsChanged || volumeChanged || bogeyCounterChanged || rssiNeedsUpdate)) {
        // Incremental update - only redraw what changed
        if (arrowsChanged) {
            lastRestingArrows = state.arrows;
            drawDirectionArrow(state.arrows, effectiveMuted, state.flashBits);
        }
        if (signalBarsChanged) {
            lastRestingSignalBars = state.signalBars;
            Band primaryBand = BAND_KA;
            if (restingDebouncedBands & BAND_LASER) primaryBand = BAND_LASER;
            else if (restingDebouncedBands & BAND_KA) primaryBand = BAND_KA;
            else if (restingDebouncedBands & BAND_K) primaryBand = BAND_K;
            else if (restingDebouncedBands & BAND_X) primaryBand = BAND_X;
            drawVerticalSignalBars(state.signalBars, state.signalBars, primaryBand, effectiveMuted);
        }
        const V1Settings& s = settingsManager.get();
        if (volumeChanged && state.supportsVolume() && !s.hideVolumeIndicator) {
            lastRestingMainVol = state.mainVolume;
            lastRestingMuteVol = state.muteVolume;
            drawVolumeIndicator(state.mainVolume, state.muteVolume);
            drawRssiIndicator(bleClient.getConnectionRssi());
            s_lastRssiUpdateMs = now;  // Reset RSSI timer when we update with volume
        }
        if (rssiNeedsUpdate && !volumeChanged) {
            // Periodic RSSI-only update
            drawRssiIndicator(bleClient.getConnectionRssi());
            s_lastRssiUpdateMs = now;
        }
        if (bogeyCounterChanged) {
            lastRestingBogeyByte = state.bogeyCounterByte;
            drawTopCounter(state.bogeyCounterChar, effectiveMuted, state.bogeyCounterDot);
        }
        drawRestTelemetryCards(false);
#if defined(DISPLAY_WAVESHARE_349)
    DISPLAY_FLUSH();
#endif
        lastState = state;
        return;
    }

    // Full redraw needed
    firstUpdate = false;
    lastRestingDebouncedBands = restingDebouncedBands;
    lastRestingArrows = state.arrows;
    lastRestingSignalBars = state.signalBars;
    lastRestingMainVol = state.mainVolume;
    lastRestingMuteVol = state.muteVolume;
    lastRestingBogeyByte = state.bogeyCounterByte;
    s_lastRssiUpdateMs = now;  // Reset RSSI timer on full redraw
    
    drawBaseFrame();
    // Use V1's decoded bogey counter byte - shows mode, volume, etc.
    char topChar = state.bogeyCounterChar;
    drawTopCounter(topChar, effectiveMuted, state.bogeyCounterDot);
    const V1Settings& s = settingsManager.get();
    if (state.supportsVolume() && !s.hideVolumeIndicator) {
        drawVolumeIndicator(state.mainVolume, state.muteVolume);  // Show volume below bogey counter (V1 4.1028+)
        drawRssiIndicator(bleClient.getConnectionRssi());
    }
    drawBandIndicators(restingDebouncedBands, effectiveMuted);
    // BLE proxy status indicator
    
    // Determine primary band for frequency and signal bar coloring
    Band primaryBand = BAND_NONE;
    if (restingDebouncedBands & BAND_LASER) primaryBand = BAND_LASER;
    else if (restingDebouncedBands & BAND_KA) primaryBand = BAND_KA;
    else if (restingDebouncedBands & BAND_K) primaryBand = BAND_K;
    else if (restingDebouncedBands & BAND_X) primaryBand = BAND_X;
    
    // Volume-zero warning: 15s delay → 10s flashing "VOL 0" → acknowledge
    bool proxyConnected = bleClient.isProxyClientConnected();
    bool showVolumeWarning = volZeroWarn.evaluate(
        volZero, proxyConnected, preQuietActive_, play_vol0_beep);
    
    if (showVolumeWarning) {
        drawVolumeZeroWarning();
    } else {
        drawFrequency(0, primaryBand, effectiveMuted);
    }
    
    drawVerticalSignalBars(state.signalBars, state.signalBars, primaryBand, effectiveMuted);
    // Never draw arrows in resting display - arrows should only appear in live mode
    // when we have actual alert data with frequency. If display packet has arrows but
    // no alert packet arrived, we shouldn't show arrows without frequency.
    drawDirectionArrow(DIR_NONE, effectiveMuted, 0);
    drawMuteIcon(effectiveMuted);
    drawLockoutIndicator();
    drawGpsIndicator();
    drawObdIndicator();
    drawProfileIndicator(currentProfileSlot);
    
    // Clear any persisted card slots when entering resting state
    AlertData emptyPriority;
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, effectiveMuted);
    drawRestTelemetryCards(true);

#if defined(DISPLAY_WAVESHARE_349)
    DISPLAY_FLUSH();  // Push canvas to display
#endif

    if (currentScreen != ScreenMode::Resting) {
        perfRecordDisplayScreenTransition(
            static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
            PerfDisplayScreen::Resting,
            millis());
    }
    currentScreen = ScreenMode::Resting;  // Set screen mode after redraw complete
    lastState = state;
}

void V1Display::refreshFrequencyOnly(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    drawFrequency(freqMHz, band, muted, isPhotoRadar);
    if (frequencyRenderDirty) {
        if (frequencyDirtyValid) {
            flushRegion(frequencyDirtyX, frequencyDirtyY, frequencyDirtyW, frequencyDirtyH);
        } else {
            // Conservative fallback when no precise dirty region was captured.
            flushRegion(DisplayLayout::CONTENT_LEFT_MARGIN,
                        DisplayLayout::PRIMARY_ZONE_Y,
                        DisplayLayout::CONTENT_AVAILABLE_WIDTH,
                        DisplayLayout::PRIMARY_ZONE_HEIGHT);
        }
    }
}

void V1Display::refreshSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted) {
    drawSecondaryAlertCards(alerts, alertCount, priority, muted);
    flushRegion(0,
                SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT,
                SCREEN_WIDTH,
                SECONDARY_ROW_HEIGHT);
}

// Persisted alert display - shows last alert in dark grey after V1 clears it
// Only draws frequency, band, and arrows - no signal bars, no mute badge
// Bogey counter shows V1 mode (from state), not "1"
void V1Display::updatePersisted(const AlertData& alert, const DisplayState& state) {
    if (!alert.isValid) {
        persistedMode = false;
        update(state);  // Fall back to normal resting display
        return;
    }
    
    // Enable persisted mode so draw functions use PALETTE_PERSISTED instead of PALETTE_MUTED
    persistedMode = true;
    
    // Track screen mode - persisted is NOT Live, so transition to Live will trigger full redraw
    if (currentScreen != ScreenMode::Resting) {
        perfRecordDisplayScreenTransition(
            static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
            PerfDisplayScreen::Resting,
            millis());
    }
    currentScreen = ScreenMode::Resting;
    
    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    multiAlertMode = false;  // No cards to draw
    wasInMultiAlertMode = false;
    
    drawBaseFrame();
    
    // Bogey counter shows V1's decoded display - NOT greyed, always visible
    char topChar = state.bogeyCounterChar;
    drawTopCounter(topChar, false, state.bogeyCounterDot);  // muted=false to keep it visible
    const V1Settings& s = settingsManager.get();
    if (state.supportsVolume() && !s.hideVolumeIndicator) {
        drawVolumeIndicator(state.mainVolume, state.muteVolume);  // Show current volume (V1 4.1028+)
        drawRssiIndicator(bleClient.getConnectionRssi());
    }
    
    // Band indicator in persisted color
    uint8_t bandMask = alert.band;
    drawBandIndicators(bandMask, true);  // muted=true triggers PALETTE_MUTED_OR_PERSISTED
    
    // Frequency in persisted color (pass muted=true)
    // Note: Photo radar check uses state.bogeyCounterChar even for persisted alerts
    bool isPhotoRadar = (state.bogeyCounterChar == 'P');
    drawFrequency(alert.frequency, alert.band, true, isPhotoRadar);
    
    // No signal bars - just draw empty
    drawVerticalSignalBars(0, 0, alert.band, true);
    
    // Arrows in dark grey
    drawDirectionArrow(alert.direction, true);  // muted=true for grey
    
    // No mute badge
    // drawMuteIcon intentionally skipped
    
    // Profile indicator still shown
    drawProfileIndicator(currentProfileSlot);
    
    // Clear card area AND expire all tracked card slots (no cards during persisted state)
    // This prevents stale cards from reappearing when returning to live alerts
    AlertData emptyPriority;
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, true);

#if defined(DISPLAY_WAVESHARE_349)
    DISPLAY_FLUSH();
#endif
}

void V1Display::updateCameraAlert(uint8_t cameraType, bool muted) {
    persistedMode = false;

    // Camera banner occupies the same primary zone as resting/live content.
    dirty.multiAlert = true;
    multiAlertMode = false;
    wasInMultiAlertMode = false;

    if (currentScreen != ScreenMode::Camera) {
        perfRecordDisplayScreenTransition(
            static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
            PerfDisplayScreen::Resting,
            millis());
    }
    currentScreen = ScreenMode::Camera;

    drawBaseFrame();
    drawTopCounter('~', muted, false);
    drawBandIndicators(0, muted);
    drawVerticalSignalBars(0, 0, BAND_KA, muted);
    drawCameraToken(cameraTokenForType(cameraType), muted);
    const V1Settings& s = settingsManager.get();
    drawDirectionArrow(DIR_FRONT, muted, 0, s.colorCameraArrow);
    drawMuteIcon(false);
    drawLockoutIndicator();
    drawGpsIndicator();
    drawObdIndicator();
    drawProfileIndicator(currentProfileSlot);

    AlertData emptyPriority;
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, muted);

#if defined(DISPLAY_WAVESHARE_349)
    DISPLAY_FLUSH();
#endif

    lastState = DisplayState();
}

// Multi-alert update: draws priority alert with secondary alert cards below
void V1Display::update(const AlertData& priority, const AlertData* allAlerts, int alertCount, const DisplayState& state) {
    // Check if we're transitioning FROM persisted mode (need full redraw to restore colors)
    bool wasPersistedMode = persistedMode;
    persistedMode = false;  // Not in persisted mode

    // Get settings reference for priorityArrowOnly
    const V1Settings& s = settingsManager.get();

    // If no valid priority alert, return (caller should use updatePersisted or update(state) instead)
    if (!priority.isValid || priority.band == BAND_NONE) {
        return;
    }

    // Track screen mode transitions - force redraw when entering live mode from resting/scanning
    bool enteringLiveMode = (currentScreen != ScreenMode::Live);
    if (enteringLiveMode) {
        DISPLAY_LOG("[DISP] Entering Live mode (was %d), alertCount=%d\n", 
                    (int)currentScreen, alertCount);
        perfRecordDisplayScreenTransition(
            static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
            PerfDisplayScreen::Live,
            millis());
    }
    currentScreen = ScreenMode::Live;

    // Always use multi-alert mode (raised layout for cards)
    dirty.multiAlert = true;
    multiAlertMode = true;

    // V1 is source of truth - use activeBands directly, no debouncing
    // This allows V1's native blinking to come through

    // Alert packets can arrive before the matching display packet, which leaves
    // bogeyCounterChar briefly stale (often mode letters like 'A') on the first frame.
    // Normalize to alert-count digits for live alerts unless the symbol is a known
    // special alert marker that should be preserved.
    char liveTopCounterChar = state.bogeyCounterChar;
    bool liveTopCounterDot = state.bogeyCounterDot;
    if (alertCount > 0 && alertCount <= 9) {
        const bool rawIsDigit = (liveTopCounterChar >= '0' && liveTopCounterChar <= '9');
        const bool preserveSpecialSymbol =
            (liveTopCounterChar == '#') || (liveTopCounterChar == 'P') || (liveTopCounterChar == 'J');
        if (!rawIsDigit && !preserveSpecialSymbol) {
            const char normalized = static_cast<char>('0' + alertCount);
            DISPLAY_LOG("[DISP] Normalize top counter '%c' -> '%c' (alerts=%d)\n",
                        liveTopCounterChar, normalized, alertCount);
            liveTopCounterChar = normalized;
            liveTopCounterDot = false;
        }
    }

    // Change detection: check if we need to redraw
    static AlertData lastPriority;
    static uint8_t lastBogeyByte = 0;  // Track V1's bogey counter byte for change detection
    static DisplayState lastMultiState;
    static bool firstRun = true;
    static AlertData lastSecondary[PacketParser::MAX_ALERTS];  // Track all 15 possible V1 alerts for change detection
    static uint8_t lastArrows = 0;
    static uint8_t lastSignalBars = 0;
    static uint8_t lastActiveBands = 0;
    
    // Check if reset was requested (e.g., on V1 disconnect)
    if (dirty.resetTracking) {
        lastPriority = AlertData();
        lastBogeyByte = 0;
        lastMultiState = DisplayState();
        firstRun = true;
        for (int i = 0; i < PacketParser::MAX_ALERTS; i++) lastSecondary[i] = AlertData();
        lastArrows = 0;
        lastSignalBars = 0;
        lastActiveBands = 0;
        dirty.resetTracking = false;
    }
    
    bool needsRedraw = false;
    
    // Frequency tolerance for V1 jitter (V1 can report ±1-3 MHz variation between packets)
    const uint32_t FREQ_TOLERANCE_MHZ = 5;
    auto freqDifferent = [FREQ_TOLERANCE_MHZ](uint32_t a, uint32_t b) -> bool {
        uint32_t diff = (a > b) ? (a - b) : (b - a);
        return diff > FREQ_TOLERANCE_MHZ;
    };
    
    // Always redraw on first run, entering live mode, or when transitioning from persisted mode
    if (firstRun) { needsRedraw = true; firstRun = false; }
    else if (enteringLiveMode) { needsRedraw = true; }
    else if (wasPersistedMode) { needsRedraw = true; }
    // V1 is source of truth - always redraw when priority alert changes
    // Use frequency tolerance to avoid full redraws from V1 jitter
    else if (freqDifferent(priority.frequency, lastPriority.frequency)) { needsRedraw = true; }
    else if (priority.band != lastPriority.band) { needsRedraw = true; }
    else if (state.muted != lastMultiState.muted) { needsRedraw = true; }
    // Note: bogey counter changes are handled via incremental update (bogeyCounterChanged) for rapid response
    
    // Also check if any secondary alert changed (set-based, not order-based)
    // V1 may reorder alerts by signal strength - we only care if the SET of alerts changed
    if (!needsRedraw) {
        // Compare counts first
        int lastAlertCount = 0;
        for (int i = 0; i < PacketParser::MAX_ALERTS; i++) {
            if (lastSecondary[i].band != BAND_NONE) lastAlertCount++;
        }
        if (alertCount != lastAlertCount) {
            needsRedraw = true;
        } else {
            // Check if any current alert is NOT in last set (set membership test)
            // Use frequency tolerance (±5 MHz) to handle V1 jitter
            const uint32_t FREQ_TOLERANCE_MHZ = 5;
            for (int i = 0; i < alertCount && i < PacketParser::MAX_ALERTS && !needsRedraw; i++) {
                bool foundInLast = false;
                for (int j = 0; j < PacketParser::MAX_ALERTS; j++) {
                    if (allAlerts[i].band == lastSecondary[j].band) {
                        if (allAlerts[i].band == BAND_LASER) {
                            foundInLast = true;
                        } else {
                            uint32_t diff = (allAlerts[i].frequency > lastSecondary[j].frequency) 
                                ? (allAlerts[i].frequency - lastSecondary[j].frequency) 
                                : (lastSecondary[j].frequency - allAlerts[i].frequency);
                            if (diff <= FREQ_TOLERANCE_MHZ) foundInLast = true;
                        }
                        if (foundInLast) break;
                    }
                }
                if (!foundInLast) {
                    needsRedraw = true;
                }
            }
        }
    }
    
    // Track arrow, signal bar, and band changes separately for incremental update
    // Arrow display depends on per-profile priorityArrowOnly setting
    // When priorityArrowOnly is enabled, still respect V1's arrow blinking by masking with state.arrows
    // V1 handles blinking by toggling image1 arrow bits - we follow that
    Direction arrowsToShow;
    if (settingsManager.getSlotPriorityArrowOnly(s.activeSlot)) {
        // Show priority arrow only when V1 is also showing that direction
        arrowsToShow = static_cast<Direction>(state.priorityArrow & state.arrows);
    } else {
        arrowsToShow = state.arrows;
    }
    bool arrowsChanged = (arrowsToShow != lastArrows);
    bool signalBarsChanged = (state.signalBars != lastSignalBars);
    bool bandsChanged = (state.activeBands != lastActiveBands);
    bool bogeyCounterChanged = (state.bogeyCounterByte != lastBogeyByte);

    // Volume tracking
    static uint8_t lastMainVol = 255;
    static uint8_t lastMuteVol = 255;
    bool volumeChanged = (state.mainVolume != lastMainVol || state.muteVolume != lastMuteVol);
    
    // Check if RSSI needs periodic refresh (every 2 seconds)
    unsigned long now = millis();
    bool rssiNeedsUpdate = (now - s_lastRssiUpdateMs) >= RSSI_UPDATE_INTERVAL_MS;
    
    // Force periodic redraw when something is flashing (for blink animation)
    // Check if any arrows or bands are marked as flashing
    bool hasFlashing = (state.flashBits != 0) || (state.bandFlashBits != 0);
    static unsigned long lastFlashRedraw = 0;
    bool needsFlashUpdate = false;
    if (hasFlashing) {
        if (now - lastFlashRedraw >= 75) {  // Redraw at ~13Hz for smoother blink
            needsFlashUpdate = true;
            lastFlashRedraw = now;
        }
    }
    
    if (!needsRedraw && !arrowsChanged && !signalBarsChanged && !bandsChanged && !needsFlashUpdate && !volumeChanged && !bogeyCounterChanged && !rssiNeedsUpdate) {
        // Nothing changed on main display, but still process cards for expiration
        drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
#if defined(DISPLAY_WAVESHARE_349)
    DISPLAY_FLUSH();
#endif
        return;
    }
    
    if (!needsRedraw && (arrowsChanged || signalBarsChanged || bandsChanged || needsFlashUpdate || volumeChanged || bogeyCounterChanged || rssiNeedsUpdate)) {
        // Only arrows, signal bars, bands, or bogey count changed - do incremental update without full redraw
        // Also handle flash updates (periodic redraw for blink animation)
        if (arrowsChanged || (needsFlashUpdate && state.flashBits != 0)) {
            lastArrows = arrowsToShow;
            drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
        }
        if (signalBarsChanged) {
            lastSignalBars = state.signalBars;
            drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
        }
        if (bandsChanged || (needsFlashUpdate && state.bandFlashBits != 0)) {
            lastActiveBands = state.activeBands;
            drawBandIndicators(state.activeBands, state.muted, state.bandFlashBits);
        }
        if (volumeChanged && state.supportsVolume() && !s.hideVolumeIndicator) {
            lastMainVol = state.mainVolume;
            lastMuteVol = state.muteVolume;
            drawVolumeIndicator(state.mainVolume, state.muteVolume);
            drawRssiIndicator(bleClient.getConnectionRssi());
            s_lastRssiUpdateMs = now;  // Reset RSSI timer when we update with volume
        }
        if (rssiNeedsUpdate && !volumeChanged) {
            // Periodic RSSI-only update
            drawRssiIndicator(bleClient.getConnectionRssi());
            s_lastRssiUpdateMs = now;
        }
        if (bogeyCounterChanged) {
            // Bogey counter update - use V1's decoded byte (shows J, P, volume, etc.)
            lastBogeyByte = state.bogeyCounterByte;
            drawTopCounter(liveTopCounterChar, state.muted, liveTopCounterDot);
        }
        // Still process cards so they can expire and be cleared
        drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
#if defined(DISPLAY_WAVESHARE_349)
    DISPLAY_FLUSH();
#endif
        return;
    }
    
    // Full redraw needed - store current state for next comparison
    lastPriority = priority;
    lastBogeyByte = state.bogeyCounterByte;
    lastMultiState = state;
    // Use same arrowsToShow logic as computed above for change detection
    lastArrows = arrowsToShow;
    lastSignalBars = state.signalBars;
    lastActiveBands = state.activeBands;
    lastMainVol = state.mainVolume;
    lastMuteVol = state.muteVolume;
    s_lastRssiUpdateMs = now;  // Reset RSSI timer on full redraw
    // Store all alerts for change detection (V1 supports up to 15)
    // We only display primary + 2 cards, but track all for accurate change detection
    for (int i = 0; i < PacketParser::MAX_ALERTS; i++) {
        lastSecondary[i] = (i < alertCount) ? allAlerts[i] : AlertData();
    }
    
    DISP_PERF_START();
    drawBaseFrame();
    DISP_PERF_LOG("drawBaseFrame");

    // V1 is source of truth - use activeBands directly (allows blinking)
    uint8_t bandMask = state.activeBands;
    
    // Bogey counter - use V1's decoded byte (shows J=Junk, P=Photo, volume, etc.)
    drawTopCounter(liveTopCounterChar, state.muted, liveTopCounterDot);
    
    const V1Settings& settings = settingsManager.get();
    if (state.supportsVolume() && !settings.hideVolumeIndicator) {
        drawVolumeIndicator(state.mainVolume, state.muteVolume);  // Show volume below bogey counter (V1 4.1028+)
        drawRssiIndicator(bleClient.getConnectionRssi());
    }
    DISP_PERF_LOG("counters+vol");
    
    // Main alert display (frequency, bands, arrows, signal bars)
    // Use state.signalBars which is the MAX across ALL alerts (calculated in packet_parser)
    bool isPhotoRadar = (liveTopCounterChar == 'P');
    drawFrequency(priority.frequency, priority.band, state.muted, isPhotoRadar);
    DISP_PERF_LOG("drawFrequency");
    drawBandIndicators(bandMask, state.muted, state.bandFlashBits);
    drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
    DISP_PERF_LOG("bands+bars");
    
    // Arrow display: use priority arrow only if setting enabled, otherwise all V1 arrows
    // (arrowsToShow already computed above for change detection)
    drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
    drawMuteIcon(state.muted);
    drawLockoutIndicator();
    drawGpsIndicator();
    drawObdIndicator();
    drawProfileIndicator(currentProfileSlot);
    DISP_PERF_LOG("arrows+icons");
    
    // Force card redraw since drawBaseFrame cleared the screen
    dirty.cards = true;
    
    // Draw secondary alert cards at bottom
    drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
    DISP_PERF_LOG("cards");
    
    // Keep dirty.multiAlert true while in multi-alert - only reset when going to single-alert mode

#if defined(DISPLAY_WAVESHARE_349)
    DISPLAY_FLUSH();
    DISP_PERF_LOG("flush");
#endif

    lastAlert = priority;
    lastState = state;
}

bool V1Display::drawRestTelemetryCards(bool forceRedraw) {
#if defined(DISPLAY_WAVESHARE_349)
    const int cardY = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT;
    const int rowX = DisplayLayout::CONTENT_LEFT_MARGIN;
    const int rowW = DisplayLayout::CONTENT_AVAILABLE_WIDTH;
    const int cardSpacing = 10;
    const int cardW = (rowW - (cardSpacing * 2)) / 3;
    const int cardH = SECONDARY_ROW_HEIGHT;
    const V1Settings& s = settingsManager.get();

    static bool cacheValid = false;
    static bool lastAvailable[3] = {false, false, false};
    static char lastValues[3][12] = {{0}};

    if (!s.showRestTelemetryCards) {
        if (cacheValid) {
            FILL_RECT(rowX, cardY, rowW, cardH, PALETTE_BG);
            cacheValid = false;
            for (int i = 0; i < 3; i++) {
                lastAvailable[i] = false;
                lastValues[i][0] = '\0';
            }
            return true;
        }
        return false;
    }

    const OBDData obd = obdHandler.getData();
    const unsigned long nowMs = millis();
    const bool hasFreshData = obd.valid && (nowMs - obd.timestamp_ms <= 3000);

    char valueOil[12];
    char valueIat[12];
    char valueVolt[12];
    strcpy(valueOil, "---");
    strcpy(valueIat, "---");
    strcpy(valueVolt, "---");

    const bool oilAvailable = hasFreshData && (obd.oil_temp_c != INT16_MIN);
    const bool iatAvailable = hasFreshData && (obd.intake_air_temp_c != -128);
    const bool voltAvailable = hasFreshData && (obd.voltage > 0.0f);

    if (oilAvailable) {
        const int oilF = static_cast<int>(obd.oil_temp_c) * 9 / 5 + 32;
        snprintf(valueOil, sizeof(valueOil), "%dF", oilF);
    }
    if (iatAvailable) {
        const int iatF = static_cast<int>(obd.intake_air_temp_c) * 9 / 5 + 32;
        snprintf(valueIat, sizeof(valueIat), "%dF", iatF);
    }
    if (voltAvailable) {
        snprintf(valueVolt, sizeof(valueVolt), "%.1fV", obd.voltage);
    }

    bool needsRedraw = forceRedraw || !cacheValid;

    const bool availableNow[3] = {oilAvailable, iatAvailable, voltAvailable};
    const char* valuesNow[3] = {valueOil, valueIat, valueVolt};
    for (int i = 0; i < 3 && !needsRedraw; i++) {
        if (lastAvailable[i] != availableNow[i] || strcmp(lastValues[i], valuesNow[i]) != 0) {
            needsRedraw = true;
        }
    }
    if (!needsRedraw) {
        return false;
    }

    cacheValid = true;
    for (int i = 0; i < 3; i++) {
        lastAvailable[i] = availableNow[i];
        strncpy(lastValues[i], valuesNow[i], sizeof(lastValues[i]));
        lastValues[i][sizeof(lastValues[i]) - 1] = '\0';
    }

    const uint16_t accentColors[3] = {s.colorBandKa, s.colorBandK, s.colorBandX};
    const char* labels[3] = {"OIL", "IAT", "VOLT"};

    // Clear only the telemetry row in the content area before redrawing cards.
    FILL_RECT(rowX, cardY, rowW, cardH, PALETTE_BG);

    for (int i = 0; i < 3; i++) {
        const int cardX = rowX + i * (cardW + cardSpacing);
        const bool available = availableNow[i];
        const uint16_t accent = accentColors[i];

        uint16_t borderCol = PALETTE_MUTED;
        uint16_t bgCol = 0x2104;
        uint16_t labelCol = PALETTE_MUTED;
        uint16_t valueCol = PALETTE_MUTED;

        if (available) {
            uint8_t r = ((accent >> 11) & 0x1F) * 3 / 10;
            uint8_t g = ((accent >> 5) & 0x3F) * 3 / 10;
            uint8_t b = (accent & 0x1F) * 3 / 10;
            bgCol = (r << 11) | (g << 5) | b;
            borderCol = accent;
            labelCol = accent;
            valueCol = TFT_WHITE;
        }

        FILL_ROUND_RECT(cardX, cardY, cardW, cardH, 5, bgCol);
        DRAW_ROUND_RECT(cardX, cardY, cardW, cardH, 5, borderCol);

        GFX_setTextDatum(TC_DATUM);
        TFT_CALL(setTextSize)(1);
        TFT_CALL(setTextColor)(labelCol, bgCol);
        GFX_drawString(tft, labels[i], cardX + cardW / 2, cardY + 5);

        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2);
        TFT_CALL(setTextColor)(valueCol, bgCol);
        GFX_drawString(tft, valuesNow[i], cardX + cardW / 2, cardY + (cardH / 2) + 8);
    }

    return true;
#else
    (void)forceRedraw;
    return false;
#endif
}

// Draw mini alert cards for secondary (non-priority) alerts
// With persistence: cards stay visible (greyed) for a grace period after alert disappears
void V1Display::drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted) {
#if defined(DISPLAY_WAVESHARE_349)
    const int cardH = SECONDARY_ROW_HEIGHT;  // 54px (compact with uniform signal bars)
    const int cardY = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT;  // Y=118
    const int cardW = 145;     // Card width (wider to fit freq + band)
    const int cardSpacing = 10;  // Increased spacing between cards
    const int leftMargin = 120;   // After band indicators
    const int rightMargin = 200;  // Before signal bars (at X=440)
    const int availableWidth = SCREEN_WIDTH - leftMargin - rightMargin;  // 320px
    // Center two cards in available space
    const int totalCardsWidth = cardW * 2 + cardSpacing;  // 300px
    const int startX = leftMargin + (availableWidth - totalCardsWidth) / 2;  // Center offset
    
    // Get persistence time from profile settings (same as main alert persistence)
    const V1Settings& settings = settingsManager.get();
    uint8_t persistSec = settingsManager.getSlotAlertPersistSec(settings.activeSlot);
    unsigned long gracePeriodMs = persistSec * 1000UL;
    
    // If persistence is disabled (0), cards disappear immediately
    if (gracePeriodMs == 0) {
        gracePeriodMs = 1;  // Minimum 1ms so expiration logic works
    }
    
    unsigned long now = millis();
    
    // Static card slots for persistence tracking
    static struct {
        AlertData alert{};
        unsigned long lastSeen = 0;  // 0 = empty slot
    } cards[2];
    
    // Track previous priority to add as persisted card when it disappears
    static AlertData lastPriorityForCards;
    
    // Track what was drawn at each POSITION (0 or 1) for incremental updates
    static struct {
        // V1 card state
        Band band = BAND_NONE;
        uint32_t frequency = 0;
        uint8_t direction = 0;
        bool isGraced = false;
        bool wasMuted = false;
        uint8_t bars = 0;           // Signal strength bars (0-6)
    } lastDrawnPositions[2];
    [[maybe_unused]] static int lastDrawnCount = 0;
    
    // Track profile changes - clear cards when profile rotates
    static int lastCardProfileSlot = -1;
    if (settings.activeSlot != lastCardProfileSlot) {
        lastCardProfileSlot = settings.activeSlot;
        // Clear all card state on profile change
        for (int c = 0; c < 2; c++) {
            cards[c].alert = AlertData();
            cards[c].lastSeen = 0;
            lastDrawnPositions[c].band = BAND_NONE;
            lastDrawnPositions[c].frequency = 0;
            lastDrawnPositions[c].bars = 0;
        }
        lastDrawnCount = 0;
        lastPriorityForCards = AlertData();
    }
    
    // If called with nullptr alerts and count 0, clear V1 card state
    if (alerts == nullptr && alertCount == 0) {
        for (int c = 0; c < 2; c++) {
            cards[c].alert = AlertData();
            cards[c].lastSeen = 0;
        }
        lastPriorityForCards = AlertData();
        
        // Clear the card area
        [[maybe_unused]] const int signalBarsX = SCREEN_WIDTH - 200 - 2;
        const int clearWidth = signalBarsX - startX;
        if (clearWidth > 0) {
            FILL_RECT(startX, cardY, clearWidth, cardH, PALETTE_BG);
        }
        // Reset last drawn count so next time cards appear, change is detected
        lastDrawnCount = 0;
        return;
    }
    
    // Helper: check if two alerts match (same band + frequency within tolerance)
    // V1 frequency can jitter by a few MHz between frames - use ±5 MHz tolerance
    auto alertsMatch = [](const AlertData& a, const AlertData& b) -> bool {
        if (a.band != b.band) return false;
        if (a.band == BAND_LASER) return true;
        // Use a small tolerance to handle V1 jitter without merging distinct nearby bogeys
        const uint32_t FREQ_TOLERANCE_MHZ = 2;
        uint32_t diff = (a.frequency > b.frequency) ? (a.frequency - b.frequency) : (b.frequency - a.frequency);
        return diff <= FREQ_TOLERANCE_MHZ;
    };
    
    // Helper: check if alert matches priority (returns false if priority is invalid)
    auto isSameAsPriority = [&priority, &alertsMatch](const AlertData& a) -> bool {
        if (!priority.isValid || priority.band == BAND_NONE) return false;
        return alertsMatch(a, priority);
    };
    
    // Step 0: Check if priority changed - add old priority as persisted card
    // This handles the case where laser takes priority, then stops - laser should persist as card
    if (lastPriorityForCards.isValid && lastPriorityForCards.band != BAND_NONE) {
        bool priorityChanged = !alertsMatch(lastPriorityForCards, priority);
        bool oldPriorityGone = true;
        
        // Check if old priority is still in current alerts
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(lastPriorityForCards, alerts[i])) {
                    oldPriorityGone = false;
                    break;
                }
            }
        }
        
        // If old priority is gone (not just demoted), add it as persisted card
        if (priorityChanged && oldPriorityGone) {
            // Check if already tracked
            bool found = false;
            for (int c = 0; c < 2; c++) {
                if (cards[c].lastSeen > 0 && alertsMatch(cards[c].alert, lastPriorityForCards)) {
                    found = true;
                    break;
                }
            }
            
            // Add to empty slot if not already tracked
            if (!found) {
                for (int c = 0; c < 2; c++) {
                    if (cards[c].lastSeen == 0) {
                        cards[c].alert = lastPriorityForCards;
                        cards[c].lastSeen = now;
                        break;
                    }
                }
            }
        }
    }
    
    // Update last priority tracking
    lastPriorityForCards = priority;
    
    // Step 1: Update existing slots - refresh timestamp if alert still exists
    for (int c = 0; c < 2; c++) {
        if (cards[c].lastSeen == 0) continue;
        
        bool stillExists = false;
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(cards[c].alert, alerts[i])) {
                    stillExists = true;
                    cards[c].alert = alerts[i];  // Update with latest data
                    cards[c].lastSeen = now;
                    break;
                }
            }
        }
        
        // Expire if past grace period
        if (!stillExists) {
            unsigned long age = now - cards[c].lastSeen;
            if (age > gracePeriodMs) {
                cards[c].alert = AlertData();
                cards[c].lastSeen = 0;
            }
        }
    }
    
    // Step 2: Add new non-priority alerts to empty slots
    // Skip priority alert - it's shown in the main display, not as a card
    if (alerts != nullptr) {
        for (int i = 0; i < alertCount; i++) {
            if (!alerts[i].isValid || alerts[i].band == BAND_NONE) continue;
            if (isSameAsPriority(alerts[i])) continue;  // Skip priority - don't waste a card slot
            
            // Check if already tracked
            bool found = false;
            for (int c = 0; c < 2; c++) {
                if (cards[c].lastSeen > 0 && alertsMatch(cards[c].alert, alerts[i])) {
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                // Find empty slot
                for (int c = 0; c < 2; c++) {
                    if (cards[c].lastSeen == 0) {
                        cards[c].alert = alerts[i];
                        cards[c].lastSeen = now;
                        break;
                    }
                }
            }
        }
    }
    
    // For debug logging if needed
    [[maybe_unused]] bool doDebug = false;
    
    // Helper: get signal bars for an alert based on direction
    auto getAlertBars = [](const AlertData& a) -> uint8_t {
        if (a.direction & DIR_FRONT) return a.frontStrength;
        if (a.direction & DIR_REAR) return a.rearStrength;
        return (a.frontStrength > a.rearStrength) ? a.frontStrength : a.rearStrength;
    };
    
    // Build list of cards to draw this frame (V1 alerts only)
    struct CardToDraw {
        int slot;           // V1 card slot index
        bool isGraced;
        uint8_t bars;       // Signal strength for V1 cards
    } cardsToDraw[2];
    int cardsToDrawCount = 0;
    
    // Add V1 secondary alerts
    for (int c = 0; c < 2 && cardsToDrawCount < 2; c++) {
        if (cards[c].lastSeen == 0) continue;
        if (isSameAsPriority(cards[c].alert)) continue;
        cardsToDraw[cardsToDrawCount].slot = c;
        cardsToDraw[cardsToDrawCount].bars = getAlertBars(cards[c].alert);
        // Check if live or graced
        bool isLive = false;
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(cards[c].alert, alerts[i])) {
                    isLive = true;
                    break;
                }
            }
        }
        cardsToDraw[cardsToDrawCount].isGraced = !isLive;
        cardsToDrawCount++;
    }
    
    // === INCREMENTAL UPDATE LOGIC ===
    // Instead of clearing all cards and redrawing, check each position independently
    
    // Capture dirty.cards before resetting (need it for redraw checks)
    bool doForceRedraw = dirty.cards;
    dirty.cards = false;  // Reset the force flag
    
    // Helper to check if position needs full redraw vs just update
    auto positionNeedsFullRedraw = [&](int pos) -> bool {
        if (pos >= cardsToDrawCount) {
            // Position now empty but had content - needs clear
            return lastDrawnPositions[pos].band != BAND_NONE;
        }
        
        auto& last = lastDrawnPositions[pos];
        auto& curr = cardsToDraw[pos];
        
        // V1 card - check if band/freq/direction changed (needs full card redraw)
        // Use frequency tolerance (±5 MHz) to handle V1 jitter
        const uint32_t FREQ_TOLERANCE_MHZ = 5;
        int slot = curr.slot;
        if (cards[slot].alert.band != last.band) return true;
        uint32_t freqDiff = (cards[slot].alert.frequency > last.frequency) 
            ? (cards[slot].alert.frequency - last.frequency) 
            : (last.frequency - cards[slot].alert.frequency);
        if (freqDiff > FREQ_TOLERANCE_MHZ) return true;
        if (cards[slot].alert.direction != last.direction) return true;
        if (curr.isGraced != last.isGraced) return true;
        if (muted != last.wasMuted) return true;
        return false;
    };
    
    // Helper to check if position needs dynamic update (bars only)
    auto positionNeedsDynamicUpdate = [&](int pos) -> bool {
        if (pos >= cardsToDrawCount) return false;
        
        auto& last = lastDrawnPositions[pos];
        auto& curr = cardsToDraw[pos];
        
        // V1 card - check signal bars
        if (curr.bars != last.bars) return true;
        return false;
    };
    
    [[maybe_unused]] const int signalBarsX = SCREEN_WIDTH - 200 - 2;
    
    // Process each card position
    for (int i = 0; i < 2; i++) {
        int cardX = startX + i * (cardW + cardSpacing);
        
        bool needsFullRedraw = positionNeedsFullRedraw(i) || doForceRedraw;
        bool needsDynamicUpdate = !needsFullRedraw && positionNeedsDynamicUpdate(i);
        
        // Clear position if it's now empty
        if (i >= cardsToDrawCount) {
            if (lastDrawnPositions[i].band != BAND_NONE) {
                FILL_RECT(cardX, cardY, cardW, cardH, PALETTE_BG);
                lastDrawnPositions[i].band = BAND_NONE;
            }
            continue;
        }
        
        if (!needsFullRedraw && !needsDynamicUpdate) {
            continue;  // Skip this position - nothing changed
        }
        
        // === V1 ALERT CARD ===
        int c = cardsToDraw[i].slot;
        const AlertData& alert = cards[c].alert;
        bool isGraced = cardsToDraw[i].isGraced;
        bool drawMuted = muted || isGraced;
        uint8_t bars = cardsToDraw[i].bars;
        
        // Card background and border colors
        uint16_t bandCol = getBandColor(alert.band);
        uint16_t bgCol, borderCol;
        
        if (isGraced) {
            bgCol = 0x2104;
            borderCol = PALETTE_MUTED;
        } else if (drawMuted) {
            bgCol = 0x2104;
            borderCol = PALETTE_MUTED;
        } else {
            uint8_t r = ((bandCol >> 11) & 0x1F) * 3 / 10;
            uint8_t g = ((bandCol >> 5) & 0x3F) * 3 / 10;
            uint8_t b = (bandCol & 0x1F) * 3 / 10;
            bgCol = (r << 11) | (g << 5) | b;
            borderCol = bandCol;
        }
        
        uint16_t contentCol = (isGraced || drawMuted) ? PALETTE_MUTED : TFT_WHITE;
        uint16_t bandLabelCol = (isGraced || drawMuted) ? PALETTE_MUTED : bandCol;
        
        if (needsFullRedraw) {
            // === FULL V1 CARD REDRAW ===
            FILL_ROUND_RECT(cardX, cardY, cardW, cardH, 5, bgCol);
            DRAW_ROUND_RECT(cardX, cardY, cardW, cardH, 5, borderCol);
            
            const int contentCenterY = cardY + 18;
            [[maybe_unused]] int topRowY = cardY + 11;
            
            // Direction arrow
            int arrowX = cardX + 18;
            int arrowCY = contentCenterY;
            if (alert.direction & DIR_FRONT) {
                tft->fillTriangle(arrowX, arrowCY - 7, arrowX - 6, arrowCY + 5, arrowX + 6, arrowCY + 5, contentCol);
            } else if (alert.direction & DIR_REAR) {
                tft->fillTriangle(arrowX, arrowCY + 7, arrowX - 6, arrowCY - 5, arrowX + 6, arrowCY - 5, contentCol);
            } else if (alert.direction & DIR_SIDE) {
                FILL_RECT(arrowX - 6, arrowCY - 2, 12, 4, contentCol);
            }
            
            // Band + frequency
            int labelX = cardX + 36;
            tft->setTextColor(bandLabelCol);
            tft->setTextSize(2);
            if (alert.band == BAND_LASER) {
                tft->setCursor(labelX, topRowY);
                tft->print("LASER");
            } else {
                const char* bandStr = bandToString(alert.band);
                tft->setCursor(labelX, topRowY);
                tft->print(bandStr);
                
                tft->setTextColor(contentCol);
                int freqX = labelX + strlen(bandStr) * 12 + 4;
                tft->setCursor(freqX, topRowY);
                if (alert.frequency > 0) {
                    char freqStr[10];
                    snprintf(freqStr, sizeof(freqStr), "%.3f", alert.frequency / 1000.0f);
                    tft->print(freqStr);
                } else {
                    tft->print("---");
                }
            }
            
            // Draw meter background
            const int meterY = cardY + 34;
            const int meterX = cardX + 10;
            const int meterW = cardW - 20;
            const int meterH = 18;
            FILL_RECT(meterX, meterY, meterW, meterH, 0x1082);
        }
        
        // Draw/update signal bars (always after full redraw, or on bars change)
        if (needsFullRedraw || needsDynamicUpdate) {
            const int meterY = cardY + 34;
            const int meterX = cardX + 10;
            const int meterW = cardW - 20;
            const int meterH = 18;
            const int barCount = 6;
            const int barSpacing = 2;
            const int barWidth = (meterW - (barCount - 1) * barSpacing) / barCount;
            
            // Clear meter area for bar update (not full redraw which already did it)
            if (!needsFullRedraw) {
                FILL_RECT(meterX, meterY, meterW, meterH, 0x1082);
            }
            
            uint16_t barColors[6] = {
                settings.colorBar1, settings.colorBar2, settings.colorBar3,
                settings.colorBar4, settings.colorBar5, settings.colorBar6
            };
            
            for (int b = 0; b < barCount; b++) {
                int barX = meterX + b * (barWidth + barSpacing);
                int barH = 10;
                int barY = meterY + (meterH - barH) / 2;
                
                if (b < bars) {
                    uint16_t fillColor = (isGraced || drawMuted) ? PALETTE_MUTED : barColors[b];
                    FILL_RECT(barX, barY, barWidth, barH, fillColor);
                } else {
                    DRAW_RECT(barX, barY, barWidth, barH, dimColor(barColors[b], 30));
                }
            }
        }
        
        // Update position tracking for V1 card
        lastDrawnPositions[i].band = alert.band;
        lastDrawnPositions[i].frequency = alert.frequency;
        lastDrawnPositions[i].direction = alert.direction;
        lastDrawnPositions[i].isGraced = isGraced;
        lastDrawnPositions[i].wasMuted = muted;
        lastDrawnPositions[i].bars = bars;
    }
    
    // Update global tracking
    lastDrawnCount = cardsToDrawCount;
#endif
}

// drawBandBadge(), drawBandIndicators() moved to display_bands.cpp (Phase 2K)

// Classic 7-segment frequency display (original V1 style)
// Uses Segment7 TTF font (JBV1 style) if available, falls back to software renderer
void V1Display::drawFrequencyClassic(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();

    // Cache classic output to avoid redraw/flush when nothing changed.
    static char lastText[16] = "";
    static uint16_t lastColor = 0;
    static bool lastUsedOfr = false;
    static bool cacheValid = false;

    if (dirty.frequency) {
        cacheValid = false;
        dirty.frequency = false;
    }

    const bool usingOfr = fontMgr.segment7Ready;
    const bool hasFreq = freqMHz > 0;

    char textBuf[16];
    if (band == BAND_LASER) {
        strcpy(textBuf, "LASER");
    } else if (hasFreq) {
        float freqGhz = freqMHz / 1000.0f;
        snprintf(textBuf, sizeof(textBuf), "%05.3f", freqGhz);
    } else {
        snprintf(textBuf, sizeof(textBuf), "--.---");
    }

    uint16_t freqColor;
    if (usingOfr) {
        if (band == BAND_LASER) {
            freqColor = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL;
        } else if (muted) {
            freqColor = PALETTE_MUTED_OR_PERSISTED;
        } else if (!hasFreq) {
            freqColor = PALETTE_GRAY;
        } else if (isPhotoRadar && s.freqUseBandColor) {
            freqColor = s.colorBandPhoto;  // Photo radar gets its own color
        } else if (s.freqUseBandColor && band != BAND_NONE) {
            freqColor = getBandColor(band);
        } else {
            freqColor = s.colorFrequency;
        }
    } else {
        // Keep fallback color behavior unchanged.
        if (band == BAND_LASER) {
            freqColor = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL;
        } else if (muted) {
            freqColor = PALETTE_MUTED_OR_PERSISTED;
        } else if (!hasFreq) {
            freqColor = PALETTE_GRAY;
        } else if (s.freqUseBandColor && band != BAND_NONE) {
            freqColor = getBandColor(band);
        } else {
            freqColor = s.colorFrequency;
        }
    }

    bool textChanged = (strcmp(lastText, textBuf) != 0);
    bool changed = !cacheValid ||
                   (lastUsedOfr != usingOfr) ||
                   textChanged ||
                   (lastColor != freqColor);
    if (!changed) {
        return;
    }

    if (usingOfr) {
        // Use Segment7 TTF font (JBV1 style)
        const int fontSize = 75;

#if defined(DISPLAY_WAVESHARE_349)
        const int leftMargin = 135;   // After band indicators (avoid clipping Ka)
        const int rightMargin = 200;  // Before signal bars (at X=440)
#else
        const int leftMargin = 0;
        const int rightMargin = 120;
#endif

        // Position frequency centered between mute icon and cards
        const int muteIconBottom = 33;
        int effectiveHeight = getEffectiveScreenHeight();
        int y = muteIconBottom + (effectiveHeight - muteIconBottom - fontSize) / 2 + 13;

        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin;
        if (band == BAND_LASER) {
            FT_BBox bbox = fontMgr.segment7.calculateBoundingBox(
                0, 0, fontSize, Align::Left, Layout::Horizontal, textBuf);
            int textWidth = bbox.xMax - bbox.xMin;
            x = leftMargin + (maxWidth - textWidth) / 2;
        } else {
            int charCount = strlen(textBuf);
            int approxWidth = charCount * 37;  // ~37px per char at fontSize 75
            x = leftMargin + (maxWidth - approxWidth) / 2;
        }
        if (x < leftMargin) x = leftMargin;

        // Clear frequency area (start 10px after leftMargin to avoid clipping Ka)
        // Clamp height to primary zone to avoid clipping cards at Y=118
        const int clearLeft = leftMargin + 10;
        int clearY = y - 5;
        int clearH = fontSize + 10;
        const int maxClearBottom = DisplayLayout::PRIMARY_ZONE_Y + DisplayLayout::PRIMARY_ZONE_HEIGHT;
        if (clearY + clearH > maxClearBottom) clearH = maxClearBottom - clearY;
        if (clearH > 0) {
            FILL_RECT(clearLeft, clearY, maxWidth - 10, clearH, PALETTE_BG);
            markFrequencyDirtyRegion(clearLeft, clearY, maxWidth - 10, clearH);
        }

        // Convert RGB565 to RGB888 for OpenFontRender
        uint8_t bgR = (PALETTE_BG >> 11) << 3;
        uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
        uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
        fontMgr.segment7.setBackgroundColor(bgR, bgG, bgB);
        fontMgr.segment7.setFontSize(fontSize);
        fontMgr.segment7.setFontColor((freqColor >> 11) << 3, ((freqColor >> 5) & 0x3F) << 2, (freqColor & 0x1F) << 3);
        fontMgr.segment7.setCursor(x, y);
        fontMgr.segment7.printf("%s", textBuf);
    } else {
        // Fallback to software 7-segment renderer
#if defined(DISPLAY_WAVESHARE_349)
        const float scale = 2.3f;
#else
        const float scale = 1.7f;
#endif
        SegMetrics m = segMetrics(scale);

        const int muteIconBottom = 33;
        int effectiveHeight = getEffectiveScreenHeight();
        int y = muteIconBottom + (effectiveHeight - muteIconBottom - m.digitH) / 2 + 5;

        int width = 0;
        if (band == BAND_LASER) {
            width = measureSevenSegmentText(textBuf, scale);
        } else {
            width = measureSevenSegmentText(textBuf, scale);
        }

#if defined(DISPLAY_WAVESHARE_349)
        const int leftMargin = 120;
        const int rightMargin = 200;
#else
        const int leftMargin = 0;
        const int rightMargin = 120;
#endif
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - width) / 2;
        if (x < leftMargin) x = leftMargin;

        if (band == BAND_LASER) {
            FILL_RECT(x - 4, y - 4, width + 8, m.digitH + 8, PALETTE_BG);
            markFrequencyDirtyRegion(x - 4, y - 4, width + 8, m.digitH + 8);
            draw14SegmentText(textBuf, x, y, scale, freqColor, PALETTE_BG);
        } else {
            FILL_RECT(x - 2, y, width + 4, m.digitH + 4, PALETTE_BG);
            markFrequencyDirtyRegion(x - 2, y, width + 4, m.digitH + 4);
            drawSevenSegmentText(textBuf, x, y, scale, freqColor, PALETTE_BG);
        }
    }

    strncpy(lastText, textBuf, sizeof(lastText));
    lastText[sizeof(lastText) - 1] = '\0';
    lastColor = freqColor;
    lastUsedOfr = usingOfr;
    cacheValid = true;
}

// Serpentine frequency display - JB's favorite font
void V1Display::drawFrequencySerpentine(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();
    
    // Fall back to Classic style if Serpentine OFR not initialized
    if (!fontMgr.serpentineReady) {
        drawFrequencyClassic(freqMHz, band, muted, isPhotoRadar);
        return;
    }
    
    // Layout constants
    const int fontSize = 65;  // Sized to match display area
    const int leftMargin = 140;   // After band indicators (Ka extends to ~132px)
    const int rightMargin = 200;  // Before signal bars
    const int effectiveHeight = getEffectiveScreenHeight();
    const int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    // Available vertical space: mute icon bottom (31) to card top (116) = 85px
    // freqY is the baseline - with 65px font, visual center needs baseline around 55-60
    const int freqY = 35;  // Baseline Y position
    const int clearTop = 20;  // Top of frequency area to clear
    const int clearHeight = effectiveHeight - clearTop;  // Full height from top to bottom of zone
    
    // Cache to avoid expensive OFR work when unchanged
    static char lastText[16] = "";
    static uint16_t lastColor = 0;
    static bool cacheValid = false;
    static unsigned long lastDrawMs = 0;
    static int lastDrawX = 0;      // Cache last draw position for minimal clearing
    static int lastDrawWidth = 0;  // Cache last text width
    static constexpr unsigned long FREQ_FORCE_REDRAW_MS = 500;
    static TextWidthCacheEntry widthCache[16];
    static uint8_t widthCacheNextSlot = 0;

    // Check for forced invalidation (e.g., after screen clear)
    if (dirty.frequency) {
        cacheValid = false;
        dirty.frequency = false;  // Clear flag - we're handling it
    }

    // Serpentine style: show nothing when no frequency (resting/idle state)
    // But we must clear the area if we previously drew something
    if (freqMHz == 0 && band != BAND_LASER) {
        if (cacheValid && lastText[0] != '\0') {
            // Clear only the previously drawn text area
            FILL_RECT(lastDrawX - 5, clearTop, lastDrawWidth + 10, clearHeight, PALETTE_BG);
            markFrequencyDirtyRegion(lastDrawX - 5, clearTop, lastDrawWidth + 10, clearHeight);
            lastText[0] = '\0';
            cacheValid = false;
        }
        return;
    }

    // Build text and color
    char textBuf[16];
    if (band == BAND_LASER) {
        strcpy(textBuf, "LASER");
    } else if (freqMHz > 0) {
        snprintf(textBuf, sizeof(textBuf), "%.3f", freqMHz / 1000.0f);
    } else {
        snprintf(textBuf, sizeof(textBuf), "--.---");
    }

    uint16_t freqColor;
    if (band == BAND_LASER) {
        freqColor = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL;
    } else if (muted) {
        freqColor = PALETTE_MUTED_OR_PERSISTED;
    } else if (freqMHz == 0) {
        freqColor = PALETTE_GRAY;
    } else if (isPhotoRadar && s.freqUseBandColor) {
        freqColor = s.colorBandPhoto;  // Photo radar gets its own color
    } else if (s.freqUseBandColor && band != BAND_NONE) {
        freqColor = getBandColor(band);
    } else {
        freqColor = s.colorFrequency;
    }

    // Check if anything changed
    unsigned long nowMs = millis();
    bool textChanged = strcmp(lastText, textBuf) != 0;
    bool changed = !cacheValid ||
                   lastColor != freqColor ||
                   textChanged ||
                   (nowMs - lastDrawMs) >= FREQ_FORCE_REDRAW_MS;

    if (!changed) {
        return;
    }

    // Only recalculate bbox if text actually changed (expensive FreeType call)
    int textW, x;
    if (textChanged || !cacheValid) {
        textW = DisplayFontManager::cachedTextWidth(fontMgr.serpentine, fontSize, textBuf, widthCache, widthCacheNextSlot);
        x = leftMargin + (maxWidth - textW) / 2;
    } else {
        // Reuse cached position for color-only changes
        textW = lastDrawWidth;
        x = lastDrawX;
    }

    // Clear only the area we're about to draw (minimizes flash)
    int clearX = (lastDrawWidth > 0) ? min(x, lastDrawX) - 5 : x - 5;
    int clearW = max(textW, lastDrawWidth) + 10;
    FILL_RECT(clearX, clearTop, clearW, clearHeight, PALETTE_BG);
    markFrequencyDirtyRegion(clearX, clearTop, clearW, clearHeight);

    fontMgr.serpentine.setFontSize(fontSize);
    fontMgr.serpentine.setBackgroundColor(0, 0, 0);  // Black background
    fontMgr.serpentine.setFontColor((freqColor >> 11) << 3, ((freqColor >> 5) & 0x3F) << 2, (freqColor & 0x1F) << 3);

    fontMgr.serpentine.setCursor(x, freqY);
    fontMgr.serpentine.printf("%s", textBuf);

    // Update cache
    strncpy(lastText, textBuf, sizeof(lastText));
    lastText[sizeof(lastText) - 1] = '\0';
    lastColor = freqColor;
    lastDrawX = x;
    lastDrawWidth = textW;
    cacheValid = true;
    lastDrawMs = nowMs;
}

// Draw volume zero warning in the frequency area (flashing red text)
void V1Display::drawVolumeZeroWarning() {
    // Flash at ~2Hz
    static unsigned long lastFlashTime = 0;
    static bool flashOn = true;
    unsigned long now = millis();
    if (now - lastFlashTime >= 250) {
        flashOn = !flashOn;
        lastFlashTime = now;
    }
    
    // Position warning centered in frequency area
#if defined(DISPLAY_WAVESHARE_349)
    const int leftMargin = 120;
    const int rightMargin = 200;
    const int textScale = 6;  // Large for visibility
#else
    const int leftMargin = 0;
    const int rightMargin = 120;
    const int textScale = 4;
#endif
    int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    int centerX = leftMargin + maxWidth / 2;
    int centerY = getEffectiveScreenHeight() / 2 + 10;
    
    // Use default built-in font (NULL) - has all ASCII characters
    // Each char is 6x8 pixels at scale 1, so "VOL 0" = 5 chars * 6 * scale wide
    const char* warningStr = "VOL 0";
    int charW = 6 * textScale;
    int charH = 8 * textScale;
    int textW = 5 * charW;  // 5 characters
    int textX = centerX - textW / 2;
    int textY = centerY - charH / 2;
    
    // Clear the frequency area
    FILL_RECT(leftMargin, textY - 5, maxWidth, charH + 10, PALETTE_BG);
    
    if (flashOn) {
        tft->setFont(NULL);  // Default built-in font
        tft->setTextSize(textScale);
        tft->setTextColor(0xF800, PALETTE_BG);  // Bright red on background
        tft->setCursor(textX, textY);
        tft->print(warningStr);
    }
}

void V1Display::drawCameraToken(const char* token, bool muted) {
    if (!token || token[0] == '\0') {
        token = "CAM";
    }

    const V1Settings& s = settingsManager.get();
    const uint16_t textColor = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorCameraToken;

#if defined(DISPLAY_WAVESHARE_349)
    const int leftMargin = 135;
    const int rightMargin = 200;
#else
    const int leftMargin = 0;
    const int rightMargin = 120;
#endif
    const int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    const int muteIconBottom = 33;
    const int effectiveHeight = getEffectiveScreenHeight();

    // Match the frequency readout renderer/font path (Segment7 / jbv1_2.ttf).
    if (fontMgr.segment7Ready) {
        int fontSize = 75;
        int textWidth = 0;

        // Camera tokens can be wider than frequency values ("SPEED"), so shrink to fit the same field.
        while (fontSize >= 42) {
            const FT_BBox bbox = fontMgr.segment7.calculateBoundingBox(
                0, 0, fontSize, Align::Left, Layout::Horizontal, token);
            textWidth = bbox.xMax - bbox.xMin;
            if (textWidth <= (maxWidth - 10)) {
                break;
            }
            fontSize -= 3;
        }

        if (textWidth <= 0) {
            textWidth = maxWidth - 10;
        }

        int x = leftMargin + (maxWidth - textWidth) / 2;
        if (x < leftMargin) x = leftMargin;

        int y = muteIconBottom + (effectiveHeight - muteIconBottom - fontSize) / 2 + 13;
        int clearY = y - 5;
        int clearH = fontSize + 10;
        const int maxClearBottom = DisplayLayout::PRIMARY_ZONE_Y + DisplayLayout::PRIMARY_ZONE_HEIGHT;
        if (clearY + clearH > maxClearBottom) clearH = maxClearBottom - clearY;
        if (clearH > 0) {
            FILL_RECT(leftMargin + 10, clearY, maxWidth - 10, clearH, PALETTE_BG);
        }

        const uint8_t bgR = (PALETTE_BG >> 11) << 3;
        const uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
        const uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
        fontMgr.segment7.setBackgroundColor(bgR, bgG, bgB);
        fontMgr.segment7.setFontSize(fontSize);
        fontMgr.segment7.setFontColor((textColor >> 11) << 3, ((textColor >> 5) & 0x3F) << 2, (textColor & 0x1F) << 3);
        fontMgr.segment7.setCursor(x, y);
        fontMgr.segment7.printf("%s", token);
        return;
    }

    // Fallback: software 7-segment renderer for environments where OFR did not initialize.
#if defined(DISPLAY_WAVESHARE_349)
    const float scale = 2.3f;
#else
    const float scale = 1.7f;
#endif
    const SegMetrics m = segMetrics(scale);
    const int y = muteIconBottom + (effectiveHeight - muteIconBottom - m.digitH) / 2 + 5;
    int width = measureSevenSegmentText(token, scale);
    if (width <= 0) width = maxWidth;

    int x = leftMargin + (maxWidth - width) / 2;
    if (x < leftMargin) x = leftMargin;

    FILL_RECT(leftMargin, y - 4, maxWidth, m.digitH + 8, PALETTE_BG);
    drawSevenSegmentText(token, x, y, scale, textColor, PALETTE_BG);
}

// Router: calls appropriate frequency draw method based on display style setting
void V1Display::drawFrequency(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();
    if (s.displayStyle == DISPLAY_STYLE_SERPENTINE) {
        fontMgr.ensureSerpentineLoaded(tft);
    }
    frequencyRenderDirty = false;
    frequencyDirtyValid = false;
    frequencyDirtyX = 0;
    frequencyDirtyY = 0;
    frequencyDirtyW = 0;
    frequencyDirtyH = 0;
    
    // Debug: log which style is being used
    static int lastStyleLogged = -1;
    if (s.displayStyle != lastStyleLogged) {
        Serial.printf("[Display] Style changed: %d (0=Classic, 3=Serpentine), serpInit=%d\n",
                      s.displayStyle, fontMgr.serpentineReady);
        lastStyleLogged = s.displayStyle;
    }
    
    if (s.displayStyle == DISPLAY_STYLE_SERPENTINE && fontMgr.serpentineReady) {
        drawFrequencySerpentine(freqMHz, band, muted, isPhotoRadar);
    } else {
        drawFrequencyClassic(freqMHz, band, muted, isPhotoRadar);
    }
}

// drawDirectionArrow() moved to display_arrow.cpp (Phase 2I)

// drawVerticalSignalBars() moved to display_bands.cpp (Phase 2K)

const char* V1Display::bandToString(Band band) {
    return bandName(band);
}

uint16_t V1Display::getBandColor(Band band) {
    const V1Settings& s = settingsManager.get();
    switch (band) {
        case BAND_LASER: return s.colorBandL;
        case BAND_KA: return s.colorBandKa;
        case BAND_K: return s.colorBandK;
        case BAND_X: return s.colorBandX;
        default: return PALETTE_TEXT;
    }
}

void V1Display::updateColorTheme() {
    // Always use standard palette - custom colors are per-element in settings
    currentPalette = ColorThemes::STANDARD();
}

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

// Display logging macro — shared header (also used by display_screens.cpp etc.)
#include "../include/display_log.h"

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

// DISPLAY_FLUSH() macro — see include/display_flush.h
#include "../include/display_flush.h"

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

// drawBaseFrame(), setLockoutIndicator(), setPreQuietActive(),
// drawLockoutIndicator(), setGpsSatellites(), drawGpsIndicator(),
// setObdConnected(), drawObdIndicator() moved to display_indicators.cpp (Phase 2P)

// drawSevenSegmentDigit(), measureSevenSegmentText(), drawSevenSegmentText(),
// draw14SegmentDigit(), draw14SegmentText(), drawTopCounterClassic(),
// drawTopCounter() moved to display_top_counter.cpp (Phase 2O)

// drawVolumeIndicator(), drawRssiIndicator() moved to display_status_bar.cpp (Phase 2N)

// drawMuteIcon() moved to display_top_counter.cpp (Phase 2O)

// drawProfileIndicator(), drawBatteryIndicator(), drawBLEProxyIndicator(),
// drawWiFiIndicator() moved to display_status_bar.cpp (Phase 2N)

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

// markFrequencyDirtyRegion() moved to display_frequency.cpp (Phase 2M)

// showDisconnected(), showResting(), forceNextRedraw(), showScanning(),
// resetChangeTracking(), showDemo(), showBootSplash(), showShutdown(),
// showLowBattery() moved to display_screens.cpp (Phase 3A)

// RSSI periodic update timer (shared between resting and alert modes)
static unsigned long s_lastRssiUpdateMs = 0;
static constexpr unsigned long RSSI_UPDATE_INTERVAL_MS = 2000;  // Update RSSI every 2 seconds

// drawStatusText() moved to display_indicators.cpp (Phase 2P)

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

// drawRestTelemetryCards(), drawSecondaryAlertCards()
// moved to display_cards.cpp (Phase 2L)

// drawBandBadge(), drawBandIndicators() moved to display_bands.cpp (Phase 2K)

// drawFrequencyClassic(), drawFrequencySerpentine(), drawVolumeZeroWarning(),
// drawCameraToken(), drawFrequency(), markFrequencyDirtyRegion()
// moved to display_frequency.cpp (Phase 2M)

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

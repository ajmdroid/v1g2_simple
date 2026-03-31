/**
 * Display Driver for V1 Gen2 Display
 * Supports multiple hardware platforms
 */

#include "display.h"
#include "../include/config.h"
#include "../include/display_layout.h"  // Centralized layout constants
#include "../include/color_themes.h"
#include "../include/band_utils.h"
#include "../include/display_segments.h"  // 7/14-segment data tables
#include "../include/display_ble_freshness.h"
#include "v1simple_logo.h"  // Splash screen image (640x172)
#include "settings.h"
#include "battery_manager.h"
#include "wifi_manager.h"
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

PerfDisplayScreen V1Display::perfScreenForMode(ScreenMode mode) {
    switch (mode) {
        case ScreenMode::Unknown:
            return PerfDisplayScreen::Unknown;
        case ScreenMode::Resting:
            return PerfDisplayScreen::Resting;
        case ScreenMode::Scanning:
            return PerfDisplayScreen::Scanning;
        case ScreenMode::Disconnected:
            return PerfDisplayScreen::Unknown;
        case ScreenMode::Live:
            return PerfDisplayScreen::Live;
        case ScreenMode::Persisted:
            return PerfDisplayScreen::Persisted;
    }
    return PerfDisplayScreen::Unknown;
}

// ============================================================================
// Volume-zero warning state machine
// Shows a flashing "VOL 0" warning when the V1 volume is 0, no app is
// connected, and the lockout pre-quiet feature is not active.
// ============================================================================
// VolumeZeroWarning struct — see include/display_vol_warn.h
#include "../include/display_vol_warn.h"

VolumeZeroWarning volZeroWarn;

// getEffectiveScreenHeight() now lives in display_layout.h

// DISPLAY_FLUSH() macro — see include/display_flush.h
#include "../include/display_flush.h"

// Drawing primitives, coordinate transforms, dimColor — see include/display_draw.h
#include "../include/display_draw.h"

// Platform-specific state kept in display.cpp
    // TFT_BL alias for backlight pin
    #define TFT_BL LCD_BL

// Global display instance reference — defined here, declared extern in display_palette.h
V1Display* g_displayInstance = nullptr;

// Palette helpers and colour macros — see include/display_palette.h
#include "../include/display_palette.h"

// Cross-platform text drawing helpers — see include/display_text.h
#include "../include/display_text.h"

using namespace DisplaySegments;

// TOP_COUNTER_* constants now live in display_layout.h
using DisplayLayout::TOP_COUNTER_FONT_SIZE;
using DisplayLayout::TOP_COUNTER_FIELD_X;
using DisplayLayout::TOP_COUNTER_FIELD_Y;
using DisplayLayout::TOP_COUNTER_FIELD_W;
using DisplayLayout::TOP_COUNTER_FIELD_H;
using DisplayLayout::TOP_COUNTER_TEXT_Y;
using DisplayLayout::TOP_COUNTER_PAD_RIGHT;
using DisplayLayout::TOP_COUNTER_FALLBACK_WIDTH;

V1Display::V1Display() {
    // Initialize with standard theme by default
    currentPalette_ = ColorThemes::STANDARD();
    // Set global instance for color palette access
    g_displayInstance = this;

    currentScreen_ = ScreenMode::Unknown;
    paletteRevision_ = 0;
    lastRestingPaletteRevision_ = 0;
    lastRestingProfileSlot_ = -1;
}

V1Display::~V1Display() = default;

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

    // Ensure restart/re-init paths never leak partially constructed objects.
    tft_.reset();
    gfxPanel_.reset();
    bus_.reset();

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
    bus_.reset(new (std::nothrow) Arduino_ESP32QSPI(
        LCD_CS,    // CS
        LCD_SCLK,  // SCK
        LCD_DATA0, // D0
        LCD_DATA1, // D1
        LCD_DATA2, // D2
        LCD_DATA3  // D3
    ));
    if (!bus_) {
        Serial.println("[Display] ERROR: Failed to create bus!");
        return false;
    }

    // Create AXS15231B panel - native 172x640 portrait
    // Pass GFX_NOT_DEFINED for RST since we already did manual reset
    gfxPanel_.reset(new (std::nothrow) Arduino_AXS15231B(
        bus_.get(),         // bus
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
    ));
    if (!gfxPanel_) {
        Serial.println("[Display] ERROR: Failed to create panel!");
        bus_.reset();
        return false;
    }

    // Create canvas as 172x640 native with rotation=1 for landscape (90°)
    tft_.reset(new (std::nothrow) Arduino_Canvas(172, 640, gfxPanel_.get(), 0, 0, 1));

    if (!tft_) {
        Serial.println("[Display] ERROR: Failed to create canvas!");
        gfxPanel_.reset();
        bus_.reset();
        return false;
    }

    if (!tft_->begin()) {
        Serial.println("[Display] ERROR: tft->begin() failed!");
        tft_.reset();
        gfxPanel_.reset();
        bus_.reset();
        return false;
    }

    tft_->fillScreen(COLOR_BLACK);
    DISPLAY_FLUSH();

    // Turn on backlight (inverted: 0 = full brightness)
    analogWrite(LCD_BL, 0);  // Full brightness (inverted: 0=on)
    delay(30);


    delay(10); // Give hardware time to settle

    tft_->setTextColor(PALETTE_TEXT);
    tft_->setTextSize(2);

    DISPLAY_LOG("[DISPLAY] Initialized successfully %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    logDisplayStage("hw_init");

    // Initialize all OpenFontRender instances via the font manager.
    // Segment7 + TopCounter are loaded immediately; Serpentine is deferred.
    fontMgr.init(tft_);
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
    // PWM brightness control for Arduino_GFX
    // Waveshare 3.49" has INVERTED backlight: 0=full on, 255=off
    #ifdef LCD_BL
    analogWrite(LCD_BL, 255 - level);  // Invert the level
    #endif
}

void V1Display::clear() {
    tft_->fillScreen(PALETTE_BG);
    DISPLAY_FLUSH();
    bleProxyDrawn_ = false;
}

void V1Display::setBleContext(const DisplayBleContext& ctx) {
    bleCtx_ = ctx;
    bleCtxUpdatedAtMs_ = millis();
}

bool V1Display::hasFreshBleContext(uint32_t nowMs) const {
    return DisplayBleFreshness::isFresh(bleCtxUpdatedAtMs_, nowMs);
}

void V1Display::setBLEProxyStatus(bool proxyEnabled, bool clientConnected, bool receivingData) {
#if defined(DISPLAY_WAVESHARE_349)
    // Detect app disconnect - was connected, now isn't
    // Reset VOL 0 warning state immediately so it can trigger again
    if (bleProxyClientConnected_ && !clientConnected) {
        volZeroWarn.reset();
    }

    // Check if proxy client connection changed - update RSSI display
    bool proxyChanged = (clientConnected != bleProxyClientConnected_);

    // Check if receiving state changed (for heartbeat visual)
    bool receivingChanged = (receivingData != bleReceivingData_);

    if (bleProxyDrawn_ &&
        proxyEnabled == bleProxyEnabled_ &&
        clientConnected == bleProxyClientConnected_ &&
        !receivingChanged) {
        return;  // No visual change needed
    }

    bleProxyEnabled_ = proxyEnabled;
    bleProxyClientConnected_ = clientConnected;
    bleReceivingData_ = receivingData;
    drawBLEProxyIndicator();

    // Update RSSI display when proxy connection changes
    if (proxyChanged) {
        drawRssiIndicator(bleCtx_.v1Rssi);
    }

    flush();
#endif
}

void V1Display::flush() {
    DISPLAY_FLUSH();
}

void V1Display::flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
    // Constrain region to framebuffer bounds
    if (!tft_ || !gfxPanel_) return;
    int16_t maxW = tft_->width();
    int16_t maxH = tft_->height();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x >= maxW || y >= maxH) return;
    if (x + w > maxW) w = maxW - x;
    if (y + h > maxH) h = maxH - y;
    const uint32_t areaPx = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);

    uint16_t* fb = tft_->getFramebuffer();
    if (!fb) {
        DISPLAY_FLUSH();
        return;
    }

    const uint32_t startUs = PERF_TIMESTAMP_US();
    int16_t stride = tft_->width();
    // Fast path: when region spans the full display width the rows are
    // contiguous in the framebuffer — issue a single draw call instead of h
    // separate calls to avoid per-row QSPI bus-setup overhead.
    if (w == stride) {
        uint16_t* regionStart = fb + static_cast<uint32_t>(y) * static_cast<uint32_t>(stride) + x;
        gfxPanel_->draw16bitRGBBitmap(x, y, regionStart, w, h);
        PERF_INC(displayFlushBatchCount);
    } else {
        for (int16_t row = 0; row < h; ++row) {
            uint16_t* rowPtr = fb + (y + row) * stride + x;
            gfxPanel_->draw16bitRGBBitmap(x, y + row, rowPtr, w, 1);
        }
    }
    perfRecordFlushUs(PERF_TIMESTAMP_US() - startUs, areaPx, false);
}

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
    currentPalette_ = ColorThemes::STANDARD();
}

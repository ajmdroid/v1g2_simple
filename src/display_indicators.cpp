/**
 * Indicator badges & frame — extracted from display.cpp (Phase 2P)
 *
 * Contains drawBaseFrame, drawLockoutIndicator, drawGpsIndicator,
 * drawObdIndicator, drawStatusText, and associated setters.
 */

#include "display.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "settings.h"

// ============================================================================
// Base frame
// ============================================================================

void V1Display::invalidateAllCaches() {
    bleProxyDrawn = false;  // Force indicator redraw after full clears
    dirty.setAll();         // Invalidate every element cache
    dirty.cards = true;     // Force secondary card row redraw on next render
    frequencyRenderDirty = false;
    frequencyDirtyValid = false;
}

void V1Display::drawBaseFrame() {
    // Clean black background (t4s3-style)
    TFT_CALL(fillScreen)(PALETTE_BG);
    invalidateAllCaches();
    drawBLEProxyIndicator();  // Redraw BLE icon after screen clear
}

// ============================================================================
// Lockout indicator
// ============================================================================

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

// ============================================================================
// GPS satellite indicator ("G" + sat count badge, left of MUTED)
// ============================================================================

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
        // User-configurable GPS text colour (no background/border).
        const V1Settings& s = settingsManager.get();
        const uint16_t textColor = s.colorGps;

        FILL_RECT(x, y, w, h, PALETTE_BG);

        char buf[6];
        snprintf(buf, sizeof(buf), "G%u", curSats);

        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2);
        TFT_CALL(setTextColor)(textColor, PALETTE_BG);
        GFX_drawString(tft, buf, x + w / 2, y + h / 2);
    } else {
        FILL_RECT(x, y, w, h, PALETTE_BG);
    }
#endif
}

// ============================================================================
// OBD connected indicator ("OBD" badge, right of lockout "L")
// ============================================================================

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

        FILL_RECT(x, y, w, h, PALETTE_BG);

        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2);
        TFT_CALL(setTextColor)(textColor, PALETTE_BG);
        GFX_drawString(tft, "OBD", x + w / 2, y + h / 2);
    } else {
        FILL_RECT(x, y, w, h, PALETTE_BG);
    }
#endif
}

// ============================================================================
// Status text (centered message)
// ============================================================================

void V1Display::drawStatusText(const char* text, uint16_t color) {
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    GFX_drawString(tft, text, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
}

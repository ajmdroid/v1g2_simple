/**
 * Indicator badges & frame — extracted from display.cpp (Phase 2P)
 *
 * Contains drawBaseFrame, drawLockoutIndicator, drawGpsIndicator,
 * drawStatusText, and associated setters.
 */

#include "display.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "settings.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/obd/obd_runtime_module.h"

extern GpsRuntimeModule gpsRuntimeModule;
extern ObdRuntimeModule obdRuntimeModule;

// ============================================================================
// Base frame
// ============================================================================

void V1Display::drawBaseFrame() {
    // Clean black background (t4s3-style)
    TFT_CALL(fillScreen)(PALETTE_BG);
    prepareFullRedrawNoClear();
}

void V1Display::prepareFullRedrawNoClear() {
    bleProxyDrawn_ = false;  // Force indicator redraw after full clears
    dirty.setAll();         // Invalidate every element cache after screen clear
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

void V1Display::setSpeedVolZeroActive(bool active) {
    speedVolZeroActive_ = active;
}

// ============================================================================
// File-scoped static cache variables for lockout and GPS indicators
// ============================================================================
static bool s_lockoutLastShown = false;
static bool s_gpsLastShown = false;
static uint8_t s_gpsLastSats = 0;
static bool s_obdLastShown = false;
static bool s_obdLastConnected = false;
static bool s_obdLastAttention = false;

void V1Display::drawLockoutIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    if (!dirty.lockout && lockoutIndicatorShown_ == s_lockoutLastShown) {
        return;
    }
    dirty.lockout = false;
    s_lockoutLastShown = lockoutIndicatorShown_;

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
        GFX_drawString(tft_, "L", x + sz / 2, y + sz / 2);
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

void V1Display::setObdStatus(bool enabled, bool connected, bool scanAttention) {
    obdEnabled_ = enabled;
    obdConnected_ = connected;
    obdScanAttention_ = scanAttention;
}

void V1Display::setObdAttention(bool attention) {
    if (obdAttention_ == attention) {
        return;
    }
    obdAttention_ = attention;
    dirty.obdIndicator = true;
}

void V1Display::refreshObdIndicator(uint32_t nowMs) {
    syncTopIndicators(nowMs);
    drawObdIndicator();
}

void V1Display::syncTopIndicators(uint32_t nowMs) {
    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    setGpsSatellites(gpsStatus.enabled, gpsStatus.stableHasFix, gpsStatus.stableSatellites);

    const ObdRuntimeStatus obdStatus = obdRuntimeModule.snapshot(nowMs);
    setObdStatus(obdStatus.enabled,
                 obdStatus.connected,
                 obdStatus.scanInProgress || obdStatus.manualScanPending);
}

void V1Display::drawGpsIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    // Build current desired state: show when GPS enabled and has fix.
    const bool wantShow = gpsSatEnabled_ && gpsSatHasFix_;
    const uint8_t curSats = wantShow ? gpsSatCount_ : 0;

    if (!dirty.gpsIndicator &&
        wantShow == s_gpsLastShown && curSats == s_gpsLastSats) {
        return;
    }
    dirty.gpsIndicator = false;
    s_gpsLastShown = wantShow;
    s_gpsLastSats  = curSats;

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
        GFX_drawString(tft_, buf, x + w / 2, y + h / 2);
    } else {
        FILL_RECT(x, y, w, h, PALETTE_BG);
    }
#endif
}

// ============================================================================
// OBD indicator ("OBD" text, right of lockout badge)
// ============================================================================

void V1Display::drawObdIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const bool wantShow = obdEnabled_;
    const bool curConnected = wantShow && obdConnected_;
    const bool curAttention = wantShow && !curConnected && (obdScanAttention_ || obdAttention_);

    if (!dirty.obdIndicator &&
        wantShow == s_obdLastShown &&
        curConnected == s_obdLastConnected &&
        curAttention == s_obdLastAttention) {
        return;
    }
    dirty.obdIndicator = false;
    s_obdLastShown = wantShow;
    s_obdLastConnected = curConnected;
    s_obdLastAttention = curAttention;

    // Position: right of lockout badge, before signal bars.
    const int x = 370;
    const int y = 5;
    const int h = 26;
    const int w = 50;

    FILL_RECT(x, y, w, h, PALETTE_BG);
    if (!wantShow) {
        return;
    }

    const V1Settings& s = settingsManager.get();
    const uint16_t textColor = curConnected ? s.colorObd : (curAttention ? 0xF800 : s.colorMuted);

    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(textColor, PALETTE_BG);
    GFX_drawString(tft_, "OBD", x + w / 2, y + h / 2);
#endif
}

// ============================================================================
// Status text (centered message)
// ============================================================================

void V1Display::drawStatusText(const char* text, uint16_t color) {
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    GFX_drawString(tft_, text, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
}

// ============================================================================
// Reset indicator rendering caches
// ============================================================================
void V1Display::resetIndicatorsCache() {
    s_lockoutLastShown = false;
    s_gpsLastShown = false;
    s_gpsLastSats = 0;
    s_obdLastShown = false;
    s_obdLastConnected = false;
    s_obdLastAttention = false;
}

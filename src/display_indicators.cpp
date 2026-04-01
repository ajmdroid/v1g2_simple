/**
 * Indicator badges & frame — extracted from display.cpp (Phase 2P)
 *
 * Contains drawBaseFrame, drawStatusText, and associated setters.
 */

#include "display.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "settings.h"
#include "modules/obd/obd_runtime_module.h"

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

void V1Display::setSpeedVolZeroActive(bool active) {
    speedVolZeroActive_ = active;
}

// ============================================================================
// File-scoped static cache variables for OBD indicator
// ============================================================================
// Thread safety: these caches are read/written only from the main loop
// (via display update calls). Not safe for concurrent access.
static bool s_obdLastShown = false;
static bool s_obdLastConnected = false;
static bool s_obdLastAttention = false;

// ============================================================================
// Status indicators
// ============================================================================

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
    const ObdRuntimeStatus obdStatus = obdRuntimeModule.snapshot(nowMs);
    setObdStatus(obdStatus.enabled,
                 obdStatus.connected,
                 obdStatus.scanInProgress || obdStatus.manualScanPending);
}

// ============================================================================
// OBD indicator ("OBD" text badge)
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

    // Position: before signal bars.
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
    s_obdLastShown = false;
    s_obdLastConnected = false;
    s_obdLastAttention = false;
}

/**
 * Indicator badges & frame — extracted from display.cpp (Phase 2P)
 *
 * Contains drawBaseFrame, drawStatusText, and associated setters.
 */

#include "display.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_element_caches.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "settings.h"
#include "modules/obd/obd_runtime_module.h"

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
    dirty.setAll();                      // Retains multiAlert, obdIndicator, cards, resetTracking
    g_elementCaches.invalidateAll();     // Directly zeros all per-element render caches
    drawBLEProxyIndicator();  // Redraw BLE icon after screen clear
}

void V1Display::setSpeedVolZeroActive(bool active) {
    speedVolZeroActive_ = active;
}

// OBD indicator render cache is in g_elementCaches.obd

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
    g_elementCaches.obd.invalidate();   // Direct cache invalidation at the source
}

void V1Display::setObdRuntimeModule(ObdRuntimeModule* m) {
    obdRtMod_ = m;
}

void V1Display::refreshObdIndicator(uint32_t nowMs) {
    syncTopIndicators(nowMs);
    drawObdIndicator();
}

void V1Display::syncTopIndicators(uint32_t nowMs) {
    if (!obdRtMod_) return;
    const ObdRuntimeStatus obdStatus = obdRtMod_->snapshot(nowMs);
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
        g_elementCaches.obd.valid &&
        wantShow == g_elementCaches.obd.lastShown &&
        curConnected == g_elementCaches.obd.lastConnected &&
        curAttention == g_elementCaches.obd.lastAttention) {
        return;
    }
    dirty.obdIndicator = false;     // Still cleared here — it's also read externally for flush
    g_elementCaches.obd.valid = true;
    g_elementCaches.obd.lastShown = wantShow;
    g_elementCaches.obd.lastConnected = curConnected;
    g_elementCaches.obd.lastAttention = curAttention;

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

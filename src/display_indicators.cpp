/**
 * Indicator badges & frame — extracted from display.cpp (Phase 2P)
 *
 * Contains drawBaseFrame, drawStatusText, and associated setters.
 */

#include "display.h"
#include <cstring>
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_element_caches.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "settings.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/alp/alp_runtime_module.h"

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
    if (obdRtMod_) {
        const ObdRuntimeStatus obdStatus = obdRtMod_->snapshot(nowMs);
        setObdStatus(obdStatus.enabled,
                     obdStatus.connected,
                     obdStatus.scanInProgress || obdStatus.manualScanPending);
    }
    if (alpRtMod_) {
        const AlpStatus alpStatus = alpRtMod_->snapshot();
        alpEnabled_ = (alpStatus.state != AlpState::OFF);
        alpStateRaw_ = static_cast<uint8_t>(alpStatus.state);
        alpHbByte1_ = alpStatus.lastHbByte1;
    }
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
// ALP indicator ("ALP" text badge — left of MUTED badge)
// ============================================================================

void V1Display::setAlpRuntimeModule(AlpRuntimeModule* m) {
    alpRtMod_ = m;
}

void V1Display::refreshAlpIndicator(uint32_t nowMs) {
    if (!alpRtMod_) return;
    const AlpStatus status = alpRtMod_->snapshot();
    alpEnabled_ = (status.state != AlpState::OFF);
    alpStateRaw_ = static_cast<uint8_t>(status.state);
    alpHbByte1_ = status.lastHbByte1;
    drawAlpIndicator();
}

void V1Display::drawAlpIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    const bool wantShow = alpEnabled_;

    if (!dirty.alpIndicator &&
        g_elementCaches.alp.valid &&
        wantShow == g_elementCaches.alp.lastShown &&
        alpStateRaw_ == g_elementCaches.alp.lastState &&
        alpHbByte1_ == g_elementCaches.alp.lastHbByte1) {
        return;
    }
    dirty.alpIndicator = false;
    g_elementCaches.alp.valid = true;
    g_elementCaches.alp.lastShown = wantShow;
    g_elementCaches.alp.lastState = alpStateRaw_;
    g_elementCaches.alp.lastHbByte1 = alpHbByte1_;

    // Position: left of MUTED badge (MUTED is at X=225, Y=5)
    const int x = 170;
    const int y = 5;
    const int h = 26;
    const int w = 50;

    FILL_RECT(x, y, w, h, PALETTE_BG);
    if (!wantShow) {
        return;
    }

    // Badge colors match ALP control pad LED — including LISTENING sub-states
    // driven by B0 heartbeat byte1 (speed-gated by ALP's internal GPS):
    //   Grey   — IDLE: enabled, no heartbeats yet
    //   Green  — LISTENING byte1=02: warm-up (~34s after boot)
    //   Orange — LISTENING byte1=03: scanning (below ~24 mph)
    //   Blue   — LISTENING byte1=04: armed (above ~24 mph)
    //   Orange — TEARDOWN: rescanning after alert
    //   Blue   — ALERT_ACTIVE / NOISE_WINDOW: laser detected, jamming
    const V1Settings& s = settingsManager.get();
    const AlpState alpState = static_cast<AlpState>(alpStateRaw_);
    uint16_t textColor;
    switch (alpState) {
        case AlpState::ALERT_ACTIVE:
        case AlpState::NOISE_WINDOW:
            textColor = s.colorAlpArmed;      // Blue — laser detected
            break;
        case AlpState::TEARDOWN:
            textColor = s.colorAlpScan;       // Orange — rescanning
            break;
        case AlpState::LISTENING:
            // Sub-state from B0 heartbeat byte1:
            //   04 = armed (speed above ~24 mph) → blue
            //   03 = scanning (speed below ~24 mph) → orange
            //   02 = warm-up (first ~34s after boot) → green
            if (alpHbByte1_ == 0x04) {
                textColor = s.colorAlpArmed;      // Blue — armed
            } else if (alpHbByte1_ == 0x03) {
                textColor = s.colorAlpScan;       // Orange — scanning
            } else {
                textColor = s.colorAlpConnected;  // Green — warm-up / init
            }
            break;
        default:
            textColor = s.colorMuted;         // Grey — IDLE / OFF
            break;
    }

    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(textColor, PALETTE_BG);
    GFX_drawString(tft_, "ALP", x + w / 2, y + h / 2);
#endif
}

// ============================================================================
// ALP frequency-area override
// ============================================================================

void V1Display::setAlpFrequencyOverride(const char* gunAbbrev) {
    if (!gunAbbrev) {
        clearAlpFrequencyOverride();
        return;
    }
    alpFreqOverride_ = true;
    strncpy(alpFreqText_, gunAbbrev, sizeof(alpFreqText_));
    alpFreqText_[sizeof(alpFreqText_) - 1] = '\0';
}

void V1Display::clearAlpFrequencyOverride() {
    alpFreqOverride_ = false;
    alpFreqText_[0] = '\0';
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

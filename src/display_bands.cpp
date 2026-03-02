/**
 * Band / Signal-bar Renderers — extracted from display.cpp (Phase 2K)
 *
 * drawBandIndicators  — vertical L/Ka/K/X stack (left side)
 * drawBandBadge       — small rounded badge overlay
 * drawVerticalSignalBars — 6-bar signal meter (right side)
 */

#include "display.h"
#include "../include/display_layout.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "../include/FreeSansBold24pt7b.h"
#include "settings.h"
#include "packet_parser.h"
#include <algorithm>  // std::max

// ============================================================================
// Band badge (small overlay for scanning screen)
// ============================================================================
void V1Display::drawBandBadge(Band band) {
    if (band == BAND_NONE) {
        return;
    }
    int bx = 14;
    int by = 10;
    int bw = 60;
    int bh = 22;
    uint16_t col = getBandColor(band);
    FILL_ROUND_RECT(bx, by, bw, bh, 4, col);
    DRAW_ROUND_RECT(bx, by, bw, bh, 4, TFT_WHITE);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextColor)(TFT_WHITE, col);
    TFT_CALL(setTextSize)(2);
    const char* txt = bandToString(band);
    GFX_drawString(tft, txt, bx + bw/2, by + bh/2 + 1);
}

// ============================================================================
// Band indicator stack (vertical L / Ka / K / X on the left)
// ============================================================================
void V1Display::drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits) {
    // Cache to avoid redrawing unchanged bands
    static uint8_t lastEffectiveMask = 0xFF;
    static bool lastMuted = false;
    static bool cacheValid = false;

    // Local blink timer — V1 blinks at ~5 Hz, we match that
    static unsigned long lastBlinkTime = 0;
    static bool blinkOn = true;
    const unsigned long BLINK_INTERVAL_MS = 100;

    unsigned long now = millis();
    if (now - lastBlinkTime >= BLINK_INTERVAL_MS) {
        blinkOn = !blinkOn;
        lastBlinkTime = now;
    }

    // Apply blink: if flash bit is set and we're in OFF phase, treat band as inactive
    uint8_t effectiveBandMask = bandMask;
    if (!blinkOn) {
        effectiveBandMask &= ~bandFlashBits;
    }

    if (dirty.bands) {
        cacheValid = false;
        dirty.bands = false;
    }

    if (cacheValid && effectiveBandMask == lastEffectiveMask && muted == lastMuted) {
        return;
    }

    const int x = 82;
    const int textSize = 1;
    const int spacing = 43;
    const int startY = 55;

    const V1Settings& s = settingsManager.get();
    struct BandCell {
        const char* label;
        uint8_t mask;
        uint16_t color;
    } cells[4] = {
        {"L",  BAND_LASER, s.colorBandL},
        {"Ka", BAND_KA,    s.colorBandKa},
        {"K",  BAND_K,     s.colorBandK},
        {"X",  BAND_X,     s.colorBandX}
    };

    TFT_CALL(setFont)(&FreeSansBold24pt7b);
    TFT_CALL(setTextSize)(textSize);
    GFX_setTextDatum(ML_DATUM);

    static bool s_bandBaselineInit = false;
    static int16_t s_bandBaselineAdjust = 0;
    if (!s_bandBaselineInit) {
        int16_t x1, y1;
        uint16_t w, h;
        TFT_CALL(getTextBounds)("Ka", 0, 0, &x1, &y1, &w, &h);
        s_bandBaselineAdjust = y1;
        s_bandBaselineInit = true;
    }

    const int labelClearW = 50;
    const int labelClearH = 38;

    // Clear once for the whole band stack
    int16_t unionX = x - 5;
    int16_t unionY = 0;
    uint16_t unionW = labelClearW;
    uint16_t unionH = 0;
    for (int i = 0; i < 4; ++i) {
        int labelY = startY + i * spacing;
        labelY += s_bandBaselineAdjust;
        int16_t y = static_cast<int16_t>(labelY - labelClearH / 2);
        if (i == 0) {
            unionY = y;
            unionH = labelClearH;
        } else {
            int16_t maxY = static_cast<int16_t>(unionY + unionH);
            int16_t newMaxY = static_cast<int16_t>(y + labelClearH);
            if (y < unionY) unionY = y;
            if (newMaxY > maxY) maxY = newMaxY;
            unionH = static_cast<uint16_t>(maxY - unionY);
        }
    }
    FILL_RECT(unionX, unionY, unionW, unionH, PALETTE_BG);

    for (int i = 0; i < 4; ++i) {
        bool isActive = (effectiveBandMask & cells[i].mask) != 0;
        int labelY = startY + i * spacing;
        labelY += s_bandBaselineAdjust;
        uint16_t col = isActive ? (muted ? PALETTE_MUTED_OR_PERSISTED : cells[i].color) : TFT_DARKGREY;
        TFT_CALL(setTextColor)(col, PALETTE_BG);
        GFX_drawString(tft, cells[i].label, x, labelY);
    }

    lastEffectiveMask = effectiveBandMask;
    lastMuted = muted;
    cacheValid = true;

    TFT_CALL(setFont)(NULL);
    TFT_CALL(setTextSize)(1);
}

// ============================================================================
// Vertical signal bars (right side, 6-bar meter)
// ============================================================================
void V1Display::drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band, bool muted) {
    const int barCount = 6;

    uint8_t strength = std::max(frontStrength, rearStrength);
    if (strength > 6) strength = 6;

    static uint8_t lastStrength = 0xFF;
    static bool lastMuted = false;
    static bool cacheValid = false;

    if (dirty.signalBars) {
        cacheValid = false;
        dirty.signalBars = false;
    }

    if (cacheValid && strength == lastStrength && muted == lastMuted) {
        return;
    }

    bool hasSignal = (strength > 0);

    const V1Settings& s = settingsManager.get();
    uint16_t barColors[6] = {
        s.colorBar1, s.colorBar2, s.colorBar3,
        s.colorBar4, s.colorBar5, s.colorBar6
    };

    const int barWidth = 44;
    const int barHeight = 14;
    const int barSpacing = 10;
    [[maybe_unused]] const int totalH = barCount * (barHeight + barSpacing) - barSpacing;

    int startX = SCREEN_WIDTH - 200;
    int startY = 18;
    if (startY < 8) startY = 8;

    for (int i = 0; i < barCount; i++) {
        bool wasLit = cacheValid && (i < lastStrength);
        bool isLit = hasSignal && (i < strength);
        bool wasMutedLit = cacheValid && wasLit && lastMuted;
        bool isMutedLit = isLit && muted;

        if (cacheValid && wasLit == isLit && wasMutedLit == isMutedLit) {
            continue;
        }

        int visualIndex = barCount - 1 - i;
        int y = startY + visualIndex * (barHeight + barSpacing);

        uint16_t fillColor;
        if (!isLit) {
            fillColor = 0x1082;
        } else if (muted) {
            fillColor = PALETTE_MUTED;
        } else {
            fillColor = barColors[i];
        }

        FILL_ROUND_RECT(startX, y, barWidth, barHeight, 2, fillColor);
    }

    lastStrength = strength;
    lastMuted = muted;
    cacheValid = true;
}

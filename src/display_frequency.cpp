/**
 * Frequency display rendering — extracted from display.cpp (Phase 2M)
 *
 * Contains the frequency router, Classic (7-segment) and Serpentine (OFR)
 * frequency renderers, volume-zero warning, and the
 * dirty-region tracking helper.
 */

#include "display.h"
#include "../include/display_layout.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "../include/display_segments.h"
#include "display_font_manager.h"
#include "settings.h"
#include <algorithm>
#include <cstring>

using namespace DisplaySegments;
using DisplayLayout::PRIMARY_ZONE_HEIGHT;

// Convenience alias (matches display.cpp)
using TextWidthCacheEntry = DisplayFontManager::WidthCacheEntry;

// ============================================================================
// File-scoped static cache variables for frequency displays
// ============================================================================
static char s_freqClassicLastText[16] = "";
static uint16_t s_freqClassicLastColor = 0;
static bool s_freqClassicLastUsedOfr = false;
static bool s_freqClassicCacheValid = false;
static int s_freqClassicLastDrawX = 0;
static int s_freqClassicLastDrawWidth = 0;
static TextWidthCacheEntry s_freqClassicWidthCache[16];
static uint8_t s_freqClassicWidthCacheNextSlot = 0;
static int s_freqClassicCachedNumericWidth = 0;
static int s_freqClassicCachedDashWidth = 0;
static int s_freqClassicCachedLaserWidth = 0;

static char s_freqSerpentineLastText[16] = "";
static uint16_t s_freqSerpentineLastColor = 0;
static bool s_freqSerpentineCacheValid = false;
static unsigned long s_freqSerpentineLastDrawMs = 0;
static int s_freqSerpentineLastDrawX = 0;
static int s_freqSerpentineLastDrawWidth = 0;
static TextWidthCacheEntry s_freqSerpentineWidthCache[16];
static uint8_t s_freqSerpentineWidthCacheNextSlot = 0;

// Periodic force-redraw for OFR serpentine font to clear any blending artifacts.
static constexpr unsigned long FREQ_FORCE_REDRAW_MS = 5000UL;

// --- Dirty-region tracking for partial refresh ---

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

    if (!frequencyDirtyValid_) {
        frequencyDirtyX_ = x;
        frequencyDirtyY_ = y;
        frequencyDirtyW_ = w;
        frequencyDirtyH_ = h;
        frequencyDirtyValid_ = true;
    } else {
        const int16_t x1 = min(frequencyDirtyX_, x);
        const int16_t y1 = min(frequencyDirtyY_, y);
        const int16_t x2 = max(static_cast<int16_t>(static_cast<int32_t>(frequencyDirtyX_) + static_cast<int32_t>(frequencyDirtyW_)), static_cast<int16_t>(static_cast<int32_t>(x) + static_cast<int32_t>(w)));
        const int16_t y2 = max(static_cast<int16_t>(static_cast<int32_t>(frequencyDirtyY_) + static_cast<int32_t>(frequencyDirtyH_)), static_cast<int16_t>(static_cast<int32_t>(y) + static_cast<int32_t>(h)));
        frequencyDirtyX_ = x1;
        frequencyDirtyY_ = y1;
        frequencyDirtyW_ = x2 - x1;
        frequencyDirtyH_ = y2 - y1;
    }

    frequencyRenderDirty_ = true;
}

// --- Classic 7-segment frequency display (original V1 style) Uses Segment7 TTF font if available, falls back to software renderer ---

void V1Display::drawFrequencyClassic(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();

    if (dirty.frequency) {
        s_freqClassicCacheValid = false;
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

    bool textChanged = (strcmp(s_freqClassicLastText, textBuf) != 0);
    bool changed = !s_freqClassicCacheValid ||
                   (s_freqClassicLastUsedOfr != usingOfr) ||
                   textChanged ||
                   (s_freqClassicLastColor != freqColor);
    if (!changed) {
        return;
    }

    if (usingOfr) {
        // Use Segment7 TTF font
        const int fontSize = 75;

        const int leftMargin = 135;   // After band indicators (avoid clipping Ka)
        const int rightMargin = 200;  // Before signal bars (at X=440)

        // Position frequency centered between mute icon and cards
        const int muteIconBottom = 33;
        int effectiveHeight = getEffectiveScreenHeight();
        int y = muteIconBottom + (effectiveHeight - muteIconBottom - fontSize) / 2 + 13;

        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        if (s_freqClassicCachedNumericWidth <= 0) {
            s_freqClassicCachedNumericWidth = DisplayFontManager::cachedTextWidth(
                fontMgr.segment7, fontSize, "88.888", s_freqClassicWidthCache, s_freqClassicWidthCacheNextSlot);
        }
        if (s_freqClassicCachedDashWidth <= 0) {
            s_freqClassicCachedDashWidth = DisplayFontManager::cachedTextWidth(
                fontMgr.segment7, fontSize, "--.---", s_freqClassicWidthCache, s_freqClassicWidthCacheNextSlot);
        }
        if (s_freqClassicCachedLaserWidth <= 0) {
            s_freqClassicCachedLaserWidth = DisplayFontManager::cachedTextWidth(
                fontMgr.segment7, fontSize, "LASER", s_freqClassicWidthCache, s_freqClassicWidthCacheNextSlot);
        }

        int textWidth = s_freqClassicCachedNumericWidth;
        if (band == BAND_LASER) {
            textWidth = s_freqClassicCachedLaserWidth;
        } else if (!hasFreq) {
            textWidth = s_freqClassicCachedDashWidth;
        }

        int x = leftMargin + (maxWidth - textWidth) / 2;
        if (x < leftMargin) x = leftMargin;

        // Clear only the union of old/new text bounds to reduce canvas work.
        int clearY = y - 5;
        int clearH = fontSize + 10;
        const int maxClearBottom = DisplayLayout::PRIMARY_ZONE_Y + DisplayLayout::PRIMARY_ZONE_HEIGHT;
        if (clearY + clearH > maxClearBottom) clearH = maxClearBottom - clearY;
        int clearLeft = x - 6;
        int clearRight = x + textWidth + 6;
        if (s_freqClassicCacheValid && s_freqClassicLastUsedOfr && s_freqClassicLastDrawWidth > 0) {
            clearLeft = std::min(clearLeft, s_freqClassicLastDrawX - 6);
            clearRight = std::max(clearRight, s_freqClassicLastDrawX + s_freqClassicLastDrawWidth + 6);
        }
        const int clearMinX = leftMargin + 10;
        const int clearMaxX = leftMargin + maxWidth;
        clearLeft = std::max(clearLeft, clearMinX);
        clearRight = std::min(clearRight, clearMaxX);
        const int clearW = clearRight - clearLeft;
        if (clearH > 0 && clearW > 0) {
            FILL_RECT(clearLeft, clearY, clearW, clearH, PALETTE_BG);
            markFrequencyDirtyRegion(clearLeft, clearY, clearW, clearH);
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

        s_freqClassicLastDrawX = x;
        s_freqClassicLastDrawWidth = textWidth;
    } else {
        // Fallback to software 7-segment renderer
        const float scale = 2.3f;
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

        const int leftMargin = 120;
        const int rightMargin = 200;
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

    strncpy(s_freqClassicLastText, textBuf, sizeof(s_freqClassicLastText));
    s_freqClassicLastText[sizeof(s_freqClassicLastText) - 1] = '\0';
    s_freqClassicLastColor = freqColor;
    s_freqClassicLastUsedOfr = usingOfr;
    s_freqClassicCacheValid = true;
}

// --- Serpentine frequency display ---

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

    // Check for forced invalidation (e.g., after screen clear)
    if (dirty.frequency) {
        s_freqSerpentineCacheValid = false;
        dirty.frequency = false;  // Clear flag - we're handling it
    }

    // Serpentine style: show nothing when no frequency (resting/idle state)
    // But we must clear the area if we previously drew something
    if (freqMHz == 0 && band != BAND_LASER) {
        if (s_freqSerpentineCacheValid && s_freqSerpentineLastText[0] != '\0') {
            // Clear only the previously drawn text area
            FILL_RECT(s_freqSerpentineLastDrawX - 5, clearTop, s_freqSerpentineLastDrawWidth + 10, clearHeight, PALETTE_BG);
            markFrequencyDirtyRegion(s_freqSerpentineLastDrawX - 5, clearTop, s_freqSerpentineLastDrawWidth + 10, clearHeight);
            s_freqSerpentineLastText[0] = '\0';
            s_freqSerpentineCacheValid = false;
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
    bool textChanged = strcmp(s_freqSerpentineLastText, textBuf) != 0;
    bool changed = !s_freqSerpentineCacheValid ||
                   s_freqSerpentineLastColor != freqColor ||
                   textChanged ||
                   (nowMs - s_freqSerpentineLastDrawMs) >= FREQ_FORCE_REDRAW_MS;

    if (!changed) {
        return;
    }

    // Only recalculate bbox if text actually changed (expensive FreeType call)
    int textW, x;
    if (textChanged || !s_freqSerpentineCacheValid) {
        textW = DisplayFontManager::cachedTextWidth(fontMgr.serpentine, fontSize, textBuf, s_freqSerpentineWidthCache, s_freqSerpentineWidthCacheNextSlot);
        x = leftMargin + (maxWidth - textW) / 2;
    } else {
        // Reuse cached position for color-only changes
        textW = s_freqSerpentineLastDrawWidth;
        x = s_freqSerpentineLastDrawX;
    }

    // Clear only the area we're about to draw (minimizes flash)
    int clearX = (s_freqSerpentineLastDrawWidth > 0) ? min(x, s_freqSerpentineLastDrawX) - 5 : x - 5;
    int clearW = max(textW, s_freqSerpentineLastDrawWidth) + 10;
    FILL_RECT(clearX, clearTop, clearW, clearHeight, PALETTE_BG);
    markFrequencyDirtyRegion(clearX, clearTop, clearW, clearHeight);

    fontMgr.serpentine.setFontSize(fontSize);
    fontMgr.serpentine.setBackgroundColor(0, 0, 0);  // Black background
    fontMgr.serpentine.setFontColor((freqColor >> 11) << 3, ((freqColor >> 5) & 0x3F) << 2, (freqColor & 0x1F) << 3);

    fontMgr.serpentine.setCursor(x, freqY);
    fontMgr.serpentine.printf("%s", textBuf);

    // Update cache
    strncpy(s_freqSerpentineLastText, textBuf, sizeof(s_freqSerpentineLastText));
    s_freqSerpentineLastText[sizeof(s_freqSerpentineLastText) - 1] = '\0';
    s_freqSerpentineLastColor = freqColor;
    s_freqSerpentineLastDrawX = x;
    s_freqSerpentineLastDrawWidth = textW;
    s_freqSerpentineCacheValid = true;
    s_freqSerpentineLastDrawMs = nowMs;
}

// ============================================================================
// Reset frequency rendering caches
// ============================================================================
void V1Display::resetFrequencyCache() {
    memset(s_freqClassicLastText, 0, sizeof(s_freqClassicLastText));
    s_freqClassicLastColor = 0;
    s_freqClassicLastUsedOfr = false;
    s_freqClassicCacheValid = false;
    s_freqClassicLastDrawX = 0;
    s_freqClassicLastDrawWidth = 0;
    s_freqClassicWidthCacheNextSlot = 0;
    s_freqClassicCachedNumericWidth = 0;
    s_freqClassicCachedDashWidth = 0;
    s_freqClassicCachedLaserWidth = 0;

    memset(s_freqSerpentineLastText, 0, sizeof(s_freqSerpentineLastText));
    s_freqSerpentineLastColor = 0;
    s_freqSerpentineCacheValid = false;
    s_freqSerpentineLastDrawMs = 0;
    s_freqSerpentineLastDrawX = 0;
    s_freqSerpentineLastDrawWidth = 0;
    s_freqSerpentineWidthCacheNextSlot = 0;
}

// --- Volume zero warning (flashing red text in frequency area) ---

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
    const int leftMargin = 120;
    const int rightMargin = 200;
    const int textScale = 6;  // Large for visibility
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
        tft_->setFont(NULL);  // Default built-in font
        tft_->setTextSize(textScale);
        tft_->setTextColor(0xF800, PALETTE_BG);  // Bright red on background
        tft_->setCursor(textX, textY);
        tft_->print(warningStr);
    }
}

// --- Frequency router — dispatches to Classic or Serpentine based on user setting ---

void V1Display::drawFrequency(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();
    if (s.displayStyle == DISPLAY_STYLE_SERPENTINE) {
        fontMgr.ensureSerpentineLoaded(tft_);
    }
    frequencyRenderDirty_ = false;
    frequencyDirtyValid_ = false;
    frequencyDirtyX_ = 0;
    frequencyDirtyY_ = 0;
    frequencyDirtyW_ = 0;
    frequencyDirtyH_ = 0;

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

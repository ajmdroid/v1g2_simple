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

namespace {

int measure14SegmentTextWidth(const char* text, float scale) {
    SegMetrics m = segMetrics(scale);
    int width = 0;
    size_t glyphCount = 0;
    const size_t len = strlen(text);
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '.') {
            continue;
        }
        width += m.digitW;
        if (i + 1 < len && text[i + 1] == '.') {
            width += m.dot / 2;
            ++i;
        }
        ++glyphCount;
    }
    if (glyphCount > 1) {
        width += static_cast<int>((glyphCount - 1) * m.spacing);
    }
    return width;
}

}  // namespace

// Convenience alias (matches display.cpp)
using TextWidthCacheEntry = DisplayFontManager::WidthCacheEntry;

// ---------------------------------------------------------------------------
// Dirty-region tracking for partial refresh
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Classic 7-segment frequency display (original V1 style)
// Uses Segment7 TTF font if available, falls back to software renderer
// ---------------------------------------------------------------------------

void V1Display::drawFrequencyClassic(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();

    // Cache classic output to avoid redraw/flush when nothing changed.
    static char lastText[16] = "";
    static uint16_t lastColor = 0;
    static bool lastUsedOfr = false;
    static bool cacheValid = false;
    static int lastDrawX = 0;
    static int lastDrawWidth = 0;
    static TextWidthCacheEntry widthCache[16];
    static uint8_t widthCacheNextSlot = 0;
    static int cachedNumericWidth = 0;
    static int cachedDashWidth = 0;
    static int cachedLaserWidth = 0;

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
        // Use Segment7 TTF font
        const int fontSize = 75;

        const int leftMargin = 135;   // After band indicators (avoid clipping Ka)
        const int rightMargin = 200;  // Before signal bars (at X=440)

        // Position frequency centered between mute icon and cards
        const int muteIconBottom = 33;
        int effectiveHeight = getEffectiveScreenHeight();
        int y = muteIconBottom + (effectiveHeight - muteIconBottom - fontSize) / 2 + 13;

        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        if (cachedNumericWidth <= 0) {
            cachedNumericWidth = DisplayFontManager::cachedTextWidth(
                fontMgr.segment7, fontSize, "88.888", widthCache, widthCacheNextSlot);
        }
        if (cachedDashWidth <= 0) {
            cachedDashWidth = DisplayFontManager::cachedTextWidth(
                fontMgr.segment7, fontSize, "--.---", widthCache, widthCacheNextSlot);
        }
        if (cachedLaserWidth <= 0) {
            cachedLaserWidth = DisplayFontManager::cachedTextWidth(
                fontMgr.segment7, fontSize, "LASER", widthCache, widthCacheNextSlot);
        }

        int textWidth = cachedNumericWidth;
        if (band == BAND_LASER) {
            textWidth = cachedLaserWidth;
        } else if (!hasFreq) {
            textWidth = cachedDashWidth;
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
        if (cacheValid && lastUsedOfr && lastDrawWidth > 0) {
            clearLeft = std::min(clearLeft, lastDrawX - 6);
            clearRight = std::max(clearRight, lastDrawX + lastDrawWidth + 6);
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
        lastDrawX = x;
        lastDrawWidth = textWidth;
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

    strncpy(lastText, textBuf, sizeof(lastText));
    lastText[sizeof(lastText) - 1] = '\0';
    lastColor = freqColor;
    lastUsedOfr = usingOfr;
    cacheValid = true;
}

// ---------------------------------------------------------------------------
// Serpentine frequency display
// ---------------------------------------------------------------------------

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

void V1Display::drawCameraLabel(const char* label, uint16_t color) {
    const V1Settings& s = settingsManager.get();
    const char* text = (label != nullptr) ? label : "";

    static char lastText[16] = "";
    static uint16_t lastColor = 0;
    static bool lastUsedSerpentine = false;
    static bool cacheValid = false;
    static int lastDrawX = 0;
    static int lastDrawWidth = 0;
    static unsigned long lastDrawMs = 0;
    static constexpr unsigned long FORCE_REDRAW_MS = 500;
    static TextWidthCacheEntry widthCache[16];
    static uint8_t widthCacheNextSlot = 0;

    if (dirty.frequency) {
        cacheValid = false;
        dirty.frequency = false;
    }

    const bool wantsSerpentine = (s.displayStyle == DISPLAY_STYLE_SERPENTINE);
    if (wantsSerpentine && !fontMgr.serpentineReady) {
        fontMgr.ensureSerpentineLoaded(tft);
    }
    const bool useSerpentine = wantsSerpentine && fontMgr.serpentineReady;

    const unsigned long nowMs = millis();
    const bool textChanged = (strcmp(lastText, text) != 0);
    const bool changed = !cacheValid ||
                         textChanged ||
                         (lastColor != color) ||
                         (lastUsedSerpentine != useSerpentine) ||
                         ((nowMs - lastDrawMs) >= FORCE_REDRAW_MS);
    if (!changed) {
        return;
    }

    const int leftMargin = 140;
    const int rightMargin = 200;
    const int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    int drawX = leftMargin;
    int drawWidth = 0;

    if (useSerpentine) {
        const int fontSize = 65;
        const int baselineY = 35;
        const int clearTop = 20;
        const int clearHeight = getEffectiveScreenHeight() - clearTop;

        if (textChanged || !cacheValid || !lastUsedSerpentine) {
            drawWidth = DisplayFontManager::cachedTextWidth(
                fontMgr.serpentine, fontSize, text, widthCache, widthCacheNextSlot);
            drawX = leftMargin + (maxWidth - drawWidth) / 2;
        } else {
            drawWidth = lastDrawWidth;
            drawX = lastDrawX;
        }

        int clearLeft = drawX - 5;
        int clearRight = drawX + drawWidth + 5;
        if (cacheValid) {
            clearLeft = std::min(clearLeft, lastDrawX - 5);
            clearRight = std::max(clearRight, lastDrawX + lastDrawWidth + 5);
        }
        clearLeft = std::max(clearLeft, leftMargin);
        clearRight = std::min(clearRight, leftMargin + maxWidth);
        const int clearWidth = clearRight - clearLeft;
        if (clearWidth > 0 && clearHeight > 0) {
            FILL_RECT(clearLeft, clearTop, clearWidth, clearHeight, PALETTE_BG);
            markFrequencyDirtyRegion(clearLeft, clearTop, clearWidth, clearHeight);
        }

        fontMgr.serpentine.setFontSize(fontSize);
        fontMgr.serpentine.setBackgroundColor(0, 0, 0);
        fontMgr.serpentine.setFontColor((color >> 11) << 3,
                                        ((color >> 5) & 0x3F) << 2,
                                        (color & 0x1F) << 3);
        fontMgr.serpentine.setCursor(drawX, baselineY);
        fontMgr.serpentine.printf("%s", text);
    } else {
        const float scale = 2.3f;
        const SegMetrics m = segMetrics(scale);
        const int muteIconBottom = 33;
        const int effectiveHeight = getEffectiveScreenHeight();
        const int drawY = muteIconBottom + (effectiveHeight - muteIconBottom - m.digitH) / 2 + 5;
        drawWidth = measure14SegmentTextWidth(text, scale);
        drawX = leftMargin + (maxWidth - drawWidth) / 2;
        if (drawX < leftMargin) {
            drawX = leftMargin;
        }

        int clearLeft = drawX - 4;
        int clearRight = drawX + drawWidth + 4;
        if (cacheValid) {
            clearLeft = std::min(clearLeft, lastDrawX - 4);
            clearRight = std::max(clearRight, lastDrawX + lastDrawWidth + 4);
        }
        clearLeft = std::max(clearLeft, leftMargin);
        clearRight = std::min(clearRight, leftMargin + maxWidth);
        const int clearWidth = clearRight - clearLeft;
        const int clearTop = drawY - 4;
        const int clearHeight = m.digitH + 8;
        if (clearWidth > 0 && clearHeight > 0) {
            FILL_RECT(clearLeft, clearTop, clearWidth, clearHeight, PALETTE_BG);
            markFrequencyDirtyRegion(clearLeft, clearTop, clearWidth, clearHeight);
        }

        draw14SegmentText(text, drawX, drawY, scale, color, PALETTE_BG);
    }

    strncpy(lastText, text, sizeof(lastText));
    lastText[sizeof(lastText) - 1] = '\0';
    lastColor = color;
    lastUsedSerpentine = useSerpentine;
    lastDrawX = drawX;
    lastDrawWidth = drawWidth;
    cacheValid = true;
    lastDrawMs = nowMs;
}

// ---------------------------------------------------------------------------
// Volume zero warning (flashing red text in frequency area)
// ---------------------------------------------------------------------------

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
        tft->setFont(NULL);  // Default built-in font
        tft->setTextSize(textScale);
        tft->setTextColor(0xF800, PALETTE_BG);  // Bright red on background
        tft->setCursor(textX, textY);
        tft->print(warningStr);
    }
}

// ---------------------------------------------------------------------------
// Frequency router — dispatches to Classic or Serpentine based on user setting
// ---------------------------------------------------------------------------

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

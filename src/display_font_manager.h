/**
 * DisplayFontManager — owns all OpenFontRender instances, init flags,
 * glyph caches, and lazy-load helpers that were previously scattered as
 * file-scope statics in display.cpp.
 *
 * Lifetime: a single global instance (`fontMgr`) is defined in display.cpp
 *           and declared extern here for use by display sub-modules.
 *           init() is called once from V1Display::begin().
 *
 * Threading: same single-thread contract as the rest of the display system.
 */
#pragma once

#include "OpenFontRender.h"
#include <cstring>
#include <memory>

class Arduino_Canvas;

struct DisplayFontManager {

    // --- Layout constant shared with display drawing code ---
    static constexpr int TOP_COUNTER_FONT_SIZE = 60;

    // --- Renderers (3 instances) ---
    OpenFontRender segment7;    // Classic style (Segment7)
    OpenFontRender topCounter;  // Dedicated Segment7 renderer for top counter
    OpenFontRender serpentine;  // Serpentine style (lazy-loaded on first demand)

    // --- Init flags ---
    bool segment7Ready    = false;
    bool topCounterReady  = false;
    bool serpentineReady  = false;

    // --- Font cache budget (set once during init) ---
    uint32_t numericCacheBytes      = 8192u;
    bool     serpentineLoadAttempted = false;

    // --- Top-counter glyph bounds cache ---
    static constexpr int16_t BOUNDS_INVALID =
        static_cast<int16_t>(-32768);
    int16_t topCounterXMin[128][2];
    int16_t topCounterXMax[128][2];
    bool    topCounterBoundsReady = false;

    // --- Lifecycle ---

    /// Load Segment7 + TopCounter fonts, prime the top-counter bounds cache.
    /// Serpentine is deferred until ensureSerpentineLoaded().
    void init(Arduino_Canvas* canvas);
    void init(const std::unique_ptr<Arduino_Canvas>& canvas) { init(canvas.get()); }

    /// Pre-render common frequency glyphs once so first live alert draws
    /// don't stall while OpenFontRender builds glyph caches.
    void prewarmSegment7FrequencyGlyphs();

    /// Lazy-load Serpentine font the first time it is requested.
    /// @param canvas  current display canvas (needed for setDrawer).
    /// @return true when the serpentine renderer is ready to use.
    bool ensureSerpentineLoaded(Arduino_Canvas* canvas);
    bool ensureSerpentineLoaded(const std::unique_ptr<Arduino_Canvas>& canvas) {
        return ensureSerpentineLoaded(canvas.get());
    }

    // --- Top-counter bounds helpers ---

    void resetTopCounterBoundsCache();
    void primeTopCounterBoundsCache();
    bool getTopCounterBounds(char symbol, bool showDot, int& xMin, int& xMax);

    // --- Text width cache ---

    /// Small fixed-size LRU cache for OFR text widths.  Re-used by every
    /// drawFrequency* variant that needs cached bounding-box queries.
    struct WidthCacheEntry {
        bool valid    = false;
        char text[16] = {0};
        int  width    = 0;
    };

    /// Look up (or compute + cache) the pixel width of @p text at @p fontSize.
    /// The cache is stored in the caller's local static array so that each
    /// rendering path maintains its own independent history.
    template <size_t N>
    static int cachedTextWidth(OpenFontRender& renderer, int fontSize,
                               const char* text,
                               WidthCacheEntry (&cache)[N],
                               uint8_t& nextSlot);

};

// --- Template implementation (must be visible to all callers) ---
template <size_t N>
int DisplayFontManager::cachedTextWidth(
        OpenFontRender& renderer, int fontSize, const char* text,
        WidthCacheEntry (&cache)[N], uint8_t& nextSlot) {

    for (size_t i = 0; i < N; ++i) {
        if (cache[i].valid && strcmp(cache[i].text, text) == 0) {
            return cache[i].width;
        }
    }

    renderer.setFontSize(fontSize);
    FT_BBox bbox = renderer.calculateBoundingBox(
        0, 0, fontSize, Align::Left, Layout::Horizontal, text);
    int width = bbox.xMax - bbox.xMin;

    WidthCacheEntry& dst = cache[nextSlot];
    dst.valid = true;
    strncpy(dst.text, text, sizeof(dst.text));
    dst.text[sizeof(dst.text) - 1] = '\0';
    dst.width = width;

    nextSlot = static_cast<uint8_t>((nextSlot + 1) % N);
    return width;
}

// Global font manager instance — defined in display.cpp
extern DisplayFontManager fontMgr;

/**
 * DisplayFontManager implementation — font loading, lazy init,
 * and top-counter glyph-bounds cache.
 */

#include "display_font_manager.h"
#include "../include/display_driver.h"     // Arduino_Canvas full definition (via Arduino_GFX_Library)
#include "../include/Segment7Font.h"       // Segment7 TTF binary data
#include "../include/Serpentine.h"         // Serpentine TTF binary data
#include <Arduino.h>                       // millis(), Serial, psramFound()
#include <esp_heap_caps.h>

// ============================================================================
// Lifecycle
// ============================================================================

void DisplayFontManager::init(Arduino_Canvas* canvas) {
    if (!canvas) {
        Serial.println("[FontMgr] ERROR: null canvas in init()");
        return;
    }

    // Determine cache budget based on PSRAM availability
    const bool psramOk = psramFound() && (ESP.getPsramSize() > 0);
    const uint32_t seg7Cache      = psramOk ? 49152u : 8192u;
    const uint32_t topCounterCache = psramOk ? 16384u : 4096u;
    numericCacheBytes      = seg7Cache;
    serpentineLoadAttempted = false;

    Serial.printf("[FontMgr] cache budget: psram=%s seg7=%lu top=%lu serp=%lu\n",
                  psramOk ? "yes" : "no",
                  static_cast<unsigned long>(seg7Cache),
                  static_cast<unsigned long>(topCounterCache),
                  static_cast<unsigned long>(seg7Cache));

    // --- Segment7 (Classic digits) ---
    segment7.setDrawer(*canvas);
    segment7.setCacheSize(1, 4, seg7Cache);
    FT_Error err = segment7.loadFont(Segment7Font, sizeof(Segment7Font));
    segment7Ready = (err == 0);
    if (err) {
        Serial.printf("[FontMgr] ERROR: Segment7 load failed (0x%02X)\n", err);
    }

    // --- TopCounter (dedicated Segment7 instance) ---
    topCounter.setDrawer(*canvas);
    topCounter.setCacheSize(1, 2, topCounterCache);
    err = topCounter.loadFont(Segment7Font, sizeof(Segment7Font));
    topCounterReady = (err == 0);
    if (err) {
        Serial.printf("[FontMgr] ERROR: TopCounter load failed (0x%02X)\n", err);
    }

    // Prime the top-counter glyph-bounds cache at boot so that
    // right-aligned counter layout is stable from the first frame.
    primeTopCounterBoundsCache();
    prewarmSegment7FrequencyGlyphs();

    // --- Serpentine (deferred until first use) ---
    serpentineReady = false;

#ifdef CONFIG_SPIRAM_SUPPORT
    Serial.println("[FontMgr] OFR using PSRAM-preferring allocator (ps_malloc)");
#else
    Serial.println("[FontMgr] OFR using internal heap allocator (malloc)");
#endif

    Serial.printf("[FontMgr] OK fonts(seg7/top/serp)=%d/%d/%d\n",
                  segment7Ready, topCounterReady, serpentineReady);
}

void DisplayFontManager::prewarmSegment7FrequencyGlyphs() {
    if (!segment7Ready) {
        return;
    }

    // Warm the hot-path glyphs used by live frequency rendering so first-hit
    // alerts (e.g., 33.8) do not pay OpenFontRender glyph build latency.
    static constexpr int kWarmFontSize = 75;
    static constexpr const char* kWarmupSamples[] = {
        "33.800",
        "35.500",
        "34.700",
        "24.150",
        "10.525",
        "88.888",
        "--.---",
        "LASER"
    };

    const unsigned long warmStartMs = millis();
    segment7.setBackgroundColor(0, 0, 0);
    segment7.setFontColor(0, 0, 0);  // Render black-on-black into canvas.
    segment7.setFontSize(kWarmFontSize);

    for (const char* sample : kWarmupSamples) {
        // Bound-box pass and draw pass together prime both metric and glyph caches.
        segment7.calculateBoundingBox(0, 0, kWarmFontSize, Align::Left, Layout::Horizontal, sample);
        segment7.setCursor(2, kWarmFontSize);
        segment7.printf("%s", sample);
    }

    Serial.printf("[FontMgr] Segment7 prewarm complete in %lu ms\n",
                  millis() - warmStartMs);
}

bool DisplayFontManager::ensureSerpentineLoaded(Arduino_Canvas* canvas) {
    if (serpentineReady) {
        return true;
    }
    if (serpentineLoadAttempted) {
        return false;
    }
    if (!canvas) {
        return false;
    }

    serpentineLoadAttempted = true;
    const unsigned long loadStartMs = millis();
    serpentine.setDrawer(*canvas);
    serpentine.setCacheSize(1, 4, numericCacheBytes);
    FT_Error err = serpentine.loadFont(Serpentine, sizeof(Serpentine));
    serpentineReady = (err == 0);
    if (serpentineReady) {
        Serial.printf("[FontMgr] Serpentine lazy-loaded in %lu ms\n",
                      millis() - loadStartMs);
    } else {
        Serial.printf("[FontMgr] ERROR: Serpentine lazy load failed (0x%02X)\n", err);
    }
    return serpentineReady;
}

// ============================================================================
// Top-counter glyph bounds cache
// ============================================================================

void DisplayFontManager::resetTopCounterBoundsCache() {
    for (uint8_t c = 0; c < 128; ++c) {
        topCounterXMin[c][0] = BOUNDS_INVALID;
        topCounterXMin[c][1] = BOUNDS_INVALID;
        topCounterXMax[c][0] = BOUNDS_INVALID;
        topCounterXMax[c][1] = BOUNDS_INVALID;
    }
    topCounterBoundsReady = false;
}

void DisplayFontManager::primeTopCounterBoundsCache() {
    resetTopCounterBoundsCache();
    if (!topCounterReady) {
        return;
    }

    topCounter.setFontSize(TOP_COUNTER_FONT_SIZE);
    for (uint8_t c = 32; c < 127; ++c) {
        // Cache both plain and dotted variants for each printable ASCII char
        const char symbol = static_cast<char>(c);

        auto cacheBounds = [&](bool showDot) {
            char text[3] = {symbol, 0, 0};
            if (showDot) {
                text[1] = '.';
            }
            FT_BBox bbox = topCounter.calculateBoundingBox(
                0, 0, TOP_COUNTER_FONT_SIZE, Align::Left, Layout::Horizontal, text);
            int xMin = static_cast<int>(bbox.xMin);
            int xMax = static_cast<int>(bbox.xMax);
            if (xMin < -32767) xMin = -32767;
            if (xMin > 32767)  xMin = 32767;
            if (xMax < -32767) xMax = -32767;
            if (xMax > 32767)  xMax = 32767;
            topCounterXMin[c][showDot ? 1 : 0] = static_cast<int16_t>(xMin);
            topCounterXMax[c][showDot ? 1 : 0] = static_cast<int16_t>(xMax);
        };

        cacheBounds(false);
        cacheBounds(true);
    }
    topCounterBoundsReady = true;
}

bool DisplayFontManager::getTopCounterBounds(
        char symbol, bool showDot, int& xMin, int& xMax) {
    const uint8_t idx = static_cast<uint8_t>(symbol);
    const uint8_t dotIdx = showDot ? 1 : 0;

    if (topCounterBoundsReady && idx < 128) {
        int16_t cachedMin = topCounterXMin[idx][dotIdx];
        int16_t cachedMax = topCounterXMax[idx][dotIdx];
        if (cachedMin != BOUNDS_INVALID && cachedMax != BOUNDS_INVALID) {
            xMin = cachedMin;
            xMax = cachedMax;
            return true;
        }
    }

    if (!topCounterReady) {
        return false;
    }

    char text[3] = {symbol, 0, 0};
    if (showDot) {
        text[1] = '.';
    }
    FT_BBox bbox = topCounter.calculateBoundingBox(
        0, 0, TOP_COUNTER_FONT_SIZE, Align::Left, Layout::Horizontal, text);
    xMin = static_cast<int>(bbox.xMin);
    xMax = static_cast<int>(bbox.xMax);
    return true;
}

// ============================================================================
// Font Degradation / Fallback Helpers
// ============================================================================

bool DisplayFontManager::checkOfrRenderingSuccess(
        OpenFontRender& renderer,
        int32_t preCallX, int32_t preCallY) {
    // After a printf() call, the cursor should have advanced.
    // If it remains at the pre-call position, rendering likely failed.
    int32_t postCallX = renderer.getCursorX();
    int32_t postCallY = renderer.getCursorY();

    // If cursor didn't move in either X or Y, rendering probably failed
    if (postCallX == preCallX && postCallY == preCallY) {
        return false;  // Rendering likely failed
    }

    return true;  // Rendering likely succeeded
}

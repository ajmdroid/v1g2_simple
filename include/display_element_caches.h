#pragma once

// ============================================================================
// Display element render caches — unified struct model
//
// Replaces the anonymous file-scoped s_* statics in each display_*.cpp.
// One struct per rendered element; all collected into DisplayElementCaches.
//
// Lifecycle:
//   - Default-initialized to "invalid" (forces first-run full draw)
//   - DisplayElementCaches::invalidateAll() is called from
//     prepareFullRedrawNoClear() after every screen clear
//   - Each render function sets valid = true after a successful draw
//   - Blink timers, font measurement statics, and active slot state
//     are NOT part of this system — they remain file-scoped statics
// ============================================================================

#include <cstdint>
#include "packet_parser.h"   // Band, AlertData

// --- Arrow render cache ---------------------------------------------------
struct ArrowRenderCache {
    bool showFront    = false;
    bool showSide     = false;
    bool showRear     = false;
    bool muted        = false;
    uint16_t frontCol = 0;
    uint16_t sideCol  = 0;
    uint16_t rearCol  = 0;
    bool raisedLayout = true;
    bool valid        = false;

    void invalidate() { valid = false; }
};

// --- Band indicator render cache ------------------------------------------
struct BandRenderCache {
    uint8_t lastMask  = 0xFF;  // 0xFF = undrawn sentinel
    bool lastMuted    = false;
    bool valid        = false;

    void invalidate() { valid = false; }
};

// --- Signal bars render cache ---------------------------------------------
struct BarsRenderCache {
    uint8_t lastStrength = 0xFF;  // 0xFF = undrawn sentinel
    bool lastMuted       = false;
    bool valid           = false;

    void invalidate() { valid = false; }
};

// --- Classic frequency render cache ---------------------------------------
// NOTE: s_freqClassicWidthCache[16] (LRU font measurement) and
//       s_freqClassicWidthCacheNextSlot stay as file-scoped statics — they
//       are TextWidthCacheEntry arrays, not render state.
// NOTE: s_freqClassicCachedNumericWidth / DashWidth / LaserWidth stay as
//       file-scoped statics — they are font metrics, not render state.
struct FreqClassicRenderCache {
    char     lastText[16] = "";
    uint16_t lastColor    = 0;
    bool     lastUsedOfr  = false;
    int      lastDrawX    = 0;
    int      lastDrawWidth = 0;
    bool     valid        = false;

    void invalidate() { valid = false; }
};

// --- Serpentine frequency render cache ------------------------------------
// NOTE: s_freqSerpentineWidthCache[16] and s_freqSerpentineWidthCacheNextSlot
//       stay as file-scoped statics (same reason as Classic above).
struct FreqSerpentineRenderCache {
    char         lastText[16]  = "";
    uint16_t     lastColor     = 0;
    unsigned long lastDrawMs   = 0;
    int          lastDrawX     = 0;
    int          lastDrawWidth = 0;
    bool         valid         = false;

    void invalidate() { valid = false; lastText[0] = '\0'; }
};

// --- Battery render cache -------------------------------------------------
// NOTE: invalidate() resets to sentinel values matching the original
//       dirty.battery behavior: s_batteryLastPctDrawn = -1 and
//       s_batteryLastPctVisible = false forced full redraw.
struct BatteryRenderCache {
    int          lastPctDrawn    = -1;     // -1 = undrawn sentinel
    bool         lastPctVisible  = false;
    uint16_t     lastPctColor    = 0;
    unsigned long lastPctDrawMs  = 0;

    void invalidate() { lastPctDrawn = -1; lastPctVisible = false; }
};

// --- Top counter render cache (bogey counter + mute icon) -----------------
// NOTE: Both drawTopCounterClassic and drawMuteIcon are in display_top_counter.cpp
//       and share this struct. Each function has its own valid flag.
struct TopCounterRenderCache {
    // Bogey counter sub-cache
    char     lastSymbol       = '\0';
    bool     lastMuted        = false;
    bool     lastShowDot      = false;
    uint16_t lastBogeyColor   = 0;
    bool     counterValid     = false;

    // Mute icon sub-cache
    bool     lastMutedState   = false;
    bool     muteIconValid    = false;

    void invalidate() {
        counterValid  = false;
        muteIconValid = false;
    }
};

// --- OBD indicator render cache -------------------------------------------
struct ObdRenderCache {
    bool lastShown     = false;
    bool lastConnected = false;
    bool lastAttention = false;
    bool valid         = false;

    void invalidate() { valid = false; }
};

// --- ALP indicator render cache -------------------------------------------
struct AlpRenderCache {
    bool lastShown     = false;
    bool lastArmed     = false;   // true = LISTENING or ALERT_ACTIVE
    bool lastAlert     = false;   // true = ALERT_ACTIVE specifically
    bool valid         = false;

    void invalidate() { valid = false; }
};

// --- Secondary alert cards render cache -----------------------------------
// NOTE: s_cardsSlots[2] (active slot state with lastSeen timer) stays as a
//       file-scoped static in display_cards.cpp — it is NOT pure last-drawn.
// NOTE: s_cardsLastDrawnPositions[2] (anonymous struct array per card slot)
//       also stays in display_cards.cpp — it is position-keyed last-drawn data.
// This struct tracks the coarser "force full redraw" flag and the profile/count
// comparison values that logically belong together.
struct CardsRenderCache {
    AlertData lastPriority{};   // Last priority alert drawn (for comparison)
    int       lastDrawnCount  = 0;
    int       lastProfileSlot = -1;
    bool      forceRedraw     = true;   // true = force full redraw on next call

    void invalidate() { forceRedraw = true; }
};

// --- Aggregate -----------------------------------------------------------
struct DisplayElementCaches {
    ArrowRenderCache       arrow;
    BandRenderCache        bands;
    BarsRenderCache        bars;
    FreqClassicRenderCache freqClassic;
    FreqSerpentineRenderCache freqSerpentine;
    BatteryRenderCache     battery;
    TopCounterRenderCache  topCounter;
    ObdRenderCache         obd;
    AlpRenderCache         alp;
    CardsRenderCache       cards;

    /// Call from prepareFullRedrawNoClear() after every screen clear.
    void invalidateAll() {
        arrow.invalidate();
        bands.invalidate();
        bars.invalidate();
        freqClassic.invalidate();
        freqSerpentine.invalidate();
        battery.invalidate();
        topCounter.invalidate();
        obd.invalidate();
        alp.invalidate();
        cards.invalidate();
    }
};

// Single shared instance — defined in display.cpp; included by display sub-modules.
extern DisplayElementCaches g_elementCaches;

# Display Cache Unification — Surgical Plan

> **Status: EXECUTED — all phases complete, pending pio test on hardware**
>
> Pre-read this document entirely before executing any step.
> Execute steps in order. Do NOT skip ahead.
> Run `pio test -e native` after each Phase.

---

## Background: The Two-Layer Problem

The display render system uses two overlapping layers to decide whether to redraw an element:

**Layer 1 — Coarse invalidation (`DisplayDirtyFlags dirty`)**
- 11 bool flags in a global struct (`include/display_dirty_flags.h`)
- Set in bulk by `dirty.setAll()`, called from `prepareFullRedrawNoClear()` after every screen clear
- Some flags also set individually (e.g., `dirty.obdIndicator = true` in `setObdAttention()`)
- Consumed and cleared by each render function the next time it runs

**Layer 2 — Fine change-detection (file-scoped `s_*` statics)**
- ~47 statics spread across 7 render files (`display_arrow.cpp`, `display_bands.cpp`, etc.)
- Store last-drawn values + a `cacheValid` boolean (or equivalent)
- Render functions compare incoming values against cached values; skip if unchanged

**How they interact:**
1. `drawBaseFrame()` → `prepareFullRedrawNoClear()` → `dirty.setAll()` ← sets dirty flags
2. Next call to e.g. `drawBandIndicators()` → `if (dirty.bands) { s_bandCacheValid = false; dirty.bands = false; }` ← sees dirty flag, clears per-file cache
3. Render proceeds because `s_bandCacheValid` is false
4. Next call → `s_bandCacheValid = true`, `dirty.bands = false` → skip

**The inefficiency:** Cache invalidation is deferred one render cycle. More importantly,
the two layers are just **redundant mental overhead** once you know both exist — you have to
understand both to reason about any redraw bug.

---

## The Goal (after this plan executes)

- One layer: per-element cache structs with explicit `valid` flags
- `prepareFullRedrawNoClear()` **directly** zeros all `valid` flags — no deferred ping-pong
- Render functions check `valid` only — no `if (dirty.x)` blocks
- `DisplayDirtyFlags` retains only the flags that serve purposes beyond element-cache invalidation
- CI script updated to match new contract

---

## What Does NOT Change

| Item | Why it stays |
|---|---|
| `dirty.multiAlert` | Layout mode flag, not element cache. Set by mode transitions.  |
| `dirty.resetTracking` | Signals `DisplayRenderCache::reset*()` in `display_update.cpp`. Not cache. |
| `dirty.obdIndicator` | Read in `updateStatusStripIncremental()` to decide strip flush. Needed externally. |
| `dirty.cards` | Set explicitly in `display_update.cpp:901` after `drawBaseFrame()`. Needed externally. |
| `DisplayRenderCache s_displayRenderCache` | Already structured correctly. No change. |
| `s_arrowBlinkOn`, `s_arrowLastBlinkTime` | Blink timer state. Not render cache. |
| `s_bandBlinkOn`, `s_bandLastBlinkTime` | Blink timer state. Not render cache. |
| `s_bandBaselineInit`, `s_bandBaselineAdjust` | Font measurement, inside function body. Not file-scope. |
| `s_batteryShowOnUSB` | Voltage hysteresis state. Not render cache. |
| `s_cardsSlots[2]` | Active slot state (contains `lastSeen` timer). Not pure last-drawn. |
| `s_freqClassicWidthCache[16]` | LRU font computation cache (`TextWidthCacheEntry[]`). Different type. |
| `s_freqClassicWidthCacheNextSlot` | LRU slot counter. Not render state. |
| `s_freqSerpentineWidthCache[16]` | LRU font computation cache. Different type. |
| `s_freqSerpentineWidthCacheNextSlot` | LRU slot counter. Not render state. |

---

## Pre-Step (Standalone Micro-Commit): Fix Stray Extern

**Problem:** `display_indicators.cpp` line 15 has `extern ObdRuntimeModule obdRuntimeModule;`
— a global extern that was missed during the WiFi extern cleanup (commit `2a0c3eda`).
It is used in `syncTopIndicators()` to call `obdRuntimeModule.snapshot(nowMs)`.

This should be fixed before starting the cache unification so the two changes don't
entangle with each other. It is a smaller and lower-risk change on its own.

### Pre-Step A: Add private member to `V1Display`

**File: `src/display.h`** — in the `private:` section, near the end of member vars (~line 227):

```cpp
// Add this line after obdAttention_ = false; :
ObdRuntimeModule* obdRtMod_ = nullptr;     // Injected in begin(); used by syncTopIndicators
```

### Pre-Step B: Add setter declaration to `V1Display`

**File: `src/display.h`** — in the `public:` section, near `refreshObdIndicator`:

```cpp
// Add this line:
void setObdRuntimeModule(ObdRuntimeModule* m);
```

### Pre-Step C: Implement setter

**File: `src/display.cpp`** (or wherever other V1Display method bodies not in extracted files live) — add:

```cpp
void V1Display::setObdRuntimeModule(ObdRuntimeModule* m) {
    obdRtMod_ = m;
}
```

### Pre-Step D: Change `syncTopIndicators()` to use the member pointer

**File: `src/display_indicators.cpp`**

Remove line 15:
```cpp
extern ObdRuntimeModule obdRuntimeModule;   // DELETE THIS LINE
```

Change `syncTopIndicators()` body (currently lines 77–83):

```cpp
// BEFORE:
void V1Display::syncTopIndicators(uint32_t nowMs) {
    const ObdRuntimeStatus obdStatus = obdRuntimeModule.snapshot(nowMs);
    setObdStatus(obdStatus.enabled,
                 obdStatus.connected,
                 obdStatus.scanInProgress || obdStatus.manualScanPending);
}

// AFTER:
void V1Display::syncTopIndicators(uint32_t nowMs) {
    if (!obdRtMod_) return;
    const ObdRuntimeStatus obdStatus = obdRtMod_->snapshot(nowMs);
    setObdStatus(obdStatus.enabled,
                 obdStatus.connected,
                 obdStatus.scanInProgress || obdStatus.manualScanPending);
}
```

### Pre-Step E: Wire it in `main.cpp`

**File: `src/main.cpp`** — after `display.begin()` (currently ~line 937):

```cpp
// After: if (!display.begin()) { ... }
display.setObdRuntimeModule(&obdRuntimeModule);
```

### Pre-Step F: Update extern usage contract baseline

Run `scripts/ci-test.sh` to confirm the contract script passes. Update the baseline if
prompted:

```bash
python3 scripts/check_extern_usage_contract.py --update
scripts/ci-test.sh
```

### Pre-Step Verification
- `pio test -e native` → passes
- `scripts/ci-test.sh` → passes
- Commit: `"fix: remove stray extern ObdRuntimeModule from display_indicators.cpp"`

---

## Phase 1: Create `include/display_element_caches.h`

Create a new header that declares per-element cache structs and a single aggregate
instance `g_elementCaches` exported for all display files.

**New file: `include/display_element_caches.h`**

```cpp
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
        cards.invalidate();
    }
};

// Single shared instance — defined in display.cpp; included by display sub-modules.
extern DisplayElementCaches g_elementCaches;
```

### Phase 1: Define the global instance

**File: `src/display.cpp`** — add near the top, after includes:

```cpp
#include "../include/display_element_caches.h"
DisplayElementCaches g_elementCaches;
```

### Phase 1 Verification
- `pio test -e native` → passes (no logic changed yet)

---

## Phase 2: Wire `invalidateAll()` into `prepareFullRedrawNoClear()`

This is the key wiring step. After this step, every screen clear automatically
zeros all render caches **without** waiting for the render function to run.

**File: `src/display_indicators.cpp`** — `prepareFullRedrawNoClear()` (currently ~lines 28–32):

```cpp
// BEFORE:
void V1Display::prepareFullRedrawNoClear() {
    bleProxyDrawn_ = false;
    dirty.setAll();         // Invalidate every element cache after screen clear
    drawBLEProxyIndicator();  // Redraw BLE icon after screen clear
}

// AFTER:
void V1Display::prepareFullRedrawNoClear() {
    bleProxyDrawn_ = false;
    dirty.setAll();             // Retains multiAlert, obdIndicator, cards, resetTracking
    g_elementCaches.invalidateAll();  // Directly zeros all per-element render caches
    drawBLEProxyIndicator();
}
```

Add the include at the top of `display_indicators.cpp`:
```cpp
#include "../include/display_element_caches.h"
```

Also, for the `dirty.cards = true` call in **`src/display_update.cpp` line 901** (inside
`update(AlertData, ...)`), add a companion cache invalidation:

```cpp
// BEFORE (line 901):
dirty.cards = true;

// AFTER:
dirty.cards = true;
g_elementCaches.cards.invalidate();
```

Add the include at the top of `display_update.cpp`:
```cpp
#include "../include/display_element_caches.h"
```

And for `setObdAttention()` in **`src/display_indicators.cpp`** (~line 60):

```cpp
// BEFORE:
void V1Display::setObdAttention(bool attention) {
    if (obdAttention_ == attention) { return; }
    obdAttention_ = attention;
    dirty.obdIndicator = true;
}

// AFTER:
void V1Display::setObdAttention(bool attention) {
    if (obdAttention_ == attention) { return; }
    obdAttention_ = attention;
    dirty.obdIndicator = true;
    g_elementCaches.obd.invalidate();   // Direct cache invalidation at the source
}
```

### Phase 2 Verification
- `pio test -e native` → passes (caches now get doubly-invalidated, harmless)
- Both old `if (dirty.x)` blocks AND new `g_elementCaches.*` are active simultaneously
- This is safe — a double-invalidation is not harmful, it just means the first render
  after a screen clear still does a full draw (which it would have done anyway)

---

## Phase 3: Convert Render Files (One File Per Commit)

For each file: add the include, swap the statics for struct fields, remove the
`if (dirty.x) { ... }` block, update the cache-update writes at end of function.
Run `pio test -e native` after each file.

### Phase 3a — `display_arrow.cpp`

**Add include** at top of file:
```cpp
#include "../include/display_element_caches.h"
```

**Remove** the 9 file-scoped render cache statics (lines 20–28):
```cpp
// DELETE these 9 lines:
static bool s_arrowLastShowFront = false;
static bool s_arrowLastShowSide = false;
static bool s_arrowLastShowRear = false;
static bool s_arrowLastMuted = false;
static uint16_t s_arrowLastFrontCol = 0;
static uint16_t s_arrowLastSideCol = 0;
static uint16_t s_arrowLastRearCol = 0;
static bool s_arrowLastRaisedLayout = true;
static bool s_arrowCacheValid = false;
// KEEP the 2 blink statics below them:
// static unsigned long s_arrowLastBlinkTime = 0;
// static bool s_arrowBlinkOn = true;
```

**In `drawDirectionArrow()`, replace lines 36–39** (the `if (dirty.arrow)` block):

```cpp
// BEFORE:
const bool forceFullRedraw = dirty.arrow;
if (forceFullRedraw) {
    s_arrowCacheValid = false;
    dirty.arrow = false;
}

// AFTER:
const bool forceFullRedraw = !g_elementCaches.arrow.valid;
```

Update all uses of `s_arrow*` statics to use `g_elementCaches.arrow.*`:

| Old name | New name |
|---|---|
| `s_arrowCacheValid` | `g_elementCaches.arrow.valid` |
| `s_arrowLastShowFront` | `g_elementCaches.arrow.showFront` |
| `s_arrowLastShowSide` | `g_elementCaches.arrow.showSide` |
| `s_arrowLastShowRear` | `g_elementCaches.arrow.showRear` |
| `s_arrowLastMuted` | `g_elementCaches.arrow.muted` |
| `s_arrowLastFrontCol` | `g_elementCaches.arrow.frontCol` |
| `s_arrowLastSideCol` | `g_elementCaches.arrow.sideCol` |
| `s_arrowLastRearCol` | `g_elementCaches.arrow.rearCol` |
| `s_arrowLastRaisedLayout` | `g_elementCaches.arrow.raisedLayout` |

After the final draw at the bottom of the function, find where `s_arrowCacheValid = true`
is written (currently it's `s_arrowCacheValid = true` — find it and replace with
`g_elementCaches.arrow.valid = true`).

> **Note:** `dirty.arrow` is no longer read in this file after this change. Do not
> remove it from `DisplayDirtyFlags` yet — that happens in Phase 4.

✓ **Verify:** `pio test -e native`, `scripts/ci-test.sh`
The CI dirty-flag discipline script will now see `dirty.arrow` as a set-only flag
(only in `dirty.setAll()`), which it will flag as a warning or fail on. See
Phase 5 for the script update — you may need to update the script now to allow this.

---

### Phase 3b — `display_bands.cpp`

**Add include** at top.

**Remove** 3 file-scoped band render cache statics (lines 24–26):
```cpp
// DELETE these 3:
static uint8_t s_bandLastEffectiveMask = 0xFF;
static bool s_bandLastMuted = false;
static bool s_bandCacheValid = false;
// KEEP blink statics:
// static unsigned long s_bandLastBlinkTime = 0;
// static bool s_bandBlinkOn = true;
```

**Remove** 3 file-scoped signal bars render cache statics (lines ~218–220):
```cpp
// DELETE these 3:
static uint8_t s_barsLastStrength = 0xFF;
static bool s_barsLastMuted = false;
static bool s_barsCacheValid = false;
```

**In `drawBandIndicators()`, replace lines 49–51** (the `if (dirty.bands)` block):
```cpp
// BEFORE:
if (dirty.bands) {
    s_bandCacheValid = false;
    dirty.bands = false;
}
// AFTER: (remove entire block — g_elementCaches.bands.valid is already false)
```

**In `drawVerticalSignalBars()`, replace the `if (dirty.signalBars)` block** (lines ~147–149):
```cpp
// BEFORE:
if (dirty.signalBars) {
    s_barsCacheValid = false;
    dirty.signalBars = false;
}
// AFTER: (remove entire block)
```

Update all `s_band*` and `s_bars*` statics to use `g_elementCaches.bands.*` and
`g_elementCaches.bars.*`:

| Old name | New name |
|---|---|
| `s_bandCacheValid` | `g_elementCaches.bands.valid` |
| `s_bandLastEffectiveMask` | `g_elementCaches.bands.lastMask` |
| `s_bandLastMuted` | `g_elementCaches.bands.lastMuted` |
| `s_barsCacheValid` | `g_elementCaches.bars.valid` |
| `s_barsLastStrength` | `g_elementCaches.bars.lastStrength` |
| `s_barsLastMuted` | `g_elementCaches.bars.lastMuted` |

> **Important:** `s_bandBaselineInit` and `s_bandBaselineAdjust` are inside the function
> body of `drawBandIndicators()`, NOT file-scope. They are font measurement only and
> must NOT be changed.

✓ **Verify:** `pio test -e native`, `scripts/ci-test.sh`

---

### Phase 3c — `display_status_bar.cpp`

**Add include** at top.

**Remove** 4 render cache statics (lines 29–32):
```cpp
// DELETE these 4:
static int s_batteryLastPctDrawn = -1;
static bool s_batteryLastPctVisible = false;
static uint16_t s_batteryLastPctColor = 0;
static unsigned long s_batteryLastPctDrawMs = 0;
// KEEP (hysteresis state, not render cache):
// static bool s_batteryShowOnUSB = true;
```

**In `drawBatteryIndicator()` or `drawBatteryPercentage()`, replace the
`if (dirty.battery)` block** (lines ~285–296):

The original pattern was:
```cpp
bool needsRedraw = dirty.battery ||  // Screen was cleared
    (pctVisible != s_batteryLastPctVisible) ||
    (pctVisible && (pct != s_batteryLastPctDrawn || color != s_batteryLastPctColor));
if (needsRedraw) {
    dirty.battery = false;
    ...
}
```

After the change, `dirty.battery` is not read — instead `g_elementCaches.battery`
acts as the invalidation sentinel:

```cpp
// AFTER:
bool needsRedraw = !g_elementCaches.battery.lastPctVisible  // covers both first-run (-1) and post-invalidate
                   ? true   // simplified: use the existing sentinel logic:
                   : ...;
// Actually: BatteryRenderCache.invalidate() already sets lastPctDrawn = -1.
// The existing redraw check (pct != s_batteryLastPctDrawn) handles this correctly
// because -1 != any real percentage. No structural change needed to the condition —
// just substitute s_battery* for g_elementCaches.battery.*.
```

Update all `s_battery*` statics to use `g_elementCaches.battery.*`:

| Old name | New name |
|---|---|
| `s_batteryLastPctDrawn` | `g_elementCaches.battery.lastPctDrawn` |
| `s_batteryLastPctVisible` | `g_elementCaches.battery.lastPctVisible` |
| `s_batteryLastPctColor` | `g_elementCaches.battery.lastPctColor` |
| `s_batteryLastPctDrawMs` | `g_elementCaches.battery.lastPctDrawMs` |

Remove the `dirty.battery = false` line inside the redraw block (it's no longer needed).

✓ **Verify:** `pio test -e native`, `scripts/ci-test.sh`

---

### Phase 3d — `display_indicators.cpp`

> Note: The stray extern fix from Pre-Step should already be merged before this phase.

**Add include** at top (if not already present from Phase 2):
```cpp
#include "../include/display_element_caches.h"
```

**Remove** the 3 render cache statics (lines 42–44):
```cpp
// DELETE these 3:
static bool s_obdLastShown = false;
static bool s_obdLastConnected = false;
static bool s_obdLastAttention = false;
```

**In `drawObdIndicator()`, update the early-return guard** (currently ~lines 86–92):
```cpp
// BEFORE:
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

// AFTER:
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
```

> **Note:** `dirty.obdIndicator` is KEPT because it is read in
> `updateStatusStripIncremental()` (`display_update.cpp`) to determine whether to set
> `flushCenterStrip = true`. The flag is still cleared here but it now serves only as a
> "needs flush" signal, not as a cache invalidation trigger.

✓ **Verify:** `pio test -e native`, `scripts/ci-test.sh`

---

### Phase 3e — `display_top_counter.cpp`

**Add include** at top.

**Remove** 5 file-scoped render cache statics (lines 36–40):
```cpp
// DELETE these 5:
static char s_topCounterLastSymbol = '\0';
static bool s_topCounterLastMuted = false;
static bool s_topCounterLastShowDot = false;
static uint16_t s_topCounterLastBogeyColor = 0;
static bool s_topCounterLastMutedState = false;
```

**In `drawTopCounterClassic()`, replace the early-return guard** (currently ~lines 271–280):
```cpp
// BEFORE:
bool colorChanged = (s.colorBogey != s_topCounterLastBogeyColor);
if (!dirty.topCounter && !colorChanged &&
    symbol == s_topCounterLastSymbol && muted == s_topCounterLastMuted && showDot == s_topCounterLastShowDot) {
    return;
}
dirty.topCounter = false;
s_topCounterLastSymbol = symbol;
s_topCounterLastMuted = muted;
s_topCounterLastShowDot = showDot;
s_topCounterLastBogeyColor = s.colorBogey;

// AFTER:
bool colorChanged = (s.colorBogey != g_elementCaches.topCounter.lastBogeyColor);
if (g_elementCaches.topCounter.counterValid && !colorChanged &&
    symbol == g_elementCaches.topCounter.lastSymbol &&
    muted == g_elementCaches.topCounter.lastMuted &&
    showDot == g_elementCaches.topCounter.lastShowDot) {
    return;
}
g_elementCaches.topCounter.counterValid   = true;
g_elementCaches.topCounter.lastSymbol     = symbol;
g_elementCaches.topCounter.lastMuted      = muted;
g_elementCaches.topCounter.lastShowDot    = showDot;
g_elementCaches.topCounter.lastBogeyColor = s.colorBogey;
```

**In `drawMuteIcon()`, replace the early-return guard** (currently ~lines 387–390):
```cpp
// BEFORE:
if (!dirty.muteIcon && muted == s_topCounterLastMutedState) {
    return;
}
dirty.muteIcon = false;
s_topCounterLastMutedState = muted;

// AFTER:
if (g_elementCaches.topCounter.muteIconValid &&
    muted == g_elementCaches.topCounter.lastMutedState) {
    return;
}
g_elementCaches.topCounter.muteIconValid = true;
g_elementCaches.topCounter.lastMutedState = muted;
```

✓ **Verify:** `pio test -e native`, `scripts/ci-test.sh`

---

### Phase 3f — `display_frequency.cpp`

> This file is the most complex. Two independent functions both consume `dirty.frequency`.
> The Serpentine path falls through to Classic when OFR font isn't ready — but
> `dirty.frequency` is consumed (cleared) in whichever function gets called first.
> After unification, each function checks its own cache struct instead.

**Add include** at top.

**Remove** 6 classic frequency render cache statics:
```cpp
// DELETE these 6 (lines ~33–41):
static char s_freqClassicLastText[16] = "";
static uint16_t s_freqClassicLastColor = 0;
static bool s_freqClassicLastUsedOfr = false;
static bool s_freqClassicCacheValid = false;
static int s_freqClassicLastDrawX = 0;
static int s_freqClassicLastDrawWidth = 0;
// KEEP these (LRU font computation caches, not render state):
// static TextWidthCacheEntry s_freqClassicWidthCache[16];
// static uint8_t s_freqClassicWidthCacheNextSlot = 0;
// static int s_freqClassicCachedNumericWidth = 0;
// static int s_freqClassicCachedDashWidth = 0;
// static int s_freqClassicCachedLaserWidth = 0;
```

**Remove** 6 serpentine frequency render cache statics:
```cpp
// DELETE these 6 (lines ~43–50):
static char s_freqSerpentineLastText[16] = "";
static uint16_t s_freqSerpentineLastColor = 0;
static bool s_freqSerpentineCacheValid = false;
static unsigned long s_freqSerpentineLastDrawMs = 0;
static int s_freqSerpentineLastDrawX = 0;
static int s_freqSerpentineLastDrawWidth = 0;
// KEEP these:
// static TextWidthCacheEntry s_freqSerpentineWidthCache[16];
// static uint8_t s_freqSerpentineWidthCacheNextSlot = 0;
```

**In `drawFrequencyClassic()`, replace lines 107–109** (the `if (dirty.frequency)` block):
```cpp
// BEFORE:
if (dirty.frequency) {
    s_freqClassicCacheValid = false;
    dirty.frequency = false;
}

// AFTER: (remove entire block — g_elementCaches.freqClassic.valid handles this)
```

Update the `changed` condition in `drawFrequencyClassic()` (currently ~lines ~166–169):
```cpp
// BEFORE:
bool changed = !s_freqClassicCacheValid ||
               (s_freqClassicLastUsedOfr != usingOfr) ||
               textChanged ||
               (s_freqClassicLastColor != freqColor);

// AFTER:
bool changed = !g_elementCaches.freqClassic.valid ||
               (g_elementCaches.freqClassic.lastUsedOfr != usingOfr) ||
               textChanged ||
               (g_elementCaches.freqClassic.lastColor != freqColor);
```

Update all `s_freqClassic*` statics to `g_elementCaches.freqClassic.*`:

| Old name | New name |
|---|---|
| `s_freqClassicCacheValid` | `g_elementCaches.freqClassic.valid` |
| `s_freqClassicLastText` | `g_elementCaches.freqClassic.lastText` |
| `s_freqClassicLastColor` | `g_elementCaches.freqClassic.lastColor` |
| `s_freqClassicLastUsedOfr` | `g_elementCaches.freqClassic.lastUsedOfr` |
| `s_freqClassicLastDrawX` | `g_elementCaches.freqClassic.lastDrawX` |
| `s_freqClassicLastDrawWidth` | `g_elementCaches.freqClassic.lastDrawWidth` |

**In `drawFrequencySerpentine()`, replace lines 297–299** (the `if (dirty.frequency)` block):
```cpp
// BEFORE:
if (dirty.frequency) {
    s_freqSerpentineCacheValid = false;
    dirty.frequency = false;  // Clear flag - we're handling it
}

// AFTER: (remove entire block)
```

Update all `s_freqSerpentine*` statics to `g_elementCaches.freqSerpentine.*`:

| Old name | New name |
|---|---|
| `s_freqSerpentineCacheValid` | `g_elementCaches.freqSerpentine.valid` |
| `s_freqSerpentineLastText` | `g_elementCaches.freqSerpentine.lastText` |
| `s_freqSerpentineLastColor` | `g_elementCaches.freqSerpentine.lastColor` |
| `s_freqSerpentineLastDrawMs` | `g_elementCaches.freqSerpentine.lastDrawMs` |
| `s_freqSerpentineLastDrawX` | `g_elementCaches.freqSerpentine.lastDrawX` |
| `s_freqSerpentineLastDrawWidth` | `g_elementCaches.freqSerpentine.lastDrawWidth` |

**Special case — Serpentine fallthrough path:**
`drawFrequencySerpentine()` calls `drawFrequencyClassic()` internally when OFR isn't ready.
After this change, the two functions use separate caches, so one clearing the other is no
longer an issue. This is actually cleaner behavior.

✓ **Verify:** `pio test -e native`, `scripts/ci-test.sh`

---

### Phase 3g — `display_cards.cpp`

> Most complex: `s_cardsSlots[2]` and `s_cardsLastDrawnPositions[2]` are NOT moved.
> Only `s_cardsLastPriority`, `s_cardsLastDrawnCount`, `s_cardsLastProfileSlot` move.

**Add include** at top.

**Remove** 3 cache statics (note: `s_cardsLastDrawnPositions` is the anonymous struct
array — it stays because it is keyed per card slot position):
```cpp
// TODO: Confirm exact lines when executing. Based on Phase 1 inventory:
// DELETE:
static AlertData s_cardsLastPriority;
static int s_cardsLastDrawnCount = 0;
static int s_cardsLastProfileSlot = -1;
// KEEP:
// static CardSlot s_cardsSlots[2];             ← active slot state, NOT render cache
// static <anon-struct> s_cardsLastDrawnPositions[2]; ← per-position data, stays
```

**In `drawSecondaryAlertCards()`, replace the `dirty.cards` capture block** (~lines 266–268):
```cpp
// BEFORE:
bool doForceRedraw = dirty.cards;
dirty.cards = false;  // Reset the force flag

// AFTER:
bool doForceRedraw = g_elementCaches.cards.forceRedraw;
dirty.cards = false;                         // Still cleared — read externally for flush decisions
g_elementCaches.cards.forceRedraw = false;   // Consumed here
```

Update any uses of `s_cardsLastPriority`, `s_cardsLastDrawnCount`, `s_cardsLastProfileSlot`
to `g_elementCaches.cards.lastPriority`, `.lastDrawnCount`, `.lastProfileSlot`.

> **Note:** `dirty.cards` is KEPT because it is set externally in `display_update.cpp:901`
> and read in the same file to communicate "force-redraw-this-frame." After unification,
> `dirty.cards = true` is accompanied by `g_elementCaches.cards.invalidate()` (added in
> Phase 2). Both signals point at the same intent.
> Long-term: once it's confirmed `dirty.cards` is fully replaced by
> `g_elementCaches.cards.forceRedraw`, the `dirty.cards` flag can be removed.

✓ **Verify:** `pio test -e native`, `scripts/ci-test.sh`

---

## Phase 4: Remove Retired Dirty Flags from `DisplayDirtyFlags`

After all per-file conversions are complete, the following flags are no longer read
anywhere in the render functions. Remove them from `DisplayDirtyFlags`.

**File: `include/display_dirty_flags.h`**

Remove these fields from the struct:
```cpp
// DELETE:
bool frequency      = false;
bool battery        = false;
bool bands          = false;
bool signalBars     = false;
bool arrow          = false;
bool muteIcon       = false;
bool topCounter     = false;
```

Remove them from `setAll()`:
```cpp
// BEFORE setAll():
void setAll() {
    frequency    = true;
    battery      = true;
    bands        = true;
    signalBars   = true;
    arrow        = true;
    muteIcon     = true;
    topCounter   = true;
    obdIndicator = true;
}

// AFTER setAll():
void setAll() {
    obdIndicator = true;   // Still read externally for flush routing
    // All other element caches are now invalidated via g_elementCaches.invalidateAll()
    // which is called from prepareFullRedrawNoClear() alongside this function.
}
```

**Remaining fields in `DisplayDirtyFlags` after Phase 4:**
```cpp
struct DisplayDirtyFlags {
    bool multiAlert    = false;  // Layout mode flag (not element cache)
    bool cards         = false;  // Force-redraw signal set from display_update.cpp:901
    bool obdIndicator  = false;  // Read externally in updateStatusStripIncremental for flush
    bool resetTracking = false;  // Signals DisplayRenderCache state reset
};
```

### Phase 4: Update `dirty.setAll()` wording in ARCHITECTURE.md

In `docs/ARCHITECTURE.md`, update the description of the dirty flag system:
- Remove references to `dirty.frequency`, `dirty.bands`, etc.
- Note that element-level invalidation now happens via `g_elementCaches.invalidateAll()`

✓ **Verify:** `pio test -e native`, `scripts/ci-test.sh`

---

## Phase 5: Update CI Script

**File: `scripts/check_dirty_flag_discipline.py`**

The script currently enforces: "every `dirty.x` read in a function must also have
`dirty.x = false` in the same function." After Phase 4, the remaining dirty fields
that are read in render functions are: `dirty.obdIndicator` (cleared in `drawObdIndicator`)
and `dirty.cards` (cleared in `drawSecondaryAlertCards`).

The script should be updated to reflect the reduced set of tracked fields:

1. Update `EXEMPT_FIELDS` if needed to exclude the removed fields
2. Or run `python3 scripts/check_dirty_flag_discipline.py --update` if the script supports it
3. Check the script output to confirm it only tracks `obdIndicator` and `cards` now
4. If any false-positives remain (flagging `dirty.multiAlert`), add it to `EXEMPT_FIELDS`

✓ **Final verification:** `scripts/ci-test.sh` — full suite green

---

## Phase 6: Update `docs/ARCHITECTURE.md`

**Section: "Display Cache Architecture"** — update wording:

- Before: "Two-layer cache system: coarse dirty flags + fine per-file statics"
- After: "Single-layer element caches: per-element structs in `display_element_caches.h`,
  invalidated directly from `prepareFullRedrawNoClear()`. Residual dirty flags
  serve layout mode (`multiAlert`) and external flush coordination (`obdIndicator`, `cards`,
  `resetTracking`) — not element cache invalidation."

---

## Commit Sequence

| # | Description | Branch Point |
|---|---|---|
| Pre | `fix: remove stray extern ObdRuntimeModule from display_indicators` | Before Phase 1 |
| 1 | `refactor: add display_element_caches.h and define g_elementCaches` | After pre |
| 2 | `refactor: wire invalidateAll into prepareFullRedrawNoClear` | After Phase 1 |
| 3a | `refactor: convert display_arrow.cpp to g_elementCaches` | After Phase 2 |
| 3b | `refactor: convert display_bands.cpp to g_elementCaches` | After 3a |
| 3c | `refactor: convert display_status_bar.cpp to g_elementCaches` | After 3b |
| 3d | `refactor: convert display_indicators.cpp to g_elementCaches` | After 3c |
| 3e | `refactor: convert display_top_counter.cpp to g_elementCaches` | After 3d |
| 3f | `refactor: convert display_frequency.cpp to g_elementCaches` | After 3e |
| 3g | `refactor: convert display_cards.cpp to g_elementCaches` | After 3f |
| 4 | `refactor: remove retired dirty flags from DisplayDirtyFlags` | After 3g |
| 5 | `fix: update dirty_flag_discipline CI script for new model` | After Phase 4 |
| 6 | `docs: update ARCHITECTURE.md display cache section` | After Phase 5 |

---

## Risk Register

| Risk | Mitigation |
|---|---|
| Double-invalidation race (Phase 2: both old dirty.x and new cache are active but one might not trigger) | Phase 2 produces only double-invalidation, never single-miss. Safe to ship. |
| `forceFullRedraw` in `display_arrow.cpp` changes semantics | Replace `const bool forceFullRedraw = dirty.arrow` with `const bool forceFullRedraw = !g_elementCaches.arrow.valid`. Same semantics — both are true when cache was just zeroed. |
| `drawFrequencySerpentine` fallthrough to `drawFrequencyClassic` — previously one `dirty.frequency = false` served both | After change: each function has its own cache. If Serpentine falls through to Classic, Serpentine's cache is still invalid (fine — Serpentine didn't draw). Classic's cache gets set after Classic draws. Next call: Serpentine falls through again but Classic cache is valid → quick return. Correct. |
| `display_top_counter.cpp` has no `cacheValid` sentinel (uses direct value comparison) | Phase 3e adds `counterValid` and `muteIconValid` booleans to `TopCounterRenderCache`. Default `false` → first call always draws. Safe. |
| CI script fails on Phase 3 partial state (flags removed from render functions before DisplayDirtyFlags is cleaned) | Phase 5 (script update) can be done at Phase 3 start. The script checks reads without clears — if we remove the reads, no violation. The script won't complain. |
| Forgetting to update the `--update` snapshot contracts | Always run `scripts/ci-test.sh` at each step, not just `pio test -e native`. |

---

## Quick-Reference: Static Inventory Before/After

### `display_arrow.cpp`

| Before | After |
|---|---|
| `s_arrowCacheValid` | `g_elementCaches.arrow.valid` |
| `s_arrowLastShowFront/Side/Rear` | `g_elementCaches.arrow.showFront/Side/Rear` |
| `s_arrowLastMuted` | `g_elementCaches.arrow.muted` |
| `s_arrowLastFrontCol/SideCol/RearCol` | `g_elementCaches.arrow.frontCol/sideCol/rearCol` |
| `s_arrowLastRaisedLayout` | `g_elementCaches.arrow.raisedLayout` |
| `s_arrowBlinkOn` | stays `s_arrowBlinkOn` (blink timer) |
| `s_arrowLastBlinkTime` | stays `s_arrowLastBlinkTime` (blink timer) |

### `display_bands.cpp`

| Before | After |
|---|---|
| `s_bandCacheValid` | `g_elementCaches.bands.valid` |
| `s_bandLastEffectiveMask` | `g_elementCaches.bands.lastMask` |
| `s_bandLastMuted` | `g_elementCaches.bands.lastMuted` |
| `s_barsCacheValid` | `g_elementCaches.bars.valid` |
| `s_barsLastStrength` | `g_elementCaches.bars.lastStrength` |
| `s_barsLastMuted` | `g_elementCaches.bars.lastMuted` |
| `s_bandBlinkOn` / `s_bandLastBlinkTime` | stays (blink timer) |
| `s_bandBaselineInit` / `s_bandBaselineAdjust` | stays (in-function font metric) |

### `display_status_bar.cpp`

| Before | After |
|---|---|
| `s_batteryLastPctDrawn` | `g_elementCaches.battery.lastPctDrawn` |
| `s_batteryLastPctVisible` | `g_elementCaches.battery.lastPctVisible` |
| `s_batteryLastPctColor` | `g_elementCaches.battery.lastPctColor` |
| `s_batteryLastPctDrawMs` | `g_elementCaches.battery.lastPctDrawMs` |
| `s_batteryShowOnUSB` | stays (hysteresis state) |

### `display_indicators.cpp`

| Before | After |
|---|---|
| `s_obdLastShown` | `g_elementCaches.obd.lastShown` |
| `s_obdLastConnected` | `g_elementCaches.obd.lastConnected` |
| `s_obdLastAttention` | `g_elementCaches.obd.lastAttention` |
| `extern ObdRuntimeModule` | removed (Pre-Step) → `obdRtMod_` member pointer |

### `display_top_counter.cpp`

| Before | After |
|---|---|
| `s_topCounterLastSymbol` | `g_elementCaches.topCounter.lastSymbol` |
| `s_topCounterLastMuted` | `g_elementCaches.topCounter.lastMuted` |
| `s_topCounterLastShowDot` | `g_elementCaches.topCounter.lastShowDot` |
| `s_topCounterLastBogeyColor` | `g_elementCaches.topCounter.lastBogeyColor` |
| `s_topCounterLastMutedState` | `g_elementCaches.topCounter.lastMutedState` |
| *(no counterValid)*  | `g_elementCaches.topCounter.counterValid` (new) |
| *(no muteIconValid)* | `g_elementCaches.topCounter.muteIconValid` (new) |

### `display_frequency.cpp`

| Before | After |
|---|---|
| `s_freqClassicCacheValid` | `g_elementCaches.freqClassic.valid` |
| `s_freqClassicLastText` | `g_elementCaches.freqClassic.lastText` |
| `s_freqClassicLastColor` | `g_elementCaches.freqClassic.lastColor` |
| `s_freqClassicLastUsedOfr` | `g_elementCaches.freqClassic.lastUsedOfr` |
| `s_freqClassicLastDrawX/Width` | `g_elementCaches.freqClassic.lastDrawX/Width` |
| `s_freqSerpentineCacheValid` | `g_elementCaches.freqSerpentine.valid` |
| `s_freqSerpentineLastText` | `g_elementCaches.freqSerpentine.lastText` |
| `s_freqSerpentineLastColor` | `g_elementCaches.freqSerpentine.lastColor` |
| `s_freqSerpentineLastDrawMs/X/Width` | `g_elementCaches.freqSerpentine.lastDrawMs/X/Width` |
| `s_freqClassicWidthCache[16]` | stays (LRU computation cache) |
| `s_freqClassicWidthCacheNextSlot` | stays |
| `s_freqClassicCachedNumeric/Dash/LaserWidth` | stays |
| `s_freqSerpentineWidthCache[16]` | stays (LRU computation cache) |
| `s_freqSerpentineWidthCacheNextSlot` | stays |

### `display_cards.cpp`

| Before | After |
|---|---|
| `s_cardsLastPriority` | `g_elementCaches.cards.lastPriority` |
| `s_cardsLastDrawnCount` | `g_elementCaches.cards.lastDrawnCount` |
| `s_cardsLastProfileSlot` | `g_elementCaches.cards.lastProfileSlot` |
| `s_cardsSlots[2]` | stays (active slot state) |
| `s_cardsLastDrawnPositions[2]` | stays (per-position per-frame data) |
| `dirty.cards` → `doForceRedraw` | `g_elementCaches.cards.forceRedraw` |

---

*Last updated: April 2026. Plan ready to execute.*

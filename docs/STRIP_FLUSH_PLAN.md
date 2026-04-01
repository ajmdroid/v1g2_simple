# Three-Strip Incremental flushRegion — Surgical Implementation Plan

> **Goal**: Replace `DISPLAY_FLUSH()` (full 640×172 = 110,080 px) with targeted
> `flushRegion()` calls on incremental update paths. Three vertical strips, each
> full height, each hitting the flushRegion fast path.

---

## Strip Definitions

All three strips use **full height** (y=0, h=172). This is critical — it means
`phys_pw = h = 172 = CANVAS_WIDTH`, which satisfies the fast-path condition
(`phys_pw == kRawStride && phys_px0 == 0`). Each strip flushes with **one**
`draw16bitRGBBitmap` call instead of row-by-row fallback.

```
LEFT:   (0,   0, 120, 172)   BAND_COLUMN_WIDTH
CENTER: (120, 0, 320, 172)   CONTENT_AVAILABLE_WIDTH
RIGHT:  (440, 0, 200, 172)   SIGNAL_COLUMN_WIDTH
```

Combined coverage: 0 + 120 + 320 + 200 = 640 = SCREEN_WIDTH. No gaps.

### Pixel Budget (SPI bytes per strip, RGB565)

| Strip  | Pixels     | Bytes  | vs Full Screen |
|--------|-----------|--------|----------------|
| Left   | 120×172   | 41,280 | 37%            |
| Center | 320×172   | 110,080| 100% (same)    |
| Right  | 200×172   | 68,800 | 63%            |
| Full   | 640×172   | 220,160| 100%           |

Best case (1 strip dirty): **37% of full flush** (left-only change like volume).
Typical case (2 strips dirty): **63–100%** but still avoids re-sending unchanged pixels.
Worst case (all 3 dirty): same total bytes as full flush, split across 3 SPI
transactions. Marginally more overhead from 3 `draw16bitRGBBitmap` calls, but
each still hits the contiguous fast path.

---

## Element-to-Strip Map

Every element that draws during incremental updates must map to exactly one strip.

### LEFT Strip (x: 0–119)

| Element             | Draw Position           | Called From                        |
|---------------------|------------------------|------------------------------------|
| Bogey counter       | x=16, y=6, 55×68       | `drawTopCounter()`                 |
| Band indicators     | x=82, y=55             | `drawBandIndicators()`             |
| Volume indicator    | x=8, y=75, 75×16       | `drawVolumeIndicator()`            |
| RSSI indicator      | x=8, y=99, 70×44       | `drawRssiIndicator()`              |
| WiFi icon           | x=8, y=145, 24×24      | `drawWiFiIndicator()`              |
| BLE icon            | x=40, y=145, 24×24     | `drawBLEProxyIndicator()`          |

### CENTER Strip (x: 120–439)

| Element             | Draw Position           | Called From                        |
|---------------------|------------------------|------------------------------------|
| Frequency display   | center zone, ~120–440  | `drawFrequency()`                  |
| OBD indicator       | x=370, y=5, 50×26      | `drawObdIndicator()`               |
| Secondary cards     | x=120–440, y=118       | `drawSecondaryAlertCards()`         |
| Mute icon           | center area             | `drawMuteIcon()`                   |

### RIGHT Strip (x: 440–639)

| Element             | Draw Position           | Called From                        |
|---------------------|------------------------|------------------------------------|
| Direction arrows    | cx=564                 | `drawDirectionArrow()`             |
| Signal bars         | right column            | `drawVerticalSignalBars()`         |
| Profile indicator   | x=564, y=152           | `drawProfileIndicator()`           |
| Battery icon        | x=618, y=136           | `drawBatteryIndicator()`           |

---

## Root Causes of Previous Failures (Reference)

These are the specific bugs this plan corrects:

1. **Volume/RSSI mapped to wrong strip**: `updateStatusStripIncremental()` line 187
   sets `flushRightStrip = true` when volume/RSSI change, but those elements draw
   at x=8 (LEFT). Pixels updated in canvas, never flushed to panel.

2. **OBD indicator uncovered**: `drawObdIndicator()` at x=370 (CENTER) is called at
   line 201 of `updateStatusStripIncremental()` but neither strip boolean is set.

3. **Strip height truncation**: Previous attempts used `kPrimaryFlushH = 115` which
   missed WiFi/BLE icons at y=145 and battery at y=136.

4. **`setBLEProxyStatus()` fires its own full flush**: Line 309 of `display.cpp`
   calls `flush()` → `DISPLAY_FLUSH()`, which conflicts with strip-based flushing
   on the incremental paths.

5. **`rightStripCleared` misnomer**: `drawStatusStrip()` line 158 clears
   `FILL_RECT(8, 75, 75, 68, PALETTE_BG)` (x=8, LEFT side) but the flag is
   named `rightStripCleared`.

---

## Implementation Steps

### Step 0: Add Strip Constants to `display_layout.h`

Add named constants so strip dimensions are never magic numbers.

```cpp
// In namespace DisplayLayout:

// === Flush Strip Regions ===
// Three full-height vertical strips covering the entire screen.
// All use h=SCREEN_HEIGHT so flushRegion() hits the fast path
// (phys_pw == CANVAS_WIDTH → single contiguous blit).
constexpr int STRIP_LEFT_X  = 0;
constexpr int STRIP_LEFT_W  = BAND_COLUMN_WIDTH;         // 120

constexpr int STRIP_CENTER_X = BAND_COLUMN_WIDTH;         // 120
constexpr int STRIP_CENTER_W = CONTENT_AVAILABLE_WIDTH;    // 320

constexpr int STRIP_RIGHT_X  = SCREEN_WIDTH - SIGNAL_COLUMN_WIDTH; // 440
constexpr int STRIP_RIGHT_W  = SIGNAL_COLUMN_WIDTH;        // 200

// Full height for all strips — required for flushRegion fast path
constexpr int STRIP_H = SCREEN_HEIGHT;                     // 172
constexpr int STRIP_Y = 0;
```

**Why first**: Every subsequent step references these constants. Defining them
first means no magic numbers anywhere and compile-time verification.

**Risk**: Zero. Adding constants has no behavioral effect.

---

### Step 1: Rename `rightStripCleared` → `leftVolRssiCleared`

In `DisplayRenderCache` (display_update.cpp line 42):

```cpp
// Before:
bool rightStripCleared = false;

// After:
bool leftVolRssiCleared = false;
```

Update the 3 references at lines 154, 157, 159 to use the new name.

**Why**: The existing name is actively misleading — it clears the area at x=8
which is in the LEFT strip. Fixing the name before changing flush logic prevents
confusion during later steps.

**Risk**: Rename only. No behavioral change.

---

### Step 2: Add `flushCenterStrip` to `updateStatusStripIncremental()`

Change the function signature to accept three strip booleans:

```cpp
void V1Display::updateStatusStripIncremental(const DisplayState& state,
                                             char topChar,
                                             bool topMuted,
                                             bool topDot,
                                             bool volumeChanged,
                                             bool rssiNeedsUpdate,
                                             bool bogeyCounterChanged,
                                             uint8_t& lastMainVol,
                                             uint8_t& lastMuteVol,
                                             uint8_t& lastBogeyByte,
                                             unsigned long now,
                                             bool& flushLeftStrip,
                                             bool& flushCenterStrip,   // NEW
                                             bool& flushRightStrip);
```

Update the declaration in `display.h` to match.

**Inside the function body**, fix the strip assignments:

```cpp
// Volume and RSSI draw at x=8 → LEFT strip (was incorrectly flushRightStrip)
if (volumeChanged && showVolumeAndRssi) {
    lastMainVol = state.mainVolume;
    lastMuteVol = state.muteVolume;
    drawVolumeIndicator(state.mainVolume, state.muteVolume);
    drawRssiIndicator(bleCtx_.v1Rssi);
    markRssiRefreshed(now);
    flushLeftStrip = true;   // FIX: was flushRightStrip
} else if (rssiNeedsUpdate && showVolumeAndRssi) {
    drawRssiIndicator(bleCtx_.v1Rssi);
    markRssiRefreshed(now);
    flushLeftStrip = true;   // FIX: was flushRightStrip
}

// Bogey counter at x=16 → LEFT strip (already correct)
if (bogeyCounterChanged) {
    lastBogeyByte = state.bogeyCounterByte;
    drawTopCounter(topChar, topMuted, topDot);
    flushLeftStrip = true;
}

// OBD indicator at x=370 → CENTER strip (was uncovered)
bool obdBefore = s_obdLastShown;
bool obdConnBefore = s_obdLastConnected;
bool obdAttnBefore = s_obdLastAttention;
drawObdIndicator();
if (s_obdLastShown != obdBefore ||
    s_obdLastConnected != obdConnBefore ||
    s_obdLastAttention != obdAttnBefore) {
    flushCenterStrip = true;  // NEW
}
```

**Why**: This is the core fix. Every element now maps to the correct strip.

**Risk**: Low — the strip booleans are not consumed yet (they're still `(void)`-
discarded at the call sites). This step changes assignments but not flush
behavior. Safe to test in isolation.

---

### Step 3: Update Both Call Sites to Pass `flushCenterStrip`

#### Resting incremental path (display_update.cpp ~line 288)

```cpp
// Before:
bool flushLeftStrip = false;
bool flushRightStrip = false;
// ...
updateStatusStripIncremental(state, ..., flushLeftStrip, flushRightStrip);
(void)flushLeftStrip;
(void)flushRightStrip;
DISPLAY_FLUSH();

// After:
bool flushLeftStrip = false;
bool flushCenterStrip = false;
bool flushRightStrip = false;
// ...
updateStatusStripIncremental(state, ..., flushLeftStrip, flushCenterStrip, flushRightStrip);
(void)flushLeftStrip;
(void)flushCenterStrip;
(void)flushRightStrip;
DISPLAY_FLUSH();
```

#### Live incremental path (display_update.cpp ~line 728)

Same pattern — add `flushCenterStrip` boolean, pass it through, `(void)` discard it.

Additionally, wire the live-only elements to the correct strip booleans:

```cpp
if (freqOnlyChanged) {
    cache.liveLastPriority.frequency = priority.frequency;
    // ... drawFrequency() ...
    flushCenterStrip = true;   // Frequency is in CENTER
}

if (arrowsChanged || ...) {
    drawDirectionArrow(...);
    flushRightStrip = true;    // Already correct
}
if (signalBarsChanged) {
    drawVerticalSignalBars(...);
    flushRightStrip = true;    // Already correct
}
if (bandsChanged || ...) {
    drawBandIndicators(...);
    flushLeftStrip = true;     // Already correct
}

// Cards are in CENTER
drawSecondaryAlertCards(...);
if (secondaryCardsRenderDirty_) {
    flushCenterStrip = true;   // NEW
}
```

**Why**: All strip booleans are now correctly set in both paths. The `(void)`
discards + `DISPLAY_FLUSH()` are still in place — **behavior is unchanged**. This
is the "wire everything up but keep the old flush" safety step.

**Risk**: None. `DISPLAY_FLUSH()` still fires. The booleans are computed but unused.
This is a checkpoint where you can verify the build still works and the display
looks identical to current behavior.

**TEST**: Build, flash, verify display looks exactly the same. Nothing should
change visually. If anything looks wrong, the bug is in step 2's strip
assignments, not in flush logic.

---

### Step 4: Replace `(void)` Discards with Conditional `flushRegion()` Calls

This is the activation step. Replace the discard+flush pattern at both call sites.

#### Helper function (add near top of display_update.cpp, inside the anonymous namespace):

```cpp
void flushDirtyStrips(V1Display* self, bool left, bool center, bool right) {
    using namespace DisplayLayout;
    if (left)   self->flushRegion(STRIP_LEFT_X,   STRIP_Y, STRIP_LEFT_W,   STRIP_H);
    if (center) self->flushRegion(STRIP_CENTER_X, STRIP_Y, STRIP_CENTER_W, STRIP_H);
    if (right)  self->flushRegion(STRIP_RIGHT_X,  STRIP_Y, STRIP_RIGHT_W,  STRIP_H);

    // Safety: if nothing was dirty (shouldn't happen — caller guards this),
    // don't leave stale canvas. But never do a surprise full flush.
    // The caller's early-return already handles the "nothing changed" case.
}
```

> **Note**: `flushRegion` is public on `V1Display`. If you prefer not to expose
> `self`, make `flushDirtyStrips` a member function instead. Either works.

#### Resting incremental path:

```cpp
// Replace:
(void)flushLeftStrip;
(void)flushCenterStrip;
(void)flushRightStrip;
DISPLAY_FLUSH();

// With:
flushDirtyStrips(this, flushLeftStrip, flushCenterStrip, flushRightStrip);
```

#### Live incremental path:

Same replacement.

**Why**: This is the switch-over. Incremental frames now flush only the strips
that actually changed.

**Risk**: Medium — this is where visual bugs will show if any element-to-strip
mapping is wrong. Mitigated by step 3's checkpoint.

**TEST**: Flash and test every incremental scenario:
- Volume change only → verify LEFT strip updates (counter/volume/RSSI visible)
- Arrow change only → verify RIGHT strip updates
- Frequency change only → verify CENTER strip updates
- OBD toggle → verify CENTER strip updates
- RSSI periodic refresh → verify LEFT strip updates
- Multiple simultaneous changes → verify all affected strips update
- Band blink/flash → verify LEFT strip updates

---

### Step 5: Fix `setBLEProxyStatus()` to Not Fire Full Flush

Currently (display.cpp line 278–311), `setBLEProxyStatus()` calls `drawBLEProxyIndicator()`
and `drawRssiIndicator()` then fires `flush()` → full `DISPLAY_FLUSH()`. This
conflicts with strip-based flushing.

**Change**: Replace the `flush()` call with a targeted left-strip flush.

```cpp
void V1Display::setBLEProxyStatus(bool proxyEnabled, bool clientConnected, bool receivingData) {
#if defined(DISPLAY_WAVESHARE_349)
    // ... (existing early-return logic unchanged) ...

    bleProxyEnabled_ = proxyEnabled;
    bleProxyClientConnected_ = clientConnected;
    bleReceivingData_ = receivingData;
    drawBLEProxyIndicator();

    if (proxyChanged) {
        drawRssiIndicator(bleCtx_.v1Rssi);
    }

    // BLE and RSSI icons are both in the LEFT strip (x=8..60, y=99..170)
    flushRegion(DisplayLayout::STRIP_LEFT_X, DisplayLayout::STRIP_Y,
                DisplayLayout::STRIP_LEFT_W, DisplayLayout::STRIP_H);
#endif
}
```

**CI contract update**: The contract snapshot at
`test/contracts/display_flush_discipline_contract.txt` tracks every flush
call-site. This change removes the `BARE_FLUSH` at `display.cpp:setBLEProxyStatus`.
The `flush()` member function itself is still called elsewhere, so it stays in
the snapshot. But the call-site line in `setBLEProxyStatus` changes from
`flush()` to `flushRegion()`.

Run `scripts/check_display_flush_discipline_contract.py --update` to regenerate
the snapshot after this change.

**Risk**: Low. `setBLEProxyStatus()` is called from `drawProfileIndicator()` (during
full redraws where `DISPLAY_FLUSH()` follows anyway) and from
`connection_state_module.cpp` (when disconnected — icons only). In both cases, the
BLE/RSSI icons are entirely within the left strip.

---

### Step 6: Fix `connection_state_module.cpp` Flush

Currently (line 81–84):

```cpp
if (!isConnected) {
    display->drawWiFiIndicator();
    display->drawBatteryIndicator();
    display->flush();
}
```

WiFi is in LEFT strip (x=8, y=145). Battery is in RIGHT strip (x=618, y=136).
Replace:

```cpp
if (!isConnected) {
    display->drawWiFiIndicator();
    display->drawBatteryIndicator();
    display->flushRegion(DisplayLayout::STRIP_LEFT_X, DisplayLayout::STRIP_Y,
                         DisplayLayout::STRIP_LEFT_W, DisplayLayout::STRIP_H);
    display->flushRegion(DisplayLayout::STRIP_RIGHT_X, DisplayLayout::STRIP_Y,
                         DisplayLayout::STRIP_RIGHT_W, DisplayLayout::STRIP_H);
}
```

**CI contract update**: Run `--update` again.

**Risk**: Low. This path only runs when BLE is disconnected.

---

### Step 7: Verify the Cards-Only Fast Path (Already Correct)

The existing cards-only path at line 720 already uses flushRegion correctly:

```cpp
flushRegion(DisplayLayout::CONTENT_LEFT_MARGIN,
            SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT,
            DisplayLayout::CONTENT_AVAILABLE_WIDTH,
            SECONDARY_ROW_HEIGHT);
```

This is a sub-region of the CENTER strip. It does NOT hit the fast path (h=54,
not 172), so it uses the row-by-row fallback — but only 54 rows of 320px width.
That's still much better than a full flush. No change needed here.

---

### Step 8: Update CI Contract Snapshot

After all changes:

```bash
python3 scripts/check_display_flush_discipline_contract.py --update
```

Then verify:

```bash
python3 scripts/check_display_flush_discipline_contract.py
```

The contract file will show:
- Removal of `BARE_FLUSH` in `setBLEProxyStatus` (now uses `flushRegion`)
- `POINTER_FLUSH` in `connection_state_module.cpp` changes to `flushRegion` calls
- Line numbers shift in `display_update.cpp` (the `DISPLAY_FLUSH` entries for the
  incremental paths disappear; full-redraw paths keep theirs)

The full-redraw paths (`drawBaseFrame()` + `DISPLAY_FLUSH()`) are **not touched**
by this plan. They stay as-is. Only incremental paths change.

---

### Step 9: Run Tests and Validate

```bash
pio test -e native          # Unit tests
scripts/ci-test.sh          # Full CI suite including contract check
./build.sh --all            # Build + flash
```

**On-device validation checklist**:

- [ ] Resting display: volume change → left strip only (no flicker in arrows/freq)
- [ ] Resting display: RSSI periodic refresh → left strip only
- [ ] Resting display: bogey counter change → left strip only
- [ ] Live display: frequency change only → center strip only
- [ ] Live display: arrow change only → right strip only
- [ ] Live display: signal bars change → right strip only
- [ ] Live display: band change → left strip only
- [ ] Live display: OBD toggle → center strip only
- [ ] Live display: cards expire → center strip only
- [ ] Live display: multiple simultaneous changes → correct strips update
- [ ] BLE disconnect → WiFi and battery update (left + right strips)
- [ ] Profile change → profile indicator visible (separate path, not affected)
- [ ] Full redraw paths → still use DISPLAY_FLUSH (unchanged)
- [ ] Touch long-press OBD pair → OBD badge updates (uses its own flushRegion)
- [ ] Touch long-press WiFi toggle → WiFi icon updates (uses flush())
- [ ] No tearing or visual artifacts on any transition

---

## What This Plan Does NOT Touch

These paths remain on full `DISPLAY_FLUSH()` and are explicitly out of scope:

| Path | Why |
|------|-----|
| Resting full redraw | `drawBaseFrame()` clears screen — must flush everything |
| Live full redraw | Same — `fillScreen()` invalidates entire canvas |
| `updatePersisted()` | Always full redraw |
| `showResting()` | Initial screen paint |
| `showScanning()` | Full-screen mode |
| `showBootSplash()` | Full-screen mode |
| `showShutdown()` | Full-screen mode |
| `showLowBattery()` | Full-screen mode |
| `showSettingsSliders()` | Full-screen mode |
| `display.begin()` | Hardware init |
| `display.clear()` | Explicit clear |
| `touch_ui_module` WiFi toggle | Uses `flush()` — could be optimized later |

These are all either full-screen repaints (where strip flush would flush all 3
strips anyway) or one-shot transitions. Optimizing them yields no benefit.

---

## Rollback Strategy

If strip flushing produces visual artifacts on-device:

1. Revert step 4 only (replace `flushDirtyStrips()` with `DISPLAY_FLUSH()`)
2. Steps 0–3 and 5–6 are independently correct and can stay

The strip boolean wiring (steps 1–3) and the `setBLEProxyStatus` fix (step 5) are
improvements regardless of whether strip flushing is active. They fix the
element-to-strip mapping bugs and eliminate a stray full flush in a hot path.

---

## Expected Performance Impact

### Resting Mode (Most Common)

Typical incremental frame: only volume OR RSSI OR bogey counter changes.
That's 1 strip (LEFT, 120×172 = 41,280 bytes) instead of full screen (220,160 bytes).

**~81% reduction in SPI traffic per incremental frame.**

### Live Mode

Typical incremental frame: frequency + signal bars change together.
That's 2 strips (CENTER + RIGHT = 320+200 = 520×172 = 178,880 bytes) vs full
screen (220,160 bytes).

**~19% reduction.** Modest but consistent. Single-element changes (arrow only,
frequency only) save much more.

### Worst Case

All 3 strips dirty: 3 × `draw16bitRGBBitmap` calls totaling 640×172 bytes.
Same total bytes as `DISPLAY_FLUSH()` but 3 function calls instead of 1.
Overhead is ~microseconds of function call setup — negligible vs SPI transfer time.

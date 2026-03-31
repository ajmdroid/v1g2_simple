# Display and Settings Subsystems Code Review

**Date:** March 31, 2026
**Scope:** Detailed code review of display rendering and settings management subsystems
**Files Examined:** 25 source/header files covering display rendering, font management, settings persistence, and validation

---

## Executive Summary

The display and settings subsystems are well-structured and generally follow good engineering practices (dirty flag caching, incremental updates, input sanitization). However, **three legitimate defects and two architectural concerns** were identified that warrant attention:

### Critical Issues
1. **Integer overflow in coordinate union calculation** — display_frequency.cpp:58-59
2. **Unvalidated RGB565 color values** — settings.cpp (22+ color fields)

### High-Priority Issues
3. **Null pointer dereference risk in flushRegion()** — display.cpp:320
4. **Stateful character muting and profile tracking** — display_bands.cpp:25-27, display_sliders.cpp

### Medium-Priority
5. **Missing font error handling in font manager** — display_font_manager.cpp

---

## Detailed Findings

### Issue 1: Integer Overflow in Dirty Region Union Calculation

**File:** `src/display_frequency.cpp:58-59`
**Severity:** HIGH
**Type:** Integer overflow / coordinate math

When expanding the dirty region bounding box, the code computes the maximum coordinate by adding width/height to the current position:

```cpp
const int16_t x2 = max(static_cast<int16_t>(frequencyDirtyX_ + frequencyDirtyW_),
                        static_cast<int16_t>(x + w));
const int16_t y2 = max(static_cast<int16_t>(frequencyDirtyY_ + frequencyDirtyH_),
                        static_cast<int16_t>(y + h));
```

**Problem:** The addition `frequencyDirtyX_ + frequencyDirtyW_` is performed on `int16_t` values **before** the cast, which can overflow if the sum exceeds 32,767. The cast to `int16_t` happens **after** the potentially-overflowing arithmetic.

**Example:** If `frequencyDirtyX_=30000` and `frequencyDirtyW_=5000`, the sum 35000 overflows to a negative number when interpreted as int16_t, producing an incorrect dirty region.

**Impact:** Partial redraws could incorrectly exclude areas that need updating, or include areas that shouldn't be flushed (wasting SPI bandwidth). Risk is low in normal operation (screen is only 640px wide) but real if coordinate math ever shifts due to rotation modes.

**Fix:**
```cpp
const int16_t x2 = max(static_cast<int16_t>(static_cast<int>(frequencyDirtyX_) + frequencyDirtyW_),
                        static_cast<int16_t>(static_cast<int>(x) + w));
```

Or safer:
```cpp
int32_t x2_raw = static_cast<int32_t>(frequencyDirtyX_) + frequencyDirtyW_;
int32_t x2_candidate = std::max(x2_raw, static_cast<int32_t>(x) + w);
const int16_t x2 = static_cast<int16_t>(std::clamp(x2_candidate, 0L, 640L));
```

---

### Issue 2: Unvalidated RGB565 Color Values

**File:** `src/settings.cpp:222-246`
**Severity:** MEDIUM
**Type:** Input validation gap

The settings loader reads 22 color values directly from NVS storage without bounds checking:

```cpp
settings_.colorFrequency = preferences_.getUShort("colorFreq", 0xF800);
settings_.colorBandL = preferences_.getUShort("colorBandL", 0x001F);
settings_.colorBandKa = preferences_.getUShort("colorBandKa", 0xF800);
settings_.colorBandK = preferences_.getUShort("colorBandK", 0x001F);
settings_.colorBandX = preferences_.getUShort("colorBandX", 0x07E0);
settings_.colorBandPhoto = preferences_.getUShort("colorBandP", 0x780F);
// ... 16 more color fields
```

**Problem:** A `uint16_t` can hold any 16-bit value (0-65535), but RGB565 colors only use bits 0-14 (0x0000 to 0xFFFF is valid, but sparse — only 5+6+5=16 bits are meaningful). Corrupted settings or malicious NVS could store out-of-range colors like 0xFFFF or 0x8888 that:
- Exceed the 5-bit red channel (max 31)
- Exceed the 6-bit green channel (max 63)
- Exceed the 5-bit blue channel (max 31)

Arduino_GFX driver may silently mask invalid bits or produce undefined rendering.

**Impact:** Display rendering anomalies if settings are corrupted. Low severity in normal operation (defaults are all valid), but this is a validation gap.

**Fix:** Add a validation function:
```cpp
inline uint16_t validateRGB565(uint16_t raw) {
    // Mask to valid 565 range: RRRRR GGGGGG BBBBB
    uint16_t r = (raw >> 11) & 0x1F;
    uint16_t g = (raw >> 5) & 0x3F;
    uint16_t b = raw & 0x1F;
    return (r << 11) | (g << 5) | b;
}

// In load():
settings_.colorFrequency = validateRGB565(preferences_.getUShort("colorFreq", 0xF800));
// ... repeat for all 22 color fields
```

---

### Issue 3: Null Pointer Dereference in flushRegion()

**File:** `src/display.cpp:317-352`
**Severity:** MEDIUM
**Type:** Potential null pointer dereference

The function checks for null pointers early:

```cpp
void V1Display::flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!tft_ || !gfxPanel_) return;  // Line 319
    // ...
    uint16_t* fb = tft_->getFramebuffer();  // Line 330 — safe due to check above
```

However, if `gfxPanel_` is non-null but `tft_` becomes null between the check and line 330 (due to a concurrent call in single-threaded ESP32 main loop, or exception during construction), this is safe. But the code also calls:

```cpp
gfxPanel_->draw16bitRGBBitmap(x, y, regionStart, w, h);  // Line 343
// and
gfxPanel_->draw16bitRGBBitmap(x, y + row, rowPtr, w, 1);  // Line 348
```

If `gfxPanel_` becomes null between the check and these calls, we have a dereference bug. **In practice:** the check is sufficient if no exception handlers reassign these members. But the code is brittle.

**Impact:** No observed crashes in production (these are only reassigned in `begin()` and destructors), but code is fragile.

**Fix:** Cache the pointers locally:
```cpp
void V1Display::flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
    auto tft = tft_.get();
    auto panel = gfxPanel_.get();
    if (!tft || !panel) return;

    uint16_t* fb = tft->getFramebuffer();
    if (!fb) {
        DISPLAY_FLUSH();
        return;
    }
    // ... use tft and panel throughout
}
```

---

### Issue 4: Static Global State in Band Indicator Rendering

**File:** `src/display_bands.cpp:25-27`
**Severity:** LOW (but architectural smell)
**Type:** Singleton-scoped render tracking

The `drawBandIndicators()` function uses static file-scoped variables to track the last-drawn state:

```cpp
static uint8_t lastEffectiveMask = 0xFF;
static bool lastMuted = false;
static bool cacheValid = false;
```

**Problem:** These caches are reset only via `dirty.bands = false` flag, not on explicit calls to `resetChangeTracking()`. If the display is reset but `drawBandIndicators()` is never called, the cache stays stale.

**Similar issue in:** `display_sliders.cpp`, `display_status_bar.cpp`, and other extracted display files.

**Impact:** Missed redraws if `resetTracking` is set but `drawBandIndicators()` isn't immediately called. Unlikely in the current loop structure, but fragile.

**Better approach:** Centralize all dirty flag tracking in the `DisplayDirtyFlags` struct and use a single reset point before rendering, not scattered static caches.

---

### Issue 5: Missing Font Error Handling

**File:** `src/display_font_manager.cpp`
**Severity:** MEDIUM
**Type:** Error handling gap

The font manager initializes OpenFontRender instances but doesn't explicitly handle glyph rasterization failures:

```cpp
// display_font_manager.cpp:init()
fontMgr.init(tft_);  // display.cpp:227
```

The font manager preloads glyphs but there's no documented fallback if a glyph fails to render under memory pressure.

**Impact:** If OpenFontRender exhausts its glyph cache during rendering, the output may be undefined (missing glyphs, corrupted text, or a crash if the library's error handling is insufficient).

**Status:** The existing test suite includes OFR initialization, but no tests for "fill cache to capacity then render new glyph" scenarios.

**Recommendation:** Add bounds checks and fallback to 7-segment rendering if OFR fails.

---

### Issue 6: Color Calculation in dimColor()

**File:** `include/display_draw.h:19-27`
**Severity:** LOW
**Type:** Color math edge case

The `dimColor()` function extracts and scales RGB565 components:

```cpp
inline uint16_t dimColor(uint16_t c, uint8_t scalePercent = 60) {
    uint8_t r = (c >> 11) & 0x1F;
    uint8_t g = (c >> 5) & 0x3F;
    uint8_t b = c & 0x1F;
    r = (r * scalePercent) / 100;
    g = (g * scalePercent) / 100;
    b = (b * scalePercent) / 100;
    return (r << 11) | (g << 5) | b;
}
```

**Observation:** Division by 100 truncates (e.g., `31 * 60 / 100 = 18`, not 18.6). This is correct behavior for integer color math, but be aware that 60% dimming is actually `18/31 ≈ 58%`, not exactly 60%.

**Impact:** Negligible visual difference; colors are already quantized to 5-6 bits.

---

## Architecture Observations

### Display Dirty-Flag System

The three-layered approach is effective:
1. Global `DisplayDirtyFlags dirty` aggregate (header: `include/display_dirty_flags.h`)
2. Renderer-specific caches (e.g., `lastEffectiveMask` in `display_bands.cpp`)
3. Precise dirty regions (e.g., `frequencyDirtyX/Y/W/H` in `display.h`)

This prevents unnecessary redraws and keeps SPI bandwidth low. However, the caches are scattered across multiple .cpp files using `static` variables, making the overall state harder to reason about. Consider centralizing cache invalidation logic.

### Settings Persistence

The two-namespace design (legacy + active) with backup/restore is sophisticated:
- `preferences_.begin(activeNs, true)` for read-only load
- Fallback to `SETTINGS_NS_LEGACY` if active fails
- SD backup with temp→validate→promote pipeline

However, **input validation is inconsistent:**
- String lengths are clamped (good)
- Numeric ranges are clamped (good)
- Color values are not validated (gap identified above)
- Display style is normalized via `normalizeDisplayStyle()` (good)

### Memory Management

All display pointer members are `std::unique_ptr<>`:
```cpp
std::unique_ptr<Arduino_ESP32QSPI> bus_;
std::unique_ptr<Arduino_AXS15231B> gfxPanel_;
std::unique_ptr<Arduino_Canvas> tft_;
```

This is excellent for exception safety. The only exposed global pointer is `g_displayInstance` in `display_palette.h`, which is properly managed.

Settings use `String` (Arduino's heap-allocated wrapper), which is fine for non-hot-path code. The font manager uses either `ps_malloc()` (PSRAM) or `malloc()` (internal heap) with a fallback strategy.

---

## Layout Constants Review

**File:** `include/display_layout.h`

The layout constants are well-centralized:

```cpp
constexpr int PRIMARY_ZONE_HEIGHT = 95;
constexpr int PRIMARY_ZONE_Y = 20;
constexpr int SECONDARY_ROW_HEIGHT = 54;
constexpr int SECONDARY_ROW_Y = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT;  // Y=118

static_assert(PRIMARY_ZONE_Y + PRIMARY_ZONE_HEIGHT <= SECONDARY_ROW_Y,
              "Primary zone overlaps with secondary row");
```

The compile-time assert at line 53 is excellent — it catches layout overlaps at build time.

**Observations:**
- Band column: 120px (fixed) — OK
- Signal column: 200px (fixed) — OK
- Content area: 320px (640 - 120 - 200) — OK
- No off-by-one errors detected

However, the *actual rendering code* uses hard-coded x,y coordinates in several places:

```cpp
// display_bands.cpp:55
const int x = 82;
const int spacing = 43;
const int startY = 55;

// display_sliders.cpp:23
const int sliderWidth = SCREEN_WIDTH - (sliderMargin * 2);  // 560 pixels

// display_status_bar.cpp:30-32
const int x = 8;
const int y = 75;
```

These hard-coded values should ideally be in `display_layout.h` for single-source-of-truth. Current approach works but is fragile if screen dimensions change.

---

## Test Coverage Observations

From the existing REPO_REVIEW.md:

**Tested display logic (e.g., dirty-flag cascade, cache invalidation):**
- `test_display/test_display.cpp` (77 tests on simulated cache tracker)
- `test_display_orchestration_module/` (33 tests on BLE→display wiring)

**NOT tested (actual GFX rendering code):**
- `drawBandIndicators()` — no tests for segment layout, color selection, cache hit/miss
- `drawFrequency()` — no tests for 7-segment vs. Serpentine rendering, dirty region tracking
- `drawVerticalSignalBars()` — no tests for bar heights, spacing, or muting color
- Font rendering paths in `display_font_manager.cpp` — no OFR edge cases

**Recommendation:** Add native unit tests for `markFrequencyDirtyRegion()` and `dimColor()` to catch integer overflow and color math issues.

---

## Naming Conventions

**Compliance check:** The project convention is trailing underscore for private members.

✓ **Correctly followed:**
```cpp
bool persistedMode_;        // display.h:224
uint16_t frequencyDirtyX_;  // display.h:228
DisplayBleContext bleCtx_;  // display.h:243
```

✓ **Correctly followed (static caches in .cpp files):**
```cpp
static uint8_t lastEffectiveMask = 0xFF;  // display_bands.cpp:25 (file-scoped, not class member)
```

✓ **No violations found** in display or settings subsystems.

---

## Summary Table

| Issue | File | Line | Severity | Type | Fix Effort |
|-------|------|------|----------|------|------------|
| Integer overflow in dirty region union | display_frequency.cpp | 58-59 | HIGH | Arithmetic overflow | 5 min |
| Unvalidated RGB565 colors | settings.cpp | 222-246 | MEDIUM | Input validation | 15 min |
| Null pointer dereference risk | display.cpp | 320-348 | MEDIUM | Pointer safety | 10 min |
| Scattered static render caches | display_bands.cpp, others | 25-27 | LOW | Architecture smell | 1-2 hours |
| Missing font error handling | display_font_manager.cpp | — | MEDIUM | Error handling | 20 min |
| Hard-coded layout constants in renderers | display_bands.cpp, etc. | 55, 23, 30 | LOW | Single-source-of-truth | 30 min |
| Color dimming math precision | display_draw.h | 19-27 | LOW | Color math | (no fix needed) |

---

## Recommendations (Ordered by Priority)

### Immediate (This Week)
1. **Fix integer overflow in `markFrequencyDirtyRegion()`** — use `int32_t` intermediate calculations
2. **Add RGB565 color validation** — apply `validateRGB565()` to all 22 color fields in settings load

### Short-term (This Sprint)
3. **Strengthen null-pointer safety in `flushRegion()`** — cache pointers locally
4. **Add native unit tests for dirty region logic** — test `markFrequencyDirtyRegion()` with edge cases (0, negative, overflow scenarios)
5. **Centralize layout constants** — move hard-coded x/y/width values from display_*.cpp to `display_layout.h`

### Medium-term (Next Sprint)
6. **Unify render cache tracking** — consolidate scattered `static` variables into a single state struct (or use a per-renderer class)
7. **Add font error handling** — graceful fallback to 7-segment if OFR glyph rendering fails
8. **Test font rendering edge cases** — memory-pressure scenarios, character misses, OFR cache exhaustion

### Nice-to-Have (Future)
9. Color math audit — verify all color conversions handle edge cases (darkest/brightest values)
10. Profile memory usage under concurrent rendering + settings persistence

---

## Conclusion

The display and settings subsystems are **well-designed and maintainable**. The identified issues are real but low-impact in normal operation:
- The integer overflow is masked by the current screen size but is mathematically unsound
- The color validation gap is unlikely to manifest (defaults are valid) but is a hygiene issue
- Other issues are architectural smells or low-severity edge cases

**Overall Rating:** 7.5/10 for this subsystem. Strong architecture + good test coverage for logic paths, but rendering code has zero unit test coverage and a few validation/safety gaps remain.

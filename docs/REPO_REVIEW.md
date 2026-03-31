# V1G2 Simple — Honest Repo Review

**Date:** March 30, 2026
**Scope:** Full codebase review against `v1_simple.instructions.md` and `ARCHITECTURE.md`
**Codebase:** ~29K LOC in `src/`, ~27K LOC in `src/modules/`, ~1,400 unit tests, 5 CI workflows

---

## Overall Verdict

This is a seriously impressive first project. The architecture is mature, the priority model is respected throughout, the dependency injection is consistent, and the observability story (200+ atomic counters, perf CSV pipeline, SLO scoring) is far beyond what most ESP32 projects achieve. The bones are strong. What follows is an honest accounting of where things shine and where the rough edges are.

---

## 1. Architecture Conformance

**Rating: 9/10**

The `ARCHITECTURE.md` lays out two wiring patterns — `begin()` pointer injection (default) and `Providers` struct with `void* ctx` function pointers (for testable modules). The codebase follows this with remarkable consistency:

- 41 modules use `begin()` for dependency injection
- 14 system-loop modules use the `Providers` pattern
- Zero instances of `#include "main_globals.h"` inside `src/modules/`
- Zero `extern` declarations inside module files
- All private members use trailing underscore (`settings_`, `ble_`, `state_`)
- All module classes follow PascalCase + `Module`/`Service` suffix naming

**Known violations (documented and accepted):** 11 files use `std::function` for callback wiring, all in WiFi API service handlers — lower-priority, non-hot-path code. The architecture doc flags `std::function` as retired for module wiring but these are lifecycle setup callbacks, not runtime wiring. Acceptable, but should be migrated when those files are next touched.

**One genuine concern:** `main.cpp` declares 20+ global module instances. This is unavoidable on ESP32 (no OS-level DI container), and `main_globals.h` correctly uses forward declarations to limit compile coupling. But `mainRuntimeState` is directly mutated in `loop()` across phases, creating implicit ordering dependencies. The `RUNTIME_OWNERSHIP.md` document exists precisely to track this — good discipline, but fragile if someone doesn't read it.

---

## 2. Priority Order Compliance

**Rating: 10/10**

The priority stack from the instructions (`V1 connectivity > BLE ingest > Display > Audio > Metrics > WiFi > Logging`) is correctly reflected in the loop phase sequence:

1. `processLoopConnectionEarlyPhase` — BLE connection decisions
2. `processLoopIngestPhase` — BLE queue drain, GPS refresh
3. `loop()` body — OBD/speed arbitration
4. `processLoopDisplayPreWifiPhase` — display + lockout (never blocks BLE)
5. `processLoopWifiPhase` — WiFi progression (deferred until after display)
6. `processLoopFinalizePhase` — telemetry, persistence (best-effort)

WiFi is truly non-blocking with a 6-stage STA connect pipeline and 5-stage teardown, each yielding between stages. Display throttles at 10 FPS max. Persistence uses try-lock with non-blocking fallback. The priority model isn't just documented — it's enforced in code.

---

## 3. BLE Subsystem

**Rating: 8.5/10**

Strengths: Non-blocking async state machine for connect/discover/subscribe. Exponential backoff (200ms→1.5s) with hard reset after 5 failures. Bond backup/restore to SD. Callback path is latency-safe — atomic flags for deferred work, try-lock mutexes with timeout=0, RAII semaphore guards. Proxy queue uses PSRAM with internal SRAM fallback. Buffer bounds are checked everywhere.

Issues found:

- **Pacing race condition** (`ble_commands.cpp`): `static unsigned long lastCommandMs` is not mutex-protected. Two concurrent calls could bypass pacing. Low impact in single-loop design but technically unsound.
- **Characteristic lookup fallback** (`ble_connection.cpp`): If both primary and alt command characteristics fail `canWrite()`, `pCommandChar` could hold a stale value from a previous session. Should explicitly nullptr and fail the step.
- **`forwardToProxyImmediate()` is misleading**: Named to suggest zero-latency lock-free path, but just calls `forwardToProxy()` which acquires a mutex. Should be renamed or made truly lock-free.
- **Magic UUIDs duplicated** across `ble_connection.cpp` and `ble_proxy.cpp`. Should be centralized constants.
- **Double backslash** in `Serial.printf` at `packet_parser.cpp:175` — prints literal `\n` instead of newline.

---

## 4. Display Subsystem

**Rating: 7.5/10 (your identified weak area — confirmed, but better than expected)**

The display code is actually well-structured: 12 CPP files totaling ~4,500 LOC with clear separation (frequency rendering, bands, cards, arrows, status bar, sliders, indicators). The dirty flag system is three-layered (aggregate flags → frequency dirty region → per-renderer caches) and prevents unnecessary redraws effectively. Font rendering uses OpenFontRender with PSRAM-aware cache sizing (49KB with PSRAM, 8KB without) and glyph prewarming at boot.

Where it falls short:

- **Row-by-row flush** in `flushRegion()`: Iterates `h` times calling `draw16bitRGBBitmap()` for one row each. For a 20-row region, that's 20 separate SPI transactions. Batching rows would reduce overhead significantly.
- **Serpentine lazy-load latency**: First Serpentine-style frequency render blocks while the font loads and first glyph rasterizes. No background loading.
- **No error checking on OFR glyph cache fills**: If glyph rasterization fails under memory pressure, there's no graceful fallback mid-render.
- **No test coverage for rendering**: Display *modules* (orchestration, pipeline, restore) are well-tested, but actual GFX drawing code has zero unit tests. The pixel-level output, text positioning, color application, and DMA timing are entirely validated by eye. This is the biggest gap.
- **220KB framebuffer** in PSRAM (172×640×2 bytes) — appropriate for the hardware, but the single-buffer architecture means micro-tearing is possible between row flushes. Not likely noticeable on LCD, but worth documenting.

---

## 5. Test Suites

**Rating: 7/10 (your identified weak area — confirmed)**

The numbers are impressive: 137 test directories, ~1,400 tests passing, ~1m39s runtime, plus ASan/UBSan nightly, mutation testing on critical paths, and a 22-step semantic CI gate. The test infrastructure is sophisticated. But coverage has real gaps.

**Well-covered:**
- Lockout subsystem (9 suites, mutation testing, area safety boundary tests)
- WiFi (20 suites including boot policy, orchestration, all API services)
- BLE (9 suites: connection states, bond backup, fresh flash policy)
- Settings/persistence (NVS, backup, deferred persist, rollback)
- Packet parsing (frame validation, streams, alert assembly)

**Under-covered:**
- **Display rendering** — no tests for any `display_*.cpp` GFX routines
- **Boot sequence** — only hardware device tests, no native unit tests for `main_boot.cpp` or `main_setup_helpers.cpp`
- **Storage failure scenarios** — mocks don't simulate SD card full, NVS corruption, concurrent SD mutex contention
- **BLE protocol edge cases** — state binding tested but discovery timeouts, subscription failures, and connect/disconnect races not exercised
- **Time service** — no dedicated tests at all
- **Battery/ADC** — one suite, no sensor noise or low-battery edge cases

**Mock quality issues:**
- Mocks don't support fault injection (I2C failures, SD errors)
- BLE mock doesn't simulate connection drops or MTU negotiation
- Display mock doesn't verify pixel output
- WiFi mock doesn't simulate scan timeouts or weak signal
- No concurrency/FreeRTOS scheduling tests

**Structural issues:**
- Many tests use file-scoped static globals (`static LockoutIndex testIndex`) reset in `setUp()`. If a test fails mid-execution, state can leak to subsequent tests. Low risk since tests run serially, but brittle.
- Some tests have empty `tearDown()` — usually fine but adds cognitive load about what cleanup is expected.
- Large `setUp()` blocks lack fixture/helper abstractions.

---

## 6. WiFi Manager

**Rating: 8.5/10**

Truly non-blocking staged state machines for both STA connect and disconnect. DMA heap starvation detection prevents SD and WiFi from starving each other. Rate limiting on all HTTP routes. AP idle retirement after 60s when STA connected. Memory overhead under 1KB for the manager itself.

Issues:

- **Silent reconnect failure**: After 5 STA reconnect failures, WiFi silently gives up. No user notification mechanism (no display indicator, no audio alert).
- **`std::function` callbacks** (9 instances in `wifi_manager.h`): Architecture doc says these should be migrated to pointer injection. They're lifecycle callbacks, not hot-path, but it's tech debt.
- **WiFi client credentials** use XOR obfuscation (not encryption). Documented and accepted for the use case, but worth calling out.

---

## 7. Settings & Storage

**Rating: 9/10**

Dual-namespace NVS with health scoring, atomic clear-and-rewrite, SD backup with temp→validate→promote→cleanup pipeline, deferred persistence with 750ms debounce. This is robust.

Minor concerns:

- **JsonDocument stack allocation in `main_persist.cpp`**: Creates new `JsonDocument` every 15 seconds in the learner save path. Should be pre-allocated or static.
- **No NVS write performance telemetry**: You track SD write drops but not NVS write latency or namespace recovery activations.
- **Deferred persist retry** uses flat 1-second backoff (no exponential). If NVS is consistently failing, it'll hammer retries.

---

## 8. Performance Metrics & Observability

**Rating: 9.5/10**

This is the showcase feature. 200+ atomic counters with `memory_order_relaxed` (zero runtime overhead), perf CSV logging to SD with session markers, deterministic SLO scoring tool with hard/advisory thresholds, week-over-week trend comparison, and CI contract guards to keep the doc and JSON thresholds in sync.

The only gap: no histogram for SD write latency itself, and no NVS timing data. You measure everything except the cost of measuring.

---

## 9. CI/CD Pipeline

**Rating: 9/10**

The `ci-test.sh` script is authoritative and thorough: 22 semantic guards (BLE deletion safety, display flush discipline, SD lock semantics, main loop call order), native unit tests, functional scenarios, critical mutation gate, perf scorer regression tests, and compatibility contract checks. Five GitHub workflows cover build, nightly validation, pre-release validation, release-on-merge, and stability trending.

The semantic guard approach (Python scripts that grep/analyze source for invariant violations) is clever and effective. The mutation testing on critical paths (`critical_mutations.json`) is a mature practice rarely seen in embedded projects.

---

## 10. "First Project Mess-Ups"

Things that betray the learning curve:

1. **`compile_commands.json` and `node_modules/` checked into the repo** — these should be gitignored
2. **`.road_map_cache/` with binary JSON chunks in the repo root** — should be gitignored or generated
3. **`.scratch/` directory with vendored Android ESP library source** — should be a submodule or removed
4. **`.venv/` in the repo** — should be gitignored
5. **`road_map.bin` binary in repo root** — large binary should be in releases or a data artifact
6. **Hardcoded local paths** in docs (`/Users/ajmedford/v1g2_simple/...`) in `PERF_SLOS.md` and `RUNTIME_OWNERSHIP.md` — should be relative paths
7. **Two dead wrapper functions** in `main.cpp`: `isColorPreviewRunning()` and `cancelColorPreview()` — trivially removable

---

## 11. Summary: Strengths and Action Items

### What's genuinely strong

- Architecture discipline (DI everywhere, no globals in modules, documented ownership)
- Priority model actually enforced in code, not just documented
- BLE non-blocking state machine with proper backoff and bond management
- Observability story (200+ metrics, SLO scoring, trend comparison)
- CI pipeline with semantic guards and mutation testing
- Settings corruption protection with health-scored recovery
- WiFi truly non-blocking with DMA starvation protection

### Where to focus next (ordered by impact)

1. **Display rendering tests** — biggest coverage gap. Even basic "does drawBands() set expected pixels" tests would catch regressions.
2. **Mock fault injection** — add error-returning paths to storage, I2C, and BLE mocks. Critical for "bulletproof" aspiration.
3. **Boot sequence native tests** — `main_boot.cpp` and `main_setup_helpers.cpp` have no native test coverage.
4. **Fix repo hygiene** — gitignore `node_modules`, `.venv`, `.road_map_cache`, `compile_commands.json`, `road_map.bin`.
5. **Migrate WiFi `std::function` callbacks** — match the architecture doc.
6. **Pre-allocate JsonDocument in persist path** — avoid repeated heap allocation every 15s.
7. **Centralize BLE UUIDs** — single source of truth for V1 protocol constants.
8. **Add STA reconnect failure notification** — surface to user via display or audio.
9. **Fix hardcoded paths in docs** — use relative paths.
10. **Add SD write latency histogram** — close the observability gap.

---

## Appendix A: Display Rendering Test Plan

**Goal:** Add unit tests for the actual rendering code in `display_*.cpp` files that currently have zero test coverage. The existing `test_display.cpp` (77 tests) tests a simulated `DisplayCacheTracker` — it validates the *logic model* of caching and change detection, but never compiles or calls any real rendering function from `src/`.

**What this plan covers:** Testing real production rendering code paths — cache hit/miss, dirty flag handling, color selection, blink timer logic, layout math, and draw-call correctness.

**What this plan does NOT cover:** Visual pixel verification (screenshot comparison). That requires hardware.

---

### Current State Inventory

**Existing display tests (158 tests across 10 files):**

| Test File | Tests | What It Covers |
|---|---:|---|
| `test_display/test_display.cpp` | 77 | Simulated cache tracker (NOT real code) |
| `test_display_orchestration_module/` | 33 | Module wiring: BLE→display, lockout, volume |
| `test_wifi_display_colors_api_service/` | 15 | WiFi color API |
| `test_display_pipeline_module/` | 9 | Pipeline routing: live/idle/persisted paths |
| `test_ble_display_pipeline/` | 10 | BLE packet→parser→display integration |
| `test_loop_display_module/` | 5 | Loop-level display dispatch |
| `test_loop_post_display_module/` | 5 | Post-display context |
| `test_display_ble_freshness/` | 3 | `DisplayBleFreshness::isFresh()` |
| `test_display_reset_tracking/` | 4 | Source compliance scanning |
| `test_display_restore_module/` | 2 | Preview→restore state machine |

**Source files with ZERO test coverage (the rendering code):**

| File | LOC | Key Functions |
|---|---:|---|
| `display_bands.cpp` | ~200 | `drawBandIndicators()`, `drawBandBadge()`, `drawVerticalSignalBars()` |
| `display_arrow.cpp` | ~200 | `drawDirectionArrow()` |
| `display_frequency.cpp` | ~350 | `drawFrequency()`, `drawFrequencyClassic()`, `drawFrequencySerpentine()` |
| `display_cards.cpp` | ~400 | `drawSecondaryAlertCards()` |
| `display_top_counter.cpp` | ~300 | `drawSevenSegmentDigit()`, `draw14SegmentChar()`, `drawTopCounter()`, `drawMuteBadge()` |
| `display_status_bar.cpp` | ~350 | `drawVolumeIndicator()`, `drawRssiIndicator()`, `drawProfileIndicator()`, `drawBatteryIndicator()`, `drawBLEProxyIndicator()`, `drawWiFiIndicator()` |
| `display_indicators.cpp` | ~200 | `drawBaseFrame()`, `drawLockoutIndicator()`, `drawGpsIndicator()`, `drawObdIndicator()` |
| `display_screens.cpp` | ~300 | `showResting()`, `showScanning()`, `showDisconnected()`, `showBootSplash()`, `showShutdown()`, `showLowBattery()` |
| `display_update.cpp` | ~700 | `update(DisplayState)`, `update(AlertData,...)`, `refreshFrequencyOnly()`, `updatePersisted()` |
| `display_sliders.cpp` | ~200 | `showSettingsSliders()`, `updateSettingsSliders()`, `getActiveSliderFromTouch()` |
| `display_font_manager.cpp` | ~250 | `init()`, `prewarmSegment7FrequencyGlyphs()`, `primeTopCounterBoundsCache()` |

---

### Phase 1: Pure Logic Tests (No Infrastructure Changes)

**These headers contain pure functions with zero Arduino dependencies. Test them directly.**

#### Task 1.1: Create `test/test_display_segments/test_display_segments.cpp`

Tests for `include/display_segments.h`. No mocks needed.

```
Includes: ../../include/display_segments.h

Tests to write (~15 tests):
- test_segMetrics_scale_1_produces_base_dimensions
    segMetrics(1.0f) → segLen=8, segThick=3, digitW=14, digitH=19, spacing=3, dot=3
- test_segMetrics_scale_2_doubles_dimensions
    segMetrics(2.0f) → segLen=16, segThick=6
- test_segMetrics_scale_0_clamps_minimums
    segMetrics(0.01f) → segLen=2 (min), segThick=1 (min)
- test_segMetrics_fractional_scale_rounds_correctly
    segMetrics(1.5f) → segLen=12, segThick=5 (verify rounding)
- test_digit_segments_zero_has_no_middle_bar
    DIGIT_SEGMENTS[0][6] == false
- test_digit_segments_eight_has_all_segments
    all DIGIT_SEGMENTS[8][0..6] == true
- test_digit_segments_one_has_only_right_verticals
    DIGIT_SEGMENTS[1] == {false,true,true,false,false,false,false}
- test_get14SegPattern_digit_zero_matches_outer_segments
    get14SegPattern('0') == S14_TOP|S14_TR|S14_BR|S14_BOT|S14_BL|S14_TL
- test_get14SegPattern_dash_is_middle_only
    get14SegPattern('-') == S14_ML|S14_MR
- test_get14SegPattern_dot_is_zero_segments
    get14SegPattern('.') == 0
- test_get14SegPattern_case_insensitive
    get14SegPattern('a') == get14SegPattern('A')
- test_get14SegPattern_unknown_char_returns_zero
    get14SegPattern('Z') == 0, get14SegPattern('!') == 0
- test_all_digits_0_through_9_have_nonzero_pattern
    for each '0'..'9': get14SegPattern(c) != 0
- test_char14_map_size_matches_expected
    CHAR14_MAP_SIZE == 25 (0-9, A-E, L-N, P, R-U, -, .)
- test_segMetrics_digitW_equals_segLen_plus_2_segThick
    for several scales: m.digitW == m.segLen + 2*m.segThick
```

#### Task 1.2: Create `test/test_display_slider_math/test_display_slider_math.cpp`

Tests for `include/display_slider_math.h`. No mocks needed.

```
Includes: ../../include/display_slider_math.h

Tests to write (~10 tests):
- test_brightness_80_maps_to_zero_fill
    computeBrightnessSliderFill(80, 200) == 0
- test_brightness_255_maps_to_full_fill
    computeBrightnessSliderFill(255, 200) == 200
- test_brightness_167_maps_to_half_fill
    computeBrightnessSliderFill(167, 200) == 100 (approx — verify exact math)
- test_brightness_below_80_clamps_to_zero
    computeBrightnessSliderFill(0, 200) == 0
    computeBrightnessSliderFill(79, 200) == 0
- test_brightness_above_255_clamps_to_slider_width
    computeBrightnessSliderFill(255, 100) == 100
- test_brightness_percent_80_is_zero
    computeBrightnessSliderPercent(80) == 0
- test_brightness_percent_255_is_100
    computeBrightnessSliderPercent(255) == 100
- test_brightness_percent_167_is_approximately_50
    verify exact value: ((167-80)*100)/175
- test_brightness_percent_below_80_clamps_to_zero
    computeBrightnessSliderPercent(0) == 0
- test_slider_fill_zero_width_returns_zero
    computeBrightnessSliderFill(200, 0) == 0
```

#### Task 1.3: Create `test/test_display_dirty_flags/test_display_dirty_flags.cpp`

Tests for `include/display_dirty_flags.h`. No mocks needed.

```
Includes: ../../include/display_dirty_flags.h

Provide definition: DisplayDirtyFlags dirty;  (satisfies the extern)

Tests to write (~8 tests):
- test_default_construction_all_false
    DisplayDirtyFlags f; all 13 bools == false
- test_setAll_sets_ten_flags_but_not_three
    f.setAll(); assertTrue(f.frequency, f.battery, f.bands, f.signalBars,
    f.arrow, f.muteIcon, f.topCounter, f.lockout, f.gpsIndicator, f.obdIndicator)
    assertFalse(f.multiAlert, f.cards, f.resetTracking)
- test_setAll_is_idempotent
    f.setAll(); f.setAll(); same result
- test_individual_flags_independent
    f.frequency = true; all others still false
- test_resetTracking_not_set_by_setAll
    f.setAll(); assertFalse(f.resetTracking)
- test_multiAlert_not_set_by_setAll
    f.setAll(); assertFalse(f.multiAlert)
- test_cards_not_set_by_setAll
    f.setAll(); assertFalse(f.cards)
- test_setAll_after_partial_sets_remaining
    f.frequency = true; f.setAll(); assertTrue(f.battery) (was false, now true)
```

#### Task 1.4: Create `test/test_display_color_utils/test_display_color_utils.cpp`

Tests for `dimColor()` from `include/display_draw.h` and `ColorThemes::STANDARD()` from `include/color_themes.h`.

```
Requires: A thin shim that includes display_draw.h in a way that
satisfies the display_driver.h dependency. Two approaches:
  (a) Include test/mocks/display_driver.h first → provides Arduino_GFX stubs
  (b) Or just extract dimColor() into a standalone test since it's an inline function

Approach (a) — include mock display_driver.h, then display_draw.h:
  #include "../mocks/Arduino.h"
  #include "../mocks/display_driver.h"
  #include "../../include/display_draw.h"
  #include "../../include/color_themes.h"

Tests to write (~10 tests):
- test_dimColor_full_white_at_100_percent_unchanged
    dimColor(0xFFFF, 100) == 0xFFFF
- test_dimColor_full_white_at_50_percent
    r=31→15, g=63→31, b=31→15 → (15<<11)|(31<<5)|15 = 0x7BEF
- test_dimColor_full_white_at_0_percent_is_black
    dimColor(0xFFFF, 0) == 0x0000
- test_dimColor_black_at_any_percent_is_black
    dimColor(0x0000, 50) == 0x0000
- test_dimColor_pure_red_at_50_percent
    0xF800 → r=31→15 → (15<<11) = 0x7800
- test_dimColor_default_param_is_60_percent
    dimColor(0xFFFF) uses 60% → verify specific value
- test_standard_palette_bg_is_black
    ColorThemes::STANDARD().bg == 0x0000
- test_standard_palette_text_is_white
    ColorThemes::STANDARD().text == 0xFFFF
- test_standard_palette_gray_value
    ColorThemes::STANDARD().colorGray == 0x1082
- test_dimColor_preserves_channel_independence
    pure green (0x07E0) at 50%: g=63→31 → 0x03E0
```

#### Task 1.5: Create `test/test_display_vol_warn/test_display_vol_warn.cpp`

Tests for `VolumeZeroWarning` state machine in `include/display_vol_warn.h`.

```
Requires: Mock millis() (already in test/mocks/Arduino.h — set via mock_millis_value)

Includes:
  #include "../mocks/Arduino.h"
  #include "../../include/display_vol_warn.h"

Provide definition: VolumeZeroWarning volZeroWarn;  (satisfies the extern)

Tests to write (~12 tests):
- test_evaluate_returns_false_when_volume_nonzero
    volZero=false → evaluate returns false
- test_evaluate_returns_false_when_proxy_connected
    volZero=true, proxyConnected=true → false
- test_evaluate_returns_false_when_prequiet_active
    volZero=true, preQuietActive=true → false
- test_evaluate_returns_false_when_speed_vol_zero_active
    volZero=true, speedVolZeroActive=true → false
- test_evaluate_returns_false_during_15s_delay
    set millis=0, evaluate(volZero=true...) → false
    set millis=14999, evaluate → false
- test_evaluate_returns_true_after_15s_delay
    set millis=0, evaluate → false (starts timer)
    set millis=15001, evaluate → true (warning active)
- test_evaluate_calls_beep_on_first_warning
    static bool beeped; playBeepFn sets beeped=true
    step through 15s → verify beeped==true on first true return
- test_evaluate_stops_after_duration_expires
    start at 0, advance to 15001 (warning starts),
    advance to 25001 (15000 delay + 10000 duration) → false, acknowledged=true
- test_reset_clears_all_state
    run through warning cycle, reset(), verify detectedMs/warningStartMs/shown/acknowledged all zero
- test_evaluate_resets_on_proxy_connect_mid_warning
    start warning, then set proxyConnected=true → resets, returns false
- test_needsFlashRedraw_true_during_active_warning
    during active warning window → true
- test_needsFlashRedraw_false_after_acknowledged
    after duration expires → false
```

---

### Phase 2: Recording Mock Canvas (Infrastructure)

**Create an enhanced `Arduino_Canvas` mock that records all draw calls into a queryable log. This enables Phase 3.**

#### Task 2.1: Create `test/mocks/recording_canvas.h`

```cpp
#pragma once
#include "display_driver.h"  // for Arduino_Canvas base
#include <vector>
#include <string>
#include <cstdint>

struct DrawCall {
    enum Type {
        FILL_RECT, DRAW_RECT, FILL_ROUND_RECT, DRAW_ROUND_RECT,
        FILL_CIRCLE, DRAW_CIRCLE, FILL_TRIANGLE, DRAW_LINE,
        DRAW_PIXEL, FILL_SCREEN, SET_TEXT_COLOR, SET_TEXT_SIZE,
        SET_CURSOR, PRINT_STR, FLUSH
    };
    Type type;
    int16_t x, y, w, h, r;
    uint16_t color;
    uint16_t bgColor;
    uint8_t textSize;
    char text[32];
};

class RecordingCanvas : public Arduino_Canvas {
public:
    std::vector<DrawCall> calls;

    RecordingCanvas() : Arduino_Canvas(172, 640, nullptr) {}

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        calls.push_back({DrawCall::FILL_RECT, x, y, w, h, 0, color});
    }
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        calls.push_back({DrawCall::DRAW_RECT, x, y, w, h, 0, color});
    }
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) override {
        calls.push_back({DrawCall::FILL_ROUND_RECT, x, y, w, h, r, color});
    }
    void fillScreen(uint16_t color) override {
        calls.push_back({DrawCall::FILL_SCREEN, 0, 0, 0, 0, 0, color});
    }
    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                       int16_t x2, int16_t y2, uint16_t color) override {
        calls.push_back({DrawCall::FILL_TRIANGLE, x0, y0, x1, y1, 0, color});
        // Store x2,y2 in w,h fields for triangle
    }
    void flush() override {
        calls.push_back({DrawCall::FLUSH});
    }
    // ... (implement all virtual methods similarly)

    // Query helpers:
    int countCalls(DrawCall::Type t) const {
        int n = 0;
        for (auto& c : calls) if (c.type == t) n++;
        return n;
    }
    bool hasCallWithColor(DrawCall::Type t, uint16_t color) const {
        for (auto& c : calls) if (c.type == t && c.color == color) return true;
        return false;
    }
    bool hasCallInRegion(DrawCall::Type t, int16_t minX, int16_t minY, int16_t maxX, int16_t maxY) const {
        for (auto& c : calls) {
            if (c.type == t && c.x >= minX && c.y >= minY && c.x <= maxX && c.y <= maxY) return true;
        }
        return false;
    }
    void clear() { calls.clear(); }
};
```

**Notes:**
- `Arduino_Canvas` virtual methods in `test/mocks/display_driver.h` must be made virtual (they already are)
- `RecordingCanvas` extends `Arduino_Canvas` which extends `Arduino_GFX`
- All draw methods record to `calls` vector then return
- Query helpers let tests assert "a fillRect with color X was drawn in region Y"

#### Task 2.2: Create `test/mocks/mock_settings_for_display.h`

A minimal settings mock that provides default V1Settings with known color values for display tests.

```cpp
#pragma once
#include "../mocks/settings.h"  // existing SettingsManager mock

// Pre-populate with known display color values matching defaults
inline void initDisplayTestSettings(SettingsManager& sm) {
    sm.settings.colorBandL   = 0xF800;  // Red for Laser
    sm.settings.colorBandKa  = 0x07E0;  // Green for Ka
    sm.settings.colorBandK   = 0x001F;  // Blue for K
    sm.settings.colorBandX   = 0xFFE0;  // Yellow for X
    sm.settings.colorVolumeMain = 0x001F;  // Blue
    sm.settings.colorVolumeMute = 0xFFE0;  // Yellow
    sm.settings.colorLockout = 0xF800;     // Red
    sm.settings.brightness = 200;
    sm.settings.activeSlot = 1;
    sm.settings.hideRssiIndicator = false;
    sm.settings.hideVolumeIndicator = false;
}
```

**Note:** The exact field names must match `V1Settings` in `src/settings.h`. Verify field names by reading `src/settings.h` before implementing.

---

### Phase 3: Rendering Logic Tests (Uses Recording Canvas)

**These tests compile the REAL rendering `.cpp` files and call the REAL `V1Display` methods against a `RecordingCanvas`. They verify draw-call correctness, cache behavior, and dirty-flag handling.**

**Key wiring challenge:** Each `display_*.cpp` file includes `display.h` which includes `display_driver.h` (Arduino_GFX). The mock `display_driver.h` already provides the types. The test must:
1. Include mocks first (`Arduino.h`, `display_driver.h`, `settings.h`)
2. Provide extern definitions (`dirty`, `settingsManager`, `volZeroWarn`)
3. Include the real `.cpp` file(s) under test
4. Construct a `V1Display` with a `RecordingCanvas` as its `tft` member

**This requires `V1Display::tft` to be accessible for test injection.** Two options:
- (a) Add `#ifdef UNIT_TEST friend class DisplayRenderingTest;` to `display.h`
- (b) Add a `setCanvas(Arduino_Canvas* c)` method behind `#ifdef UNIT_TEST`

Recommend option (b) — minimal, explicit.

#### Task 3.1: Add test seam to `src/display.h`

```cpp
// At end of V1Display class, before closing brace:
#ifdef UNIT_TEST
public:
    void setTestCanvas(Arduino_Canvas* canvas) { tft.reset(canvas); }
    Arduino_Canvas* getTestCanvas() { return tft.get(); }
#endif
```

**Wait — `tft` is a `std::unique_ptr<Arduino_Canvas>`. Calling `reset()` with a non-heap pointer is UB. Instead:**

```cpp
#ifdef UNIT_TEST
public:
    // For unit tests: replace tft with a non-owning raw pointer wrapper.
    // Test must ensure canvas outlives display.
    Arduino_Canvas* testCanvas_ = nullptr;
    Arduino_Canvas* getCanvas() { return testCanvas_ ? testCanvas_ : tft.get(); }
#endif
```

**Actually, the simplest approach: make the rendering tests NOT construct a full V1Display. Instead, test individual rendering functions by providing the statics they need.**

**Revised approach: Include the `.cpp` file directly (like `test_lockout_enforcer` does) and provide the required globals. The rendering functions access `tft` via the `this->tft` member, so we need a minimal V1Display with an injected canvas.**

**Simplest working approach for `display.h`:**

```cpp
#ifdef UNIT_TEST
public:
    void injectTestCanvas(Arduino_Canvas* canvas);
#endif
```

Implementation in a test helper:
```cpp
void V1Display::injectTestCanvas(Arduino_Canvas* canvas) {
    tft.release();  // Release ownership without deleting
    tft.reset(canvas);  // Take ownership (test must heap-allocate)
}
```

Or even simpler: since the recording canvas is heap-allocated in the test with `new RecordingCanvas()`, `unique_ptr` ownership works fine.

#### Task 3.2: Create `test/test_display_rendering_bands/test_display_rendering_bands.cpp`

Tests for real `display_bands.cpp` rendering.

```
Includes (in order):
  #include "../mocks/Arduino.h"
  #include "../mocks/display_driver.h"
  #include "../mocks/recording_canvas.h"
  #include "../mocks/settings.h"
  #include "../mocks/mock_settings_for_display.h"

Extern definitions:
  SerialClass Serial;
  SettingsManager settingsManager;
  DisplayDirtyFlags dirty;
  // Provide stub for any other externs display_bands.cpp needs

Include real source:
  #include "../../include/display_dirty_flags.h"
  #include "../../include/display_draw.h"
  #include "../../include/display_palette.h"
  #include "../../include/display_layout.h"
  #include "../../src/display_bands.cpp"  // Real code under test

setUp():
  initDisplayTestSettings(settingsManager);
  dirty = DisplayDirtyFlags{};
  // Reset static caches in drawBandIndicators by setting dirty.bands = true
  // Reset millis mock

Tests to write (~12 tests):
- test_drawBandIndicators_ka_active_draws_green_text
    Set up V1Display with RecordingCanvas
    Call drawBandIndicators(BAND_KA, false, 0)
    Verify canvas has text/rect calls with Ka color at expected Y position
- test_drawBandIndicators_all_bands_draws_four_labels
    drawBandIndicators(BAND_LASER|BAND_KA|BAND_K|BAND_X, false, 0)
    Verify 4 distinct label regions drawn
- test_drawBandIndicators_muted_uses_muted_color
    drawBandIndicators(BAND_KA, true, 0)
    Verify color is PALETTE_MUTED_OR_PERSISTED (gray), not green
- test_drawBandIndicators_cache_hit_no_redraw
    Call twice with same args → second call produces zero draw calls
- test_drawBandIndicators_dirty_flag_forces_redraw
    Call once, clear canvas, set dirty.bands=true, call again → draws
- test_drawBandIndicators_flash_bit_hides_band_on_blink_off
    Set millis to trigger blinkOn=false, set flashBits for Ka
    Verify Ka not drawn (effectiveBandMask clears Ka)
- test_drawBandIndicators_no_bands_clears_all_labels
    drawBandIndicators(0, false, 0) → draws background-colored rects
- test_drawVerticalSignalBars_draws_correct_bar_count
    drawVerticalSignalBars(4, 2, false) → verify bar regions drawn
- test_drawVerticalSignalBars_muted_uses_gray
    drawVerticalSignalBars(4, 2, true) → verify muted color
- test_drawVerticalSignalBars_zero_strength_draws_outline_only
    drawVerticalSignalBars(0, 0, false) → only outlines
- test_drawVerticalSignalBars_cache_hit_no_redraw
    Same args twice → no draw calls on second
- test_drawVerticalSignalBars_dirty_flag_forces_redraw
    Set dirty.signalBars=true → forces redraw
```

#### Task 3.3: Create `test/test_display_rendering_arrow/test_display_rendering_arrow.cpp`

Same structure as 3.2 but for `display_arrow.cpp`.

```
Tests to write (~10 tests):
- test_drawDirectionArrow_front_draws_upward_triangle
    dir=DIR_FRONT → verify fillTriangle call with upward-pointing coords
- test_drawDirectionArrow_rear_draws_downward_triangle
    dir=DIR_REAR → verify triangle points down
- test_drawDirectionArrow_all_three_draws_three_shapes
    dir=DIR_FRONT|DIR_SIDE|DIR_REAR → three distinct triangle/rect regions
- test_drawDirectionArrow_muted_uses_gray
    muted=true → verify all shapes use muted color
- test_drawDirectionArrow_cache_hit_no_redraw
    Same args twice → no draw calls on second
- test_drawDirectionArrow_dirty_flag_forces_redraw
    dirty.arrow=true → forces redraw
- test_drawDirectionArrow_blink_off_hides_flashing_arrow
    flashBits=0x20 (front), blink OFF → front not drawn
- test_drawDirectionArrow_blink_on_shows_flashing_arrow
    flashBits=0x20 (front), blink ON → front drawn
- test_drawDirectionArrow_front_color_override
    frontColorOverride != 0 → front uses override color
- test_drawDirectionArrow_no_direction_clears_area
    dir=DIR_NONE → clear rects drawn in arrow region
```

#### Task 3.4: Create `test/test_display_rendering_indicators/test_display_rendering_indicators.cpp`

Tests for `display_indicators.cpp` (lockout, GPS, OBD badges).

```
Tests to write (~10 tests):
- test_drawLockoutIndicator_shown_draws_badge
    lockoutIndicatorShown_=true → fillRoundRect + text "L"
- test_drawLockoutIndicator_hidden_clears_area
    lockoutIndicatorShown_=false → fillRect with BG color at badge position
- test_drawLockoutIndicator_cache_hit_no_redraw
    Same state twice → no draw calls on second
- test_drawLockoutIndicator_dirty_flag_forces_redraw
    dirty.lockout=true → forces redraw even if state unchanged
- test_drawLockoutIndicator_uses_settings_color
    Set colorLockout to specific value → verify badge color matches
- test_drawGpsIndicator_with_fix_shows_green
    Set gps fix=true, satellites=8 → verify green-ish indicator
- test_drawGpsIndicator_no_fix_shows_gray
    Set gps fix=false → verify gray indicator
- test_drawObdIndicator_connected_shows_active
    Set obd connected=true → verify active indicator drawn
- test_drawObdIndicator_attention_shows_flash
    Set obd attention=true → verify distinct flash state
- test_drawBaseFrame_fills_screen_and_sets_all_dirty
    drawBaseFrame() → fillScreen(BG) + dirty.setAll() called
```

#### Task 3.5: Create `test/test_display_rendering_status_bar/test_display_rendering_status_bar.cpp`

Tests for battery, RSSI, volume, profile indicators in `display_status_bar.cpp`.

```
Tests to write (~12 tests):
- test_drawBatteryIndicator_full_charge_green
    batteryPercent=100 → verify green color region
- test_drawBatteryIndicator_low_charge_red
    batteryPercent=10 → verify red color
- test_drawBatteryIndicator_usb_power_threshold
    ADC value in USB hysteresis window (4095-4125) → verify USB icon
- test_drawRssiIndicator_strong_signal_green
    rssi=-60 → verify green color (above -75 threshold)
- test_drawRssiIndicator_weak_signal_red
    rssi=-95 → verify red color (below -90 threshold)
- test_drawRssiIndicator_hidden_when_setting_off
    hideRssiIndicator=true → only BG fill, no signal drawn
- test_drawRssiIndicator_stale_ble_clears_area
    hasFreshBleContext returns false → clears RSSI area
- test_drawVolumeIndicator_draws_main_and_mute
    mainVol=5, muteVol=0 → verify "5V" and "0M" text drawn
- test_drawProfileIndicator_draws_slot_number
    slot=2 → verify text or segment for "2"
- test_drawProfileIndicator_flash_period_timing
    Verify profile flash expires after configured duration
- test_drawWiFiIndicator_draws_when_connected
    WiFi setup mode active → verify WiFi icon drawn
- test_drawBLEProxyIndicator_draws_phone_icon_when_connected
    proxy connected → verify BLE proxy icon region drawn
```

---

### Phase 4: Update CI Contract (After All Tests Pass)

#### Task 4.1: Update `docs/TEST_BASELINE.md`

Add the new test count to the baseline.

#### Task 4.2: Verify `scripts/ci-test.sh` passes

Run `python3 scripts/run_native_tests_serial.py` to confirm all new suites build and pass on native.

---

### Execution Order & Dependencies

```
Phase 1 (independent, parallelizable):
  Task 1.1  test_display_segments         ← pure, no deps
  Task 1.2  test_display_slider_math      ← pure, no deps
  Task 1.3  test_display_dirty_flags      ← pure, no deps
  Task 1.4  test_display_color_utils      ← needs mock display_driver.h (exists)
  Task 1.5  test_display_vol_warn         ← needs mock Arduino.h millis (exists)

Phase 2 (infrastructure, serial):
  Task 2.1  recording_canvas.h            ← new mock file
  Task 2.2  mock_settings_for_display.h   ← new mock helper

Phase 3 (depends on Phase 2, parallelizable):
  Task 3.1  display.h test seam           ← one-line change
  Task 3.2  test_display_rendering_bands  ← depends on 2.1, 2.2, 3.1
  Task 3.3  test_display_rendering_arrow  ← depends on 2.1, 2.2, 3.1
  Task 3.4  test_display_rendering_indicators ← depends on 2.1, 2.2, 3.1
  Task 3.5  test_display_rendering_status_bar ← depends on 2.1, 2.2, 3.1

Phase 4 (after all pass):
  Task 4.1  Update TEST_BASELINE.md
  Task 4.2  Verify ci-test.sh
```

### Expected Test Count Increase

| Phase | New Tests | Running Total |
|---|---:|---:|
| Phase 1 (pure logic) | ~55 | 1,352 |
| Phase 3 (rendering) | ~44 | 1,396 |
| **Total new** | **~99** | |

### Key Risks & Mitigations

1. **Risk:** `display_bands.cpp` includes `settings.h` for `V1Settings` colors → mock `settings.h` must have all color fields.
   **Mitigation:** Read `src/settings.h` to get exact `V1Settings` field names before writing `mock_settings_for_display.h`.

2. **Risk:** Rendering functions use file-scoped `static` variables for caching. These persist across tests in the same suite.
   **Mitigation:** Each test must set `dirty.bands = true` (etc.) to force cache invalidation, or the test file must use a fresh process (one test per process via `run_native_tests_serial.py`).

3. **Risk:** Some rendering functions call `settingsManager.get()` → mock must return valid `V1Settings`.
   **Mitigation:** `initDisplayTestSettings()` sets all display-relevant fields.

4. **Risk:** `display_cards.cpp` uses `settingsManager.getSlotAlertPersistSec()` → mock must implement this.
   **Mitigation:** Verify mock `settings.h` has this method, or add it.

5. **Risk:** Some files include `battery_manager.h`, `wifi_manager.h`, `perf_metrics.h` → need stubs.
   **Mitigation:** Check each rendering file's `#include` list. Create minimal stubs for any missing headers. `perf_metrics.h` likely has `PERF_INC()` → already `#ifdef`'d to no-op under `UNIT_TEST`.

---

*This plan is research-only. No changes were made to the codebase.*

---

# Appendix B: Codebase Consistency & Normalization Plan

**Date:** March 30, 2026
**Scope:** File organization, naming conventions, formatting, comment style, and include hygiene across `src/` and `include/`
**Goal:** Make the repo look and feel like a single author wrote it — continuous, uniform conventions from top to bottom

---

## Current State Summary

The codebase is architecturally strong but visually inconsistent in ways that signal "first project" rather than "showcase." The code reads like it was written in phases — earlier files use one convention, later files another — and no automated formatter enforces a baseline. None of these issues affect runtime behavior, but they undermine the professional, bulletproof impression the architecture actually deserves.

**Key findings across five audit dimensions:**

| Dimension | Finding | Severity |
|---|---|---|
| Header guards | 118 files use `#pragma once`, 43 use `#ifndef` | Medium |
| Private members | 11 classes (~150+ members) lack trailing underscore | High |
| Section separators | 8–10 distinct visual separator styles | Low |
| Log prefixes | 90% use `[Tag]` format; 6% use bare `TAG:` | Low |
| Trailing whitespace | 1,207 lines across 55 files | Low |
| Formatting config | No `.clang-format` or `.editorconfig` | High |
| Header placement | Headers split across `include/`, `src/`, and orphaned `src/include/` | Medium |
| File header comments | Most files have no file-level docblock; a few have verbose ones | Low |

---

## Phase 1: Automated Formatting Baseline (Zero-Risk)

These changes are mechanical, touch no logic, and can be validated by compilation alone.

### Task 1.1: Add `.editorconfig`

Create `.editorconfig` at repo root:

```ini
root = true

[*]
indent_style = space
indent_size = 4
end_of_line = lf
charset = utf-8
trim_trailing_whitespace = true
insert_final_newline = true

[*.{h,cpp}]
indent_size = 4

[*.{json,yml,yaml}]
indent_size = 2

[Makefile]
indent_style = tab
```

**Files:** 1 new file
**Risk:** None — editor config only

### Task 1.2: Add `.clang-format`

Create `.clang-format` at repo root. Match the existing dominant style rather than imposing a new one:

```yaml
BasedOnStyle: LLVM
IndentWidth: 4
TabWidth: 4
UseTab: Never
ColumnLimit: 120
BreakBeforeBraces: Attach
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: false
AllowShortLoopsOnASingleLine: false
SortIncludes: false
PointerAlignment: Left
SpaceAfterCStyleCast: false
SpaceBeforeParens: ControlStatements
```

`SortIncludes: false` is critical — reordering includes on ESP32 can break builds due to Arduino.h dependency ordering.

**Files:** 1 new file
**Risk:** None until explicitly applied. Add to repo but do NOT run formatter repo-wide in this phase.

### Task 1.3: Strip trailing whitespace

```bash
find src/ include/ -name '*.h' -o -name '*.cpp' | xargs sed -i 's/[[:blank:]]*$//'
```

**Files:** ~55 files, 1,207 lines trimmed
**Risk:** Whitespace-only diff. Verify with `pio run -e native`.

---

## Phase 2: Header Guard Standardization

### Task 2.1: Convert 43 `#ifndef` files to `#pragma once`

The project already uses `#pragma once` in 73% of headers (118/161). Both GCC and Clang support it on all target platforms (ESP32-S3 via xtensa-gcc, native via host GCC/Clang). Standardize on `#pragma once` for uniformity and to eliminate stale guard name bugs.

**Files to convert (43 total):**

`include/` (24 files): `config.h`, `wifi_manager_internals.h`, `main_loop_phases.h`, `main_globals.h`, `main_internals.h`, `display_vol_warn.h`, `ble_internals.h`, `audio_internals.h`, `audio_i2c_utils.h`, `settings_internals.h`, `display_flush.h`, `main_runtime_state.h`, `audio_task_utils.h`, `v1simple_logo.h`, `settings_namespace_ids.h`, `wifi_rate_limiter.h`, `display_log.h`, `display_layout.h`, `display_driver.h`, `display_ble_context.h`, `psram_alloc_compat.h`, `band_utils.h`, `color_themes.h`, `FreeSansBold24pt7b.h`

`src/` (19 files): `ble_client.h`, `wifi_manager.h`, `modules/obd/obd_ble_client.h`, `packet_parser.h`, `perf_metrics.h`, `display.h`, `settings.h`, `modules/quiet/quiet_coordinator_templates.h`, `settings_runtime_sync.h`, `battery_manager.h`, `storage_manager.h`, `touch_handler.h`, `perf_sd_logger.h`, `v1_devices.h`, `ble_fresh_flash_policy.h`, `v1_profiles.h`, `modules/wifi/wifi_api_response.h`, `time_service.h`, `settings_sanitize.h`

**Conversion pattern per file:**
```cpp
// Remove:
#ifndef SOME_HEADER_H
#define SOME_HEADER_H
// ... (and matching #endif at bottom)

// Replace with:
#pragma once
```

**Risk:** Low. Compilation validates correctness.

### Task 2.2: Update test mock headers

`test/mocks/display_driver.h` currently uses `#ifndef DISPLAY_DRIVER_H`. Convert to `#pragma once` to match.

**Files:** 1 file
**Risk:** None

---

## Phase 3: Private Member Naming Normalization

This is the highest-value consistency fix. The project convention (per `ARCHITECTURE.md`) is trailing underscore for private members. 11 classes violate this with ~150+ bare members. Newer files already comply — this is purely catching up older code.

### Task 3.1: Normalize `V1BLEClient` (~40 members)

**File:** `src/ble_client.h` (and all `.cpp` files that access these members)
**Pattern:** `proxyEnabled` → `proxyEnabled_`, `lastScanStart` → `lastScanStart_`, etc.
**Affected .cpp files:** `ble_client.cpp`, `ble_connection.cpp`, `ble_commands.cpp`, `ble_runtime.cpp`, `ble_proxy.cpp`

### Task 3.2: Normalize `V1Display` (~25 members)

**File:** `src/display.h` (and `src/display*.cpp` files)
**Pattern:** `currentProfileSlot` → `currentProfileSlot_`, `currentScreen` → `currentScreen_`, etc.

### Task 3.3: Normalize `WiFiManager` (~20 members)

**File:** `src/wifi_manager.h` (and `src/wifi_manager*.cpp`, `src/wifi_client*.cpp`)
**Pattern:** `setupModeState` → `setupModeState_`, `apInterfaceEnabled` → `apInterfaceEnabled_`, etc.

### Task 3.4: Normalize `BatteryManager` (10 members)

**File:** `src/battery_manager.h` (and `src/battery_manager.cpp`)

### Task 3.5: Normalize `TouchHandler` (11 members)

**File:** `src/touch_handler.h` (and `src/touch_handler.cpp`)

### Task 3.6: Normalize `PacketParser` (7 members)

**File:** `src/packet_parser.h` (and `src/packet_parser.cpp`)

### Task 3.7: Normalize `PerfSdLogger` (10 members)

**File:** `src/perf_sd_logger.h` (and `src/perf_sd_logger.cpp`)

### Task 3.8: Normalize remaining 4 classes

**Files:** `settings.h` (3 members), `storage_manager.h` (4 members), `v1_devices.h` (3 members), `v1_profiles.h` (4 members)

**Risk:** Medium — find-and-replace must be scoped to the class. Use IDE rename-symbol or careful `sed` with word boundaries. Validate with `pio run -e native && pio test -e native`.

**Execution order:** One class per commit. Run full native test suite after each.

---

## Phase 4: Section Separator Standardization

The codebase uses 8–10 different visual separator styles. Standardize on a two-tier hierarchy that matches the dominant existing pattern:

### Chosen standard

**Major section** (top-level groupings like "Public API", "Private Implementation"):
```cpp
// ============================================================================
// Section Name
// ============================================================================
```

**Minor section** (subsections within a group):
```cpp
// --- Subsection Name ---
```

No other separator styles. No bare `// ---`, no `// -----`, no `// ==========`, no `// ---- Name ----` with varying dash counts.

### Task 4.1: Normalize separator styles

**Scope:** All `.h` and `.cpp` files in `src/` and `include/`
**Count:** ~300+ separators across ~296 files
**Risk:** Zero — comments only. Validation: compilation.

---

## Phase 5: Log Prefix Standardization

The `[Tag]` bracketed format already dominates (90%). Normalize the remaining 10%.

### Chosen standard

```
[Tag] message                    // Normal
[Tag] ERROR: message             // Error
[Tag] WARN: message              // Warning
```

Rules:
- Tag is PascalCase, matching the module/subsystem name
- Severity keyword (`ERROR:`, `WARN:`, `CRITICAL:`) follows the tag, not replaces it
- No bare `ERROR:` or `WARN:` without a bracketed tag
- Crash recovery messages (`!!! CRASH RECOVERY !!!`) are exempt — they are intentionally distinctive

### Task 5.1: Audit and fix bare severity prefixes

**Scope:** ~33 occurrences using `ERROR:`, `WARN:`, `WARNING:` without a `[Tag]` prefix
**Fix:** Add the appropriate `[Tag]` for the file's module
**Risk:** Zero — log string changes only

### Task 5.2: Normalize `WARNING:` → `WARN:`

Standardize on 4-char severity: `WARN:` not `WARNING:` (13 occurrences to fix).

---

## Phase 6: Header Placement Cleanup

### Task 6.1: Relocate orphaned `src/include/obd_ble_arbitration.h`

This is the only file in `src/include/`. It should move to `src/modules/obd/obd_ble_arbitration.h` to sit with its sibling OBD module headers.

**Fix:** Move file, update `#include` paths in any consumers.
**Risk:** Low — compiler catches broken includes immediately.

### Task 6.2: Establish header placement rule

Document in `ARCHITECTURE.md`:
- `include/` — headers consumed by multiple subsystems or needed by tests
- `src/*.h` — private headers for a single `.cpp` translation unit
- `src/modules/<category>/` — module-scoped headers

No `src/include/` directory.

---

## Phase 7: File Header Comment Standard (Optional)

Currently ~80% of files have no file-level docblock. The few that do are inconsistent. Two options:

**Option A (Recommended):** No file-level docblocks. The filename and `ARCHITECTURE.md` provide sufficient context. Remove the few existing verbose headers for consistency.

**Option B:** Add a minimal one-liner to every file:
```cpp
/// @file display_cards.cpp — Secondary alert card rendering and persistence tracking
```

This phase is low priority and can be deferred.

---

## Execution Order and Dependencies

| Order | Phase | Tasks | Risk | Validation |
|---|---|---|---|---|
| 1 | Phase 1 | 1.1, 1.2, 1.3 | Zero | `pio run -e native` |
| 2 | Phase 2 | 2.1, 2.2 | Low | `pio run -e native && pio test -e native` |
| 3 | Phase 3 | 3.1–3.8 | Medium | Full test suite after each class (one commit per class) |
| 4 | Phase 4 | 4.1 | Zero | `pio run -e native` |
| 5 | Phase 5 | 5.1, 5.2 | Zero | `pio run -e native` |
| 6 | Phase 6 | 6.1, 6.2 | Low | `pio run -e native` |
| 7 | Phase 7 | Optional | Zero | Visual review |

**Total estimated file touches:** ~200+ files across all phases
**Phase 3 is the most impactful and the most delicate** — do it one class at a time with full test validation between each.

---

## CI Guard Recommendation

After all phases complete, add a CI step to prevent drift:

```bash
# In scripts/ci-test.sh, add after existing guards:

# --- Consistency guards ---
echo "=== Trailing whitespace check ==="
if grep -rn '[[:blank:]]$' --include='*.h' --include='*.cpp' src/ include/; then
    echo "FAIL: trailing whitespace found"
    exit 1
fi

echo "=== #ifndef header guard check ==="
if grep -rn '#ifndef.*_H$' --include='*.h' src/ include/ | grep -v 'test/'; then
    echo "FAIL: #ifndef header guard found (use #pragma once)"
    exit 1
fi
```

---

*This plan is research-only. No changes were made to the codebase.*

---

# Appendix C: Documentation Correction — AMOLED → LCD

**Date:** March 30, 2026
**Scope:** All references to "AMOLED" in docs, source comments, and headers
**Issue:** The Waveshare ESP32-S3-Touch-LCD-3.49 uses an LCD panel, not AMOLED. Multiple files incorrectly describe the display as AMOLED.

---

## Affected Files (6 total)

### 1. `docs/MANUAL.md` — Line 9

**Current:**
```
**Hardware:** Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B, 640×172 AMOLED)
```

**Fix:**
```
**Hardware:** Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B, 640×172 LCD)
```

### 2. `docs/MANUAL.md` — Line 214

**Current:**
```
| Display | AXS15231B QSPI AMOLED, 640×172 pixels |
```

**Fix:**
```
| Display | AXS15231B QSPI LCD, 640×172 pixels |
```

### 3. `src/main.cpp` — Line 8

**Current:**
```cpp
 * - 3.49" AMOLED display with touch support
```

**Fix:**
```cpp
 * - 3.49" LCD display with touch support
```

### 4. `include/display_layout.h` — Line 3

**Current:**
```cpp
 * Waveshare ESP32-S3-Touch-LCD-3.49 (640x172 AMOLED)
```

**Fix:**
```cpp
 * Waveshare ESP32-S3-Touch-LCD-3.49 (640x172 LCD)
```

### 5. `src/display.h` — Line 3

**Current:**
```cpp
 * Target: Waveshare ESP32-S3-Touch-LCD-3.49 (172x640 AMOLED, AXS15231B)
```

**Fix:**
```cpp
 * Target: Waveshare ESP32-S3-Touch-LCD-3.49 (172x640 LCD, AXS15231B)
```

### 6. `docs/REPO_REVIEW.md` — Line 79

**Current:**
```
Not likely noticeable on AMOLED, but worth documenting.
```

**Fix:**
```
Not likely noticeable on LCD, but worth documenting.
```

---

## Verification

After applying all fixes, confirm no remaining references:

```bash
grep -rn 'AMOLED\|amoled' --include='*.h' --include='*.cpp' --include='*.md' src/ include/ docs/
```

Expected result: no matches (aside from this appendix itself).

---

*All fixes listed above have been applied. No remaining AMOLED references exist outside this appendix.*

---

# Appendix D: Confirmed Issues — BLE Thread Safety, Display Performance, Display Palette Coupling

**Date:** March 31, 2026
**Scope:** Three issues confirmed through code review with specific reproduction conditions and fix plans
**Priority:** Issues 1 and 2 are ship-blocking. Issue 3 is build/testability improvement.

---

## Issue 1 — BLE Thread Safety (High Priority, Ship-Blocking)

### Problem

`cleanupConnection()` in `ble_client.cpp` (~line 355) has a use-after-free window between clearing characteristic pointers (step 3) and setting `connected_` to `false` (step 4). A BLE notification callback running between these two steps sees `connected_ == true`, proceeds to dereference the now-null characteristic pointer, and crashes.

The stores to `notifyShortChar_`/`notifyLongChar_` use `memory_order_relaxed`, which provides no cross-thread ordering guarantee. The callback thread can observe `connected_ == true` while the characteristic pointer has already been nulled.

Additionally, the callback setters (`onDataReceived`, `onV1ConnectImmediate`, `onV1Connected`, ~line 697) assign plain `std::function` objects with no synchronization while callbacks can fire from the BLE task. This is a data race per the C++ memory model.

### Fix Plan

**Step 1: Move characteristic clears inside the mutex guard**

The characteristic pointer nulling (step 3) must happen inside the `bleMutex_` lock that already protects step 4. This eliminates the TOCTOU window — the callback cannot observe a state where `connected_` is true but characteristic pointers are null.

```
Current order in cleanupConnection():
  step 1: unsubscribe notifications
  step 2: disconnect
  step 3: notifyShortChar_.store(nullptr, relaxed)   ← OUTSIDE mutex
           notifyLongChar_.store(nullptr, relaxed)
  step 4: lock(bleMutex_); connected_ = false; ...   ← INSIDE mutex

Fixed order:
  step 1: unsubscribe notifications
  step 2: disconnect
  step 3: lock(bleMutex_) {
             notifyShortChar_.store(nullptr, release)
             notifyLongChar_.store(nullptr, release)
             connected_ = false
             ...remaining cleanup
           }
```

**Step 2: Fix memory ordering on atomic characteristic pointers**

Change `notifyShortChar_` and `notifyLongChar_` stores to `memory_order_release`. Change corresponding loads in the notification callback to `memory_order_acquire`. This ensures the callback thread sees the nullptr before or after the store, never a torn intermediate state, and that all preceding writes (the unsubscribe) are visible.

**Step 3: Protect callback registration with bleMutex_**

Wrap assignments to `onDataReceived`, `onV1ConnectImmediate`, and `onV1Connected` in a `bleMutex_` lock. The BLE notification path already acquires `bleMutex_` (or should, after step 1), so this creates a happens-before relationship between registration and invocation.

**Validation:**
- BLE reconnect stress test: 100 connect/disconnect cycles at 500ms intervals
- ASan/TSan native build to catch remaining data races
- Verify no regression in BLE connection latency (P99 should remain under 2s)

**Files touched:** `src/ble_client.cpp`, `src/ble_client.h` (if memory order constants need updating)

---

## Issue 2 — Display Performance (Medium-High Priority)

### Problem

`flushRegion()` in `src/display.cpp` calls `draw16bitRGBBitmap(x, y + row, rowPtr, w, 1)` once per row. On the 640x172 LCD over QSPI, a full-height flush issues 172 separate draw calls, each with bus-setup overhead (chip select, command header, address window configuration).

When the flush region spans the full display width (`w == stride`), the framebuffer rows are contiguous in memory and a single `draw16bitRGBBitmap(x, y, fb + y * stride + x, w, h)` call would work. The current code never takes this fast path.

### Fix Plan

**Step 1: Add a contiguous-buffer fast path in flushRegion()**

Before the existing row-by-row loop, check whether the region is contiguous:

```cpp
void V1Display::flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (w <= 0 || h <= 0) return;

    uint16_t* fb = getFramebuffer();
    const int16_t stride = getStride();  // full display width (172)

    // Fast path: when region width equals stride, rows are contiguous
    if (w == stride) {
        uint16_t* regionStart = fb + y * stride + x;
        tft->draw16bitRGBBitmap(x, y, regionStart, w, h);
        return;
    }

    // Slow path: non-contiguous rows, flush row by row (existing code)
    for (int16_t row = 0; row < h; row++) {
        uint16_t* rowPtr = fb + (y + row) * stride + x;
        tft->draw16bitRGBBitmap(x, y + row, rowPtr, w, 1);
    }
}
```

**Step 2: Add a perf counter for flush call count**

Add `PERF_INC(displayFlushCalls)` inside the row-by-row loop and a separate `PERF_INC(displayFlushBatch)` for the fast path. This lets the SLO scorer verify the optimization is actually firing.

**Step 3: Measure before/after**

Use the existing perf CSV pipeline to capture display flush timing across 60 seconds of active alerting. Expected improvement: ~170x fewer QSPI transactions for full-screen redraws, measurable as reduced `displayFlushMs` P99.

**Validation:**
- Perf CSV comparison: flush call count and display update latency
- Visual inspection: no tearing or rendering artifacts on hardware
- Compile check: `pio run -e waveshare-349`

**Files touched:** `src/display.cpp` (flushRegion), `include/perf_metrics.h` (new counter)

---

## Issue 3 — Global State / display_palette.h Coupling (Medium Priority)

### Problem

`include/display_palette.h` includes `settings.h` (1,038 lines) solely to support two macros that call `settingsManager.get()` for `colorMuted` and `colorPersisted` values. Every display sub-module (`display_arrow.cpp`, `display_text.cpp`, `display_bands.cpp`, etc.) that includes `display_palette.h` transitively pulls in the entire settings translation unit.

This creates tight coupling between the display rendering layer and the settings system: any change to `settings.h` triggers a rebuild of all display files, display modules cannot be tested without a full settings mock, and the pattern is inconsistent with the dependency-injection approach used throughout the rest of the codebase.

### Fix Plan

**Step 1: Add color fields to the existing ColorPalette struct**

The `ColorPalette` struct (in `include/color_themes.h`) already carries per-theme color values. Add two fields:

```cpp
struct ColorPalette {
    // ... existing fields ...
    uint16_t colorMuted;
    uint16_t colorPersisted;
};
```

**Step 2: Populate the new fields at palette construction time**

In the code that builds `ColorPalette` (likely in the display orchestration module or at display `begin()`), read the two values from settings once and inject them:

```cpp
ColorPalette palette = ColorThemes::STANDARD();
palette.colorMuted = settingsManager.get().colorMuted;
palette.colorPersisted = settingsManager.get().colorPersisted;
```

**Step 3: Remove the settings.h include from display_palette.h**

Replace the two macros (`PALETTE_MUTED_COLOR`, `PALETTE_PERSISTED_COLOR`) with references to the palette parameter that rendering functions already receive. Each rendering function that currently calls the macro will instead read `palette.colorMuted` or `palette.colorPersisted`.

**Step 4: Verify build isolation**

After the change, confirm that modifying `settings.h` does NOT trigger recompilation of any `display_*.cpp` file (check with `pio run -e native -v` and inspect the compiler commands).

**Validation:**
- `pio run -e native && pio test -e native` — full build and test pass
- Incremental build test: touch `src/settings.h`, rebuild, verify zero display files recompile
- Display rendering tests (Appendix A Phase 3) should pass without a settings mock

**Files touched:** `include/color_themes.h` (add fields), `include/display_palette.h` (remove settings.h include, remove macros), display orchestration code (populate new fields), each `display_*.cpp` that uses the macros (~6-8 files)

---

*This appendix is research-only. No changes were made to the codebase.*

---

# Appendix E: Full Repo Audit — Corrective and Normalizing Work

**Date:** March 31, 2026
**Scope:** Five-axis review covering BLE, display/settings, WiFi/storage/main loop, test suite/CI, and include hygiene/build config
**Method:** Parallel deep audit of all subsystems, cross-referenced against existing REPO_REVIEW.md to report only NEW findings

---

## 1. BLE Subsystem — New Findings

### 1.1 Second Double-Backslash in Log Format String (Low)

**File:** `src/ble_connection.cpp`, line 742
**Issue:** `Serial.printf("[BLE] Conn params (%s): interval=%.2f ms latency=%u\\n", ...)` prints literal `\n` instead of a newline. This is a second instance beyond the one already documented in `packet_parser.cpp:175`.
**Fix:** Change `\\n` to `\n`.

### 1.2 TOCTOU Race on pCommandCharLong_ (Medium)

**File:** `src/ble_proxy.cpp`, lines 677–695
**Issue:** `pCommandCharLong_` is checked for null then dereferenced without holding `bleMutex_`. A concurrent `cleanupConnection()` call (which clears the pointer under the mutex) could set it to null between the check and the `writeValue()` dereference.
**Fix:** Either snapshot the pointer under `bleMutex_` before use, or hold the mutex across the check-and-dereference sequence.

### 1.3 Magic Number 0x2902 — CCCD Descriptor UUID (Low)

**File:** `src/ble_connection.cpp`, lines 666 and 705
**Issue:** The standard BLE Client Characteristic Configuration Descriptor UUID `0x2902` appears as a bare magic number in two places.
**Fix:** Add `#define V1_CCCD_DESCRIPTOR_UUID ((uint16_t)0x2902)` to `include/config.h` and reference it.

---

## 2. Display Subsystem — New Findings

### 2.1 Integer Overflow Risk in Dirty Region Union (Medium)

**File:** `src/display_frequency.cpp`, lines 58–59
**Issue:** The dirty region union computes `frequencyDirtyX_ + frequencyDirtyW_` on `int16_t` before any widening. If the sum exceeds 32,767, it overflows. The 640px screen width makes this unlikely today but the code is mathematically unsound and fragile against future display hardware.
**Fix:** Use `int32_t` intermediate calculations for coordinate arithmetic in region union logic.

### 2.2 Unvalidated RGB565 Color Values from NVS (Low)

**File:** `src/settings.cpp`, lines 222–246
**Issue:** 22 color settings fields (band colors, volume colors, RSSI colors, etc.) are loaded from NVS with no validation that the stored value is a valid RGB565 value. Corrupted NVS entries could produce garbage colors.
**Fix:** Mask loaded color values with `value & 0xFFFF` after NVS read (or add a `sanitizeColor()` pass in the existing settings sanitization pipeline).

### 2.3 Missing Font Error Handling / OFR Fallback (Medium)

**File:** `src/display_font_manager.cpp`, `src/display.cpp`
**Issue:** OpenFontRender glyph rasterization has no fallback if it fails under memory pressure. The system continues with missing glyphs rather than degrading to a monospace bitmap font. Already noted in Section 4 of the review but no fix plan existed.
**Fix:** Add a font degradation strategy — detect rasterization failure (null glyph return) and fall back to a built-in bitmap font for that frame.

### 2.4 Scattered Static Render Caches Not Reset on resetChangeTracking() (Low)

**Files:** `src/display_bands.cpp`, `src/display_arrow.cpp`, `src/display_indicators.cpp`, `src/display_status_bar.cpp`
**Issue:** File-scoped `static` variables that cache last-drawn state (e.g., `lastBandMask`, `lastArrowDir`) are only reset via the dirty flag system, not by `resetChangeTracking()`. If a new reset path is added that doesn't set all dirty flags, stale caches could prevent redraws.
**Fix:** Add a `resetAllRenderCaches()` function called from `resetChangeTracking()` that explicitly resets all file-scoped statics, or migrate the caches into the `V1Display` class so they're tied to object lifetime.

---

## 3. WiFi / Storage / Main Loop — New Findings

No new issues beyond those already documented in the main review and Appendix D. The WiFi state machine, storage pipeline, and main loop ordering are all sound. The `static JsonDocument doc` fix in `main_persist.cpp` is confirmed applied.

---

## 4. Test Suite and CI — New Findings

### 4.1 Trivial Assertion in Placeholder Test (Medium)

**File:** `test/test_drive_replay/test_drive_replay.cpp`, line 136
**Issue:** `TEST_ASSERT_TRUE(true)` — this test always passes and provides no value. It's a scaffold for CSV/JSON fixture loading that was never completed.
**Fix:** Either complete the replay fixture loading implementation or remove the placeholder test. A test that can't fail is worse than no test — it inflates the count and creates false confidence.

### 4.2 Replay Test Suite Not Integrated into CI (Medium)

**File:** `scripts/ci-test.sh`, `test/test_drive_replay/`
**Issue:** The replay test environment (`native-replay`) is configured in `platformio.ini` and the scaffold exists, but it's not wired into the CI gate. Captured-log regression tests don't run on every PR.
**Fix:** Once the replay fixture loading is complete (4.1), add a `run_replay_suite.py` invocation to `ci-test.sh`.

### 4.3 Missing Semantic Guards (Medium)

**File:** `scripts/`
**Issue:** No CI guard enforces const-correctness violations in module headers or mutable-state escape (e.g., a module accidentally adding an `extern` global). The existing semantic guards cover BLE deletion safety, display flush discipline, SD lock semantics, and main loop call order, but not these two patterns.
**Fix:** Add two new Python semantic guards:
1. `check_module_const_correctness.py` — scan `src/modules/**/*.h` for non-const global-scope declarations
2. `check_extern_escape.py` — scan `src/modules/**/*.h` for `extern` declarations (which violate the DI architecture)

### 4.4 Copy-Paste Tests Not Parameterized (Low)

**Files:** `test/test_audio/test_audio.cpp` (lines 125–293), `test/test_display_color_utils/test_display_color_utils.cpp`
**Issue:** Enum value tests and boundary tests repeat the same assertion pattern for different inputs (e.g., `test_band_enum_values`, `test_direction_enum_values`, `test_voice_mode_enum_values`). These could use a data-driven approach.
**Fix:** Extract to helper loops or use Unity parameterized test macros. Lower priority — correctness is fine, this is maintainability.

### 4.5 Mutation Catalog Limited to 10 Critical Mutations (Low)

**File:** `test/mutations/critical_mutations.json`
**Issue:** Only 10 critical mutations are tracked. The "full" mode resolves to the same 9 items. Secondary mutations (bounds off-by-one, sign errors in non-critical paths) are not systematically tested.
**Fix:** Add 5–10 secondary mutations covering display coordinate math, WiFi timeout comparisons, and settings range validation.

---

## 5. Include Hygiene and Build Config — New Findings

### 5.1 Redundant Dual Header Guards — 43 Files (Medium)

**Files:** 43 headers across `src/` and `include/`
**Issue:** These files have both `#pragma once` AND `#ifndef`/`#define`/`#endif` guards. The `#ifndef` guard is redundant now that `#pragma once` is present. This is leftover from the Phase 2 normalization — `#pragma once` was added but the old guard was not removed.
**Affected files (all 43):**

`include/` (24): `config.h`, `wifi_manager_internals.h`, `main_loop_phases.h`, `main_globals.h`, `main_internals.h`, `display_vol_warn.h`, `ble_internals.h`, `audio_internals.h`, `audio_i2c_utils.h`, `settings_internals.h`, `display_flush.h`, `main_runtime_state.h`, `audio_task_utils.h`, `v1simple_logo.h`, `settings_namespace_ids.h`, `wifi_rate_limiter.h`, `display_log.h`, `display_layout.h`, `display_driver.h`, `display_ble_context.h`, `psram_alloc_compat.h`, `band_utils.h`, `color_themes.h`, `FreeSansBold24pt7b.h`

`src/` (19): `ble_client.h`, `wifi_manager.h`, `modules/obd/obd_ble_client.h`, `packet_parser.h`, `perf_metrics.h`, `display.h`, `settings.h`, `modules/quiet/quiet_coordinator_templates.h`, `settings_runtime_sync.h`, `battery_manager.h`, `storage_manager.h`, `touch_handler.h`, `perf_sd_logger.h`, `v1_devices.h`, `ble_fresh_flash_policy.h`, `v1_profiles.h`, `modules/wifi/wifi_api_response.h`, `time_service.h`, `settings_sanitize.h`

**Fix:** Remove the `#ifndef`/`#define` line and the matching `#endif` at the bottom from each file. Keep only `#pragma once`. This is a safe mechanical transformation — validate with `pio run -e native`.

### 5.2 Duplicate #include in main_setup_helpers.cpp (Low)

**File:** `src/main_setup_helpers.cpp`, lines 9 and 45
**Issue:** `#include "../include/main_globals.h"` appears twice.
**Fix:** Remove the duplicate on line 45.

### 5.3 Separator Style Minor Inconsistencies (Low)

**Files:** ~60 files across `src/` and `include/`
**Issue:** While the dominant separator pattern matches the standard from Appendix B Phase 4, ~15 minor variations remain (inconsistent dash counts, indented separators, `// -----` vs `// ---`).
**Fix:** Normalize to the two-tier hierarchy in a single pass. Low priority — comments only.

---

## 6. Normalization Status Update (Appendix B Progress)

| Phase | Status | Notes |
|---|---|---|
| Phase 1: `.editorconfig`, `.clang-format`, trailing whitespace | **DONE** | All three complete. Zero trailing whitespace. |
| Phase 2: Header guard standardization | **90% DONE** | `#pragma once` added to all 43 files, but old `#ifndef` guards not removed (5.1 above) |
| Phase 3: Private member naming | **DONE** | All 11 classes normalized. Spot-checked V1BLEClient, V1Display, WiFiManager — all correct. |
| Phase 4: Section separators | **MOSTLY DONE** | ~15 minor variations remain (5.3 above) |
| Phase 5: Log prefixes | **DONE** | 100% `[Tag]` format confirmed. |
| Phase 6: Header placement | **DONE** | No orphaned `src/include/` directory. |
| Phase 7: File headers | **SKIPPED** | Optional per plan. Current state adequate. |

---

## 7. Consolidated Action Items (Ordered by Impact)

### Ship-Blocking (Fix before release)

Items from Appendix D remain the top priority:
1. BLE cleanupConnection() use-after-free (Appendix D, Issue 1)
2. Display flushRegion() fast path (Appendix D, Issue 2)

### High Priority (Fix in next sprint)

3. **TOCTOU on pCommandCharLong_** (§1.2) — potential null deref under BLE reconnect stress
4. **Complete replay test fixture loading** (§4.1) and wire into CI (§4.2) — closes the biggest test coverage methodology gap
5. **Add const-correctness and extern-escape semantic guards** (§4.3) — prevents architecture regressions

### Medium Priority (Planned cleanup)

6. **Remove 43 redundant `#ifndef` guards** (§5.1) — mechanical, safe, completes Phase 2
7. **Integer overflow in dirty region union** (§2.1) — widen to int32_t intermediates
8. **Font degradation strategy** (§2.3) — fallback for OFR rasterization failures
9. **display_palette.h decoupling** (Appendix D, Issue 3) — build isolation improvement

### Low Priority (Polish)

10. **Second double-backslash** in ble_connection.cpp:742 (§1.1) — one-character fix
11. **CCCD magic number** (§1.3) — add named constant
12. **Unvalidated RGB565 colors from NVS** (§2.2) — mask on load
13. **Scattered static render caches** (§2.4) — centralize reset
14. **Duplicate include** in main_setup_helpers.cpp (§5.2) — remove one line
15. **Separator style variations** (§5.3) — normalize remaining ~15
16. **Parameterize copy-paste tests** (§4.4) — maintainability
17. **Expand mutation catalog** (§4.5) — add secondary mutations

---

*This appendix is research-only. No changes were made to the codebase.*

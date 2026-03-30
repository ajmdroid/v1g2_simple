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
- **220KB framebuffer** in PSRAM (172×640×2 bytes) — appropriate for the hardware, but the single-buffer architecture means micro-tearing is possible between row flushes. Not likely noticeable on AMOLED, but worth documenting.

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

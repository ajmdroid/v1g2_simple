/**
 * test_display_rendering_bands.cpp
 *
 * Phase 3 Task 3.2 — integration tests for display_bands.cpp
 * (drawBandIndicators + drawVerticalSignalBars).
 *
 * Includes the real rendering source so that GFX call-recording
 * assertions on the injected Arduino_Canvas verify actual draw behaviour.
 */

#include <unity.h>

// ---------------------------------------------------------------------------
// Mocks (explicit relative path so the include guard fires before any real
// include/display_driver.h is pulled in by src/ headers)
// ---------------------------------------------------------------------------
#include "../mocks/display_driver.h"
#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"

#ifndef ARDUINO
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;
#endif

// Real display classes (display_driver.h guard already set to mock above)
#include "../../src/display.h"
#include "../../include/display_dirty_flags.h"

// ---------------------------------------------------------------------------
// Required extern definitions
// ---------------------------------------------------------------------------
V1Display* g_displayInstance = nullptr;  // Set by V1Display constructor
DisplayDirtyFlags dirty;
SettingsManager settingsManager;

// ---------------------------------------------------------------------------
// Minimal V1Display constructor / destructor stubs
// (avoids pulling in all of display.cpp with its hardware dependencies)
// ---------------------------------------------------------------------------
V1Display::V1Display() {
    currentPalette_ = ColorThemes::STANDARD();
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

// Global test display instance (owns the injected canvas via unique_ptr)
V1Display display;

// ---------------------------------------------------------------------------
// Real rendering code under test
// ---------------------------------------------------------------------------
#include "../../src/display_bands.cpp"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static Arduino_Canvas* canvas() { return display.testCanvas(); }

static void resetCanvas() {
    // Replace the canvas with a fresh one before each test.
    // V1Display takes ownership; old canvas is deleted by unique_ptr.
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    canvas()->resetCounters();
}

// ---------------------------------------------------------------------------
// setUp / tearDown
// ---------------------------------------------------------------------------

void setUp() {
    mockMillis = 1000;
    resetCanvas();
    dirty = DisplayDirtyFlags{};
}

void tearDown() {}

// ============================================================================
// drawBandIndicators tests
// ============================================================================

void test_drawBandIndicators_produces_background_clear_on_first_draw() {
    dirty.bands = true;
    display.ut_drawBandIndicators(BAND_KA, false, 0);

    // Exactly one FILL_RECT clearing the entire band-label stack with PALETTE_BG
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_drawBandIndicators_cache_hit_skips_redraw() {
    dirty.bands = true;
    display.ut_drawBandIndicators(BAND_KA, false, 0);
    resetCanvas();

    // Second call with same args → no new draw calls
    display.ut_drawBandIndicators(BAND_KA, false, 0);
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRectCalls.size());
}

void test_drawBandIndicators_dirty_flag_forces_redraw() {
    dirty.bands = true;
    display.ut_drawBandIndicators(BAND_KA, false, 0);   // first draw (primes cache)
    resetCanvas();

    dirty.bands = true;   // invalidate cache
    display.ut_drawBandIndicators(BAND_KA, false, 0);

    // Cache was invalidated — expect at least one FILL_RECT
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
}

void test_drawBandIndicators_different_mask_invalidates_cache() {
    dirty.bands = true;
    display.ut_drawBandIndicators(BAND_KA, false, 0);
    resetCanvas();

    // Different band mask — cache miss → redraws
    display.ut_drawBandIndicators(BAND_KA | BAND_K, false, 0);
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
}

void test_drawBandIndicators_muted_change_invalidates_cache() {
    dirty.bands = true;
    display.ut_drawBandIndicators(BAND_KA, false, 0);
    resetCanvas();

    // Toggle muted → cache miss
    display.ut_drawBandIndicators(BAND_KA, true, 0);
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
}

// ============================================================================
// drawVerticalSignalBars tests
// ============================================================================

// Helper: force signal bars redraw and return fillRoundRectCalls count
static size_t signalBarsRedrawCount(uint8_t front, uint8_t rear, bool muted) {
    dirty.signalBars = true;
    resetCanvas();
    display.ut_drawVerticalSignalBars(front, rear, BAND_KA, muted);
    return canvas()->fillRoundRectCalls.size();
}

void test_drawSignalBars_strength_4_draws_6_bars() {
    TEST_ASSERT_EQUAL_UINT(6u, signalBarsRedrawCount(4, 0, false));
}

void test_drawSignalBars_strength_0_draws_6_unlit_bars() {
    // strength=0 → all 6 bars are unlit and need drawing (first render after cache clear)
    TEST_ASSERT_EQUAL_UINT(6u, signalBarsRedrawCount(0, 0, false));
}

void test_drawSignalBars_lit_bars_use_bar_colors() {
    dirty.signalBars = true;
    resetCanvas();
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);

    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(6u, calls.size());

    // Bars 0-3 (i < strength=4) must use the configured barN colors
    const V1Settings& s = settingsManager.get();
    TEST_ASSERT_EQUAL_UINT16(s.colorBar1, calls[0].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar2, calls[1].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar3, calls[2].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar4, calls[3].color);
}

void test_drawSignalBars_unlit_bars_use_dark_gray() {
    dirty.signalBars = true;
    resetCanvas();
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);

    const auto& calls = canvas()->fillRoundRectCalls;
    // Bars 4 and 5 (past strength) must use 0x1082 (off-color)
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[4].color);
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[5].color);
}

void test_drawSignalBars_muted_uses_muted_color() {
    dirty.signalBars = true;
    resetCanvas();
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, /*muted=*/true);

    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(6u, calls.size());

    // All lit bars (i < 4) must use PALETTE_MUTED = colorMuted
    const uint16_t expectedMuted = settingsManager.get().colorMuted;
    TEST_ASSERT_EQUAL_UINT16(expectedMuted, calls[0].color);
    TEST_ASSERT_EQUAL_UINT16(expectedMuted, calls[1].color);
    TEST_ASSERT_EQUAL_UINT16(expectedMuted, calls[2].color);
    TEST_ASSERT_EQUAL_UINT16(expectedMuted, calls[3].color);
    // Unlit bars still use 0x1082
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[4].color);
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[5].color);
}

void test_drawSignalBars_cache_hit_no_redraw() {
    dirty.signalBars = true;
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);   // primes cache
    resetCanvas();

    // Same args → cache hit, no new calls
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRoundRectCalls.size());
}

void test_drawSignalBars_dirty_flag_forces_redraw() {
    dirty.signalBars = true;
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);   // first draw
    resetCanvas();

    dirty.signalBars = true;     // invalidate
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);

    TEST_ASSERT_EQUAL_UINT(6u, canvas()->fillRoundRectCalls.size());
}

void test_drawSignalBars_max_of_front_rear_used() {
    // rearStrength > frontStrength → max is used
    dirty.signalBars = true;
    resetCanvas();
    display.ut_drawVerticalSignalBars(2, 5, BAND_KA, false);

    // 5 lit + 1 unlit = 6 bars. Bars 0-4 use bar colors, bar 5 uses 0x1082.
    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(6u, calls.size());
    const V1Settings& s = settingsManager.get();
    TEST_ASSERT_EQUAL_UINT16(s.colorBar5, calls[4].color);   // i=4 → 5th bar from bottom
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[5].color);        // i=5 → unlit
}

// ============================================================================
// main
// ============================================================================

int main() {
    UNITY_BEGIN();

    // drawBandIndicators
    RUN_TEST(test_drawBandIndicators_produces_background_clear_on_first_draw);
    RUN_TEST(test_drawBandIndicators_cache_hit_skips_redraw);
    RUN_TEST(test_drawBandIndicators_dirty_flag_forces_redraw);
    RUN_TEST(test_drawBandIndicators_different_mask_invalidates_cache);
    RUN_TEST(test_drawBandIndicators_muted_change_invalidates_cache);

    // drawVerticalSignalBars
    RUN_TEST(test_drawSignalBars_strength_4_draws_6_bars);
    RUN_TEST(test_drawSignalBars_strength_0_draws_6_unlit_bars);
    RUN_TEST(test_drawSignalBars_lit_bars_use_bar_colors);
    RUN_TEST(test_drawSignalBars_unlit_bars_use_dark_gray);
    RUN_TEST(test_drawSignalBars_muted_uses_muted_color);
    RUN_TEST(test_drawSignalBars_cache_hit_no_redraw);
    RUN_TEST(test_drawSignalBars_dirty_flag_forces_redraw);
    RUN_TEST(test_drawSignalBars_max_of_front_rear_used);

    return UNITY_END();
}

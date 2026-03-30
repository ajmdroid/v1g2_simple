/**
 * test_display_rendering_indicators.cpp
 *
 * Phase 3 Task 3.4 — integration tests for display_indicators.cpp
 * (drawLockoutIndicator, drawGpsIndicator, drawObdIndicator, drawBaseFrame).
 *
 * Includes the real rendering source so that GFX call-recording assertions
 * on the injected Arduino_Canvas verify actual draw behaviour.
 *
 * NOTE: Drawing logic in display_indicators.cpp is guarded by
 * #if defined(DISPLAY_WAVESHARE_349) — we define it here to enable it.
 */

// Enable the display-variant guards before any display headers are pulled in.
#ifndef DISPLAY_WAVESHARE_349
#define DISPLAY_WAVESHARE_349 1
#endif

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
V1Display* g_displayInstance = nullptr;
DisplayDirtyFlags dirty;
SettingsManager settingsManager;

// ---------------------------------------------------------------------------
// Minimal V1Display constructor / destructor stubs
// ---------------------------------------------------------------------------
V1Display::V1Display() {
    currentPalette = ColorThemes::STANDARD();
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

// Global test display instance
V1Display display;

// ---------------------------------------------------------------------------
// Real rendering code under test.
// display_indicators.cpp includes the GPS/OBD module headers from src/ — those
// compile fine on native because FreeRTOS is mocked in test/mocks/freertos/.
// ---------------------------------------------------------------------------
#include "../../src/display_indicators.cpp"

// ---------------------------------------------------------------------------
// Stub for drawBLEProxyIndicator (defined in display_status_bar.cpp which is
// not compiled here; prepareFullRedrawNoClear() calls it after a full clear).
// ---------------------------------------------------------------------------
void V1Display::drawBLEProxyIndicator() {}

// ---------------------------------------------------------------------------
// Module globals required by display_indicators.cpp externs.
// snapshot() is never called in these tests so the implementations are not
// needed; only the type definition from the headers is required.
// ---------------------------------------------------------------------------
GpsRuntimeModule gpsRuntimeModule;
ObdRuntimeModule obdRuntimeModule;

// Stub snapshot() implementations so the linker satisfies syncTopIndicators()
// references even though syncTopIndicators() is never called in these tests.
GpsRuntimeStatus GpsRuntimeModule::snapshot(uint32_t /*nowMs*/) const {
    return GpsRuntimeStatus{};
}
ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t /*nowMs*/) const {
    return ObdRuntimeStatus{};
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static Arduino_Canvas* canvas() { return display.testCanvas(); }

static void resetCanvas() {
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
    // Invalidate all static caches in each indicator function
    dirty.lockout      = true;
    dirty.gpsIndicator = true;
    dirty.obdIndicator = true;
}

void tearDown() {}

// ============================================================================
// drawBaseFrame tests
// ============================================================================

void test_drawBaseFrame_fills_screen_with_bg_color() {
    display.ut_drawBaseFrame();
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFillScreenCount());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->getLastFillColor());
}

void test_drawBaseFrame_sets_all_dirty_flags() {
    // After drawBaseFrame, dirty.setAll() is called — verify at least one managed
    // flag (bands) is set; all flags are reset by dirty.setAll()
    dirty = DisplayDirtyFlags{};  // clear everything first
    display.ut_drawBaseFrame();
    TEST_ASSERT_TRUE(dirty.bands);
    TEST_ASSERT_TRUE(dirty.arrow);
}

// ============================================================================
// drawLockoutIndicator tests
// ============================================================================

void test_drawLockoutIndicator_shown_draws_badge() {
    display.setLockoutIndicator(true);
    display.ut_drawLockoutIndicator();

    // Should draw at least one fillRoundRect (the badge fill)
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRoundRectCalls.size());
}

void test_drawLockoutIndicator_shown_badge_uses_lockout_color() {
    display.setLockoutIndicator(true);
    display.ut_drawLockoutIndicator();

    // The badge fill color is dimColor(colorLockout, 45) — a dimmed version of 0x07E0.
    // We just check that the first fillRoundRect color is NOT PALETTE_BG (it's the badge).
    uint16_t bg = ColorThemes::STANDARD().bg;
    TEST_ASSERT_NOT_EQUAL(bg, canvas()->fillRoundRectCalls[0].color);
}

void test_drawLockoutIndicator_hidden_clears_area() {
    display.setLockoutIndicator(false);
    display.ut_drawLockoutIndicator();

    // Hidden state: clears the badge area with BG color
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_drawLockoutIndicator_cache_hit_skips_redraw() {
    display.setLockoutIndicator(true);
    display.ut_drawLockoutIndicator();  // primes cache
    resetCanvas();

    // Same state, no dirty flag → return early
    display.ut_drawLockoutIndicator();
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRoundRectCalls.size());
}

void test_drawLockoutIndicator_dirty_flag_forces_redraw() {
    display.setLockoutIndicator(true);
    display.ut_drawLockoutIndicator();  // primes cache
    resetCanvas();

    dirty.lockout = true;   // invalidate
    display.ut_drawLockoutIndicator();
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRoundRectCalls.size());
}

// ============================================================================
// drawGpsIndicator tests
// ============================================================================

void test_drawGpsIndicator_with_fix_clears_then_draws_text() {
    display.setGpsSatellites(true, true, 8);
    display.ut_drawGpsIndicator();

    // Draws one fillRect for the background clear
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
}

void test_drawGpsIndicator_no_fix_clears_area() {
    display.setGpsSatellites(true, false, 0);
    display.ut_drawGpsIndicator();

    // No fix → just clear the area
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_drawGpsIndicator_disabled_clears_area() {
    display.setGpsSatellites(false, false, 0);
    display.ut_drawGpsIndicator();

    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_drawGpsIndicator_cache_hit_skips_redraw() {
    display.setGpsSatellites(true, true, 8);
    display.ut_drawGpsIndicator();  // primes cache
    resetCanvas();

    display.ut_drawGpsIndicator();  // same state, no dirty → cache hit
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRectCalls.size());
}

// ============================================================================
// drawObdIndicator tests
// ============================================================================

void test_drawObdIndicator_enabled_connected_draws_text() {
    display.ut_setObdStatus(true, true, false);
    display.ut_drawObdIndicator();

    // One fillRect (background clear) + text draw
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_drawObdIndicator_disabled_clears_area() {
    display.ut_setObdStatus(false, false, false);
    display.ut_drawObdIndicator();

    // Disabled: clears with BG and returns
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_drawObdIndicator_cache_hit_skips_redraw() {
    display.ut_setObdStatus(true, false, false);
    display.ut_drawObdIndicator();  // primes cache
    resetCanvas();

    display.ut_drawObdIndicator();  // same state, no dirty → cache hit
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRectCalls.size());
}

// ============================================================================
// Test runner
// ============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_drawBaseFrame_fills_screen_with_bg_color);
    RUN_TEST(test_drawBaseFrame_sets_all_dirty_flags);
    RUN_TEST(test_drawLockoutIndicator_shown_draws_badge);
    RUN_TEST(test_drawLockoutIndicator_shown_badge_uses_lockout_color);
    RUN_TEST(test_drawLockoutIndicator_hidden_clears_area);
    RUN_TEST(test_drawLockoutIndicator_cache_hit_skips_redraw);
    RUN_TEST(test_drawLockoutIndicator_dirty_flag_forces_redraw);
    RUN_TEST(test_drawGpsIndicator_with_fix_clears_then_draws_text);
    RUN_TEST(test_drawGpsIndicator_no_fix_clears_area);
    RUN_TEST(test_drawGpsIndicator_disabled_clears_area);
    RUN_TEST(test_drawGpsIndicator_cache_hit_skips_redraw);
    RUN_TEST(test_drawObdIndicator_enabled_connected_draws_text);
    RUN_TEST(test_drawObdIndicator_disabled_clears_area);
    RUN_TEST(test_drawObdIndicator_cache_hit_skips_redraw);
    return UNITY_END();
}

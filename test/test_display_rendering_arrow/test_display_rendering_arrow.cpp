/**
 * test_display_rendering_arrow.cpp
 *
 * Phase 3 Task 3.3 — integration tests for display_arrow.cpp
 * (drawDirectionArrow).
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
#include "../../include/display_element_caches.h"

// ---------------------------------------------------------------------------
// Required extern definitions
// ---------------------------------------------------------------------------
V1Display* g_displayInstance = nullptr;  // Set by V1Display constructor
DisplayDirtyFlags dirty;
DisplayElementCaches g_elementCaches;
SettingsManager settingsManager;

// ---------------------------------------------------------------------------
// Minimal V1Display constructor / destructor stubs
// ---------------------------------------------------------------------------
V1Display::V1Display() {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settingsManager.get().colorMuted;
    currentPalette_.colorPersisted = settingsManager.get().colorPersisted;
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

// Global test display instance (owns the injected canvas via unique_ptr)
V1Display display;

// ---------------------------------------------------------------------------
// Real rendering code under test
// ---------------------------------------------------------------------------
#include "../../src/display_arrow.cpp"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static Arduino_Canvas* canvas() { return display.testCanvas(); }

static void resetCanvas() {
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    canvas()->resetCounters();
}

// Arrow active direction combinations
static const Direction DIR_ALL = static_cast<Direction>(DIR_FRONT | DIR_SIDE | DIR_REAR);

// Default arrow colors (from settings mock)
static constexpr uint16_t FRONT_COL = 0xF800;  // s.colorArrowFront default
static constexpr uint16_t SIDE_COL  = 0xF800;  // s.colorArrowSide default
static constexpr uint16_t REAR_COL  = 0xF800;  // s.colorArrowRear default
static constexpr uint16_t OFF_COL   = 0x1082;  // offCol constant in display_arrow.cpp
static constexpr uint16_t MUTED_COL = 0x8410;  // colorMuted default

// ---------------------------------------------------------------------------
// setUp / tearDown
// ---------------------------------------------------------------------------

void setUp() {
    mockMillis = 1000;
    resetCanvas();
    dirty = DisplayDirtyFlags{};
    g_elementCaches = DisplayElementCaches{};  // invalidate all caches every test
}

void tearDown() {}

// ============================================================================
// Full-redraw structure tests (cacheValid forced to false via g_elementCaches.arrow.valid)
// ============================================================================

void test_drawArrow_full_redraw_produces_4_fill_triangles() {
    // Full redraw draws: 1 top triangle + 2 side triangles + 1 rear triangle
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillTriangleCalls.size());
}

void test_drawArrow_full_redraw_clears_region_with_bg_color() {
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    // First fillRect call is the full-region clear with PALETTE_BG
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

// ============================================================================
// Cache tests
// ============================================================================

void test_drawArrow_cache_hit_skips_redraw() {
    // First draw primes the cache
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    resetCanvas();

    // Same args, no dirty flag → return early, no new draw calls
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillTriangleCalls.size());
}

void test_drawArrow_dirty_arrow_flag_forces_redraw() {
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);  // primes cache
    resetCanvas();

    g_elementCaches.arrow.valid = false;  // invalidate
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillTriangleCalls.size());
}

void test_drawArrow_direction_change_invalidates_cache() {
    // Prime cache with no arrows active
    display.ut_drawDirectionArrow(DIR_NONE, false, 0, 0);
    resetCanvas();

    // All three visibility bits change at once → full redraw (4 triangles)
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillTriangleCalls.size());
}

void test_drawArrow_muted_change_invalidates_cache() {
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);  // primes cache (not muted)
    resetCanvas();

    // Muted changes → cache miss → redraws
    display.ut_drawDirectionArrow(DIR_ALL, true, 0, 0);
    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillTriangleCalls.size());
}

// ============================================================================
// Active-color tests (fillTriangleCalls[0]=top, [1]=side-left, [2]=side-right, [3]=rear)
// ============================================================================

void test_drawArrow_front_only_top_triangle_uses_front_color() {
    display.ut_drawDirectionArrow(DIR_FRONT, false, 0, 0);
    auto& tc = canvas()->fillTriangleCalls;
    TEST_ASSERT_EQUAL_UINT(4u, tc.size());
    TEST_ASSERT_EQUAL_UINT16(FRONT_COL, tc[0].color);  // top   = active
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,   tc[1].color);  // side  = inactive
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,   tc[2].color);  // side  = inactive
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,   tc[3].color);  // rear  = inactive
}

void test_drawArrow_rear_only_bottom_triangle_uses_rear_color() {
    display.ut_drawDirectionArrow(DIR_REAR, false, 0, 0);
    auto& tc = canvas()->fillTriangleCalls;
    TEST_ASSERT_EQUAL_UINT(4u, tc.size());
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,  tc[0].color);  // top  = inactive
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,  tc[1].color);  // side = inactive
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,  tc[2].color);  // side = inactive
    TEST_ASSERT_EQUAL_UINT16(REAR_COL, tc[3].color);  // rear = active
}

void test_drawArrow_side_only_both_side_triangles_use_side_color() {
    display.ut_drawDirectionArrow(DIR_SIDE, false, 0, 0);
    auto& tc = canvas()->fillTriangleCalls;
    TEST_ASSERT_EQUAL_UINT(4u, tc.size());
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,  tc[0].color);  // top  = inactive
    TEST_ASSERT_EQUAL_UINT16(SIDE_COL, tc[1].color);  // side-left  = active
    TEST_ASSERT_EQUAL_UINT16(SIDE_COL, tc[2].color);  // side-right = active
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,  tc[3].color);  // rear = inactive
}

void test_drawArrow_muted_all_triangles_use_muted_color() {
    display.ut_drawDirectionArrow(DIR_ALL, true, 0, 0);
    auto& tc = canvas()->fillTriangleCalls;
    TEST_ASSERT_EQUAL_UINT(4u, tc.size());
    for (size_t i = 0; i < tc.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(MUTED_COL, tc[i].color, "all arrows should use muted color");
    }
}

void test_drawArrow_no_arrows_all_triangles_use_off_color() {
    display.ut_drawDirectionArrow(DIR_NONE, false, 0, 0);
    auto& tc = canvas()->fillTriangleCalls;
    TEST_ASSERT_EQUAL_UINT(4u, tc.size());
    for (size_t i = 0; i < tc.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(OFF_COL, tc[i].color, "all arrows should use off color");
    }
}

// ============================================================================
// Test runner
// ============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_drawArrow_full_redraw_produces_4_fill_triangles);
    RUN_TEST(test_drawArrow_full_redraw_clears_region_with_bg_color);
    RUN_TEST(test_drawArrow_cache_hit_skips_redraw);
    RUN_TEST(test_drawArrow_dirty_arrow_flag_forces_redraw);
    RUN_TEST(test_drawArrow_direction_change_invalidates_cache);
    RUN_TEST(test_drawArrow_muted_change_invalidates_cache);
    RUN_TEST(test_drawArrow_front_only_top_triangle_uses_front_color);
    RUN_TEST(test_drawArrow_rear_only_bottom_triangle_uses_rear_color);
    RUN_TEST(test_drawArrow_side_only_both_side_triangles_use_side_color);
    RUN_TEST(test_drawArrow_muted_all_triangles_use_muted_color);
    RUN_TEST(test_drawArrow_no_arrows_all_triangles_use_off_color);
    return UNITY_END();
}

/**
 * test_display_dirty_flags.cpp
 *
 * Phase 1 Task 1.3 — pure logic tests for display_dirty_flags.h.
 * DisplayDirtyFlags is a plain struct with no hardware dependencies.
 */

#include <unity.h>

#include "../../include/display_dirty_flags.h"

// Provide the required extern definition
DisplayDirtyFlags dirty;

void setUp() {
    // Reset to all-false before each test
    dirty = DisplayDirtyFlags{};
}

void tearDown() {}

// ============================================================================
// Initial state
// ============================================================================

void test_dirty_flags_default_all_false() {
    DisplayDirtyFlags f{};
    TEST_ASSERT_FALSE(f.multiAlert);
    TEST_ASSERT_FALSE(f.cards);
    TEST_ASSERT_FALSE(f.frequency);
    TEST_ASSERT_FALSE(f.battery);
    TEST_ASSERT_FALSE(f.bands);
    TEST_ASSERT_FALSE(f.signalBars);
    TEST_ASSERT_FALSE(f.arrow);
    TEST_ASSERT_FALSE(f.muteIcon);
    TEST_ASSERT_FALSE(f.topCounter);
    TEST_ASSERT_FALSE(f.lockout);
    TEST_ASSERT_FALSE(f.gpsIndicator);
    TEST_ASSERT_FALSE(f.obdIndicator);
    TEST_ASSERT_FALSE(f.resetTracking);
}

// ============================================================================
// setAll() — sets core redraw flags, leaves management flags alone
// ============================================================================

void test_dirty_setAll_sets_primary_frame_flags() {
    DisplayDirtyFlags f{};
    f.setAll();
    TEST_ASSERT_TRUE(f.frequency);
    TEST_ASSERT_TRUE(f.battery);
    TEST_ASSERT_TRUE(f.bands);
    TEST_ASSERT_TRUE(f.signalBars);
    TEST_ASSERT_TRUE(f.arrow);
    TEST_ASSERT_TRUE(f.muteIcon);
    TEST_ASSERT_TRUE(f.topCounter);
    TEST_ASSERT_TRUE(f.lockout);
    TEST_ASSERT_TRUE(f.gpsIndicator);
    TEST_ASSERT_TRUE(f.obdIndicator);
}

void test_dirty_setAll_does_not_set_multiAlert() {
    DisplayDirtyFlags f{};
    f.setAll();
    TEST_ASSERT_FALSE(f.multiAlert);
}

void test_dirty_setAll_does_not_set_cards() {
    DisplayDirtyFlags f{};
    f.setAll();
    TEST_ASSERT_FALSE(f.cards);
}

void test_dirty_setAll_does_not_set_resetTracking() {
    DisplayDirtyFlags f{};
    f.setAll();
    TEST_ASSERT_FALSE(f.resetTracking);
}

void test_dirty_setAll_preserves_previously_set_multiAlert() {
    DisplayDirtyFlags f{};
    f.multiAlert = true;
    f.setAll();
    // setAll does not clear multiAlert
    TEST_ASSERT_TRUE(f.multiAlert);
}

// ============================================================================
// Individual flag manipulation
// ============================================================================

void test_dirty_individual_flags_can_be_set_independently() {
    DisplayDirtyFlags f{};
    f.bands = true;
    TEST_ASSERT_TRUE(f.bands);
    TEST_ASSERT_FALSE(f.arrow);
    TEST_ASSERT_FALSE(f.frequency);
}

void test_dirty_flag_cleared_by_assignment() {
    DisplayDirtyFlags f{};
    f.setAll();
    f.bands = false;
    TEST_ASSERT_FALSE(f.bands);
    // Other flags must still be set
    TEST_ASSERT_TRUE(f.frequency);
    TEST_ASSERT_TRUE(f.arrow);
}

// ============================================================================
// main
// ============================================================================

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_dirty_flags_default_all_false);
    RUN_TEST(test_dirty_setAll_sets_primary_frame_flags);
    RUN_TEST(test_dirty_setAll_does_not_set_multiAlert);
    RUN_TEST(test_dirty_setAll_does_not_set_cards);
    RUN_TEST(test_dirty_setAll_does_not_set_resetTracking);
    RUN_TEST(test_dirty_setAll_preserves_previously_set_multiAlert);
    RUN_TEST(test_dirty_individual_flags_can_be_set_independently);
    RUN_TEST(test_dirty_flag_cleared_by_assignment);

    return UNITY_END();
}

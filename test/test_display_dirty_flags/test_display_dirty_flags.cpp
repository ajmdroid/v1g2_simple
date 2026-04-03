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
    TEST_ASSERT_FALSE(f.obdIndicator);
    TEST_ASSERT_FALSE(f.resetTracking);
}

// ============================================================================
// setAll() — sets residual dirty flags, leaves management flags alone
// ============================================================================

void test_dirty_setAll_sets_obd_indicator_only() {
    DisplayDirtyFlags f{};
    f.setAll();
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

void test_dirty_setAll_preserves_previously_set_cards() {
    DisplayDirtyFlags f{};
    f.cards = true;
    f.setAll();
    TEST_ASSERT_TRUE(f.cards);
}

void test_dirty_setAll_preserves_previously_set_resetTracking() {
    DisplayDirtyFlags f{};
    f.resetTracking = true;
    f.setAll();
    TEST_ASSERT_TRUE(f.resetTracking);
}

// ============================================================================
// Individual flag manipulation
// ============================================================================

void test_dirty_individual_flags_can_be_set_independently() {
    DisplayDirtyFlags f{};
    f.cards = true;
    TEST_ASSERT_TRUE(f.cards);
    TEST_ASSERT_FALSE(f.multiAlert);
    TEST_ASSERT_FALSE(f.obdIndicator);
}

void test_dirty_flag_cleared_by_assignment() {
    DisplayDirtyFlags f{};
    f.setAll();
    f.obdIndicator = false;
    TEST_ASSERT_FALSE(f.obdIndicator);
    // Other flags must still be set
    TEST_ASSERT_FALSE(f.multiAlert);
    TEST_ASSERT_FALSE(f.cards);
}

// ============================================================================
// main
// ============================================================================

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_dirty_flags_default_all_false);
    RUN_TEST(test_dirty_setAll_sets_obd_indicator_only);
    RUN_TEST(test_dirty_setAll_does_not_set_multiAlert);
    RUN_TEST(test_dirty_setAll_does_not_set_cards);
    RUN_TEST(test_dirty_setAll_does_not_set_resetTracking);
    RUN_TEST(test_dirty_setAll_preserves_previously_set_multiAlert);
    RUN_TEST(test_dirty_setAll_preserves_previously_set_cards);
    RUN_TEST(test_dirty_setAll_preserves_previously_set_resetTracking);
    RUN_TEST(test_dirty_individual_flags_can_be_set_independently);
    RUN_TEST(test_dirty_flag_cleared_by_assignment);

    return UNITY_END();
}

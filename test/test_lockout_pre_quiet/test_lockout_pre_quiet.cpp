#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"

#include "../../src/modules/lockout/lockout_pre_quiet_controller.h"
#include "../../src/modules/lockout/lockout_pre_quiet_controller.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Helper: millis progression constants
// ---------------------------------------------------------------------------
static constexpr uint32_t T0 = 10000;
static constexpr uint32_t ENTRY_WAIT = 250;  // > 200ms entry debounce
static constexpr uint32_t EXIT_WAIT  = 550;  // > 500ms exit debounce

// ---------------------------------------------------------------------------
// 1. Feature disabled → always NONE
// ---------------------------------------------------------------------------
void test_feature_disabled_returns_none() {
    PreQuietState state;
    auto d = evaluatePreQuiet(
        /*featureEnabled=*/false, /*enforceMode=*/true, /*bleConnected=*/true,
        /*hasAlert=*/false, /*lockoutEvaluated=*/false, /*lockoutShouldMute=*/false,
        /*nearbyZoneCount=*/3, /*currentMainVolume=*/6, /*currentMuteVolume=*/0,
        T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_FALSE(state.preQuietActive);
}

// ---------------------------------------------------------------------------
// 2. Non-enforce mode → always NONE
// ---------------------------------------------------------------------------
void test_non_enforce_mode_returns_none() {
    PreQuietState state;
    auto d = evaluatePreQuiet(
        true, /*enforceMode=*/false, true,
        false, false, false,
        3, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
}

// ---------------------------------------------------------------------------
// 3. BLE disconnected → always NONE (and restores if active)
// ---------------------------------------------------------------------------
void test_ble_disconnected_restores_if_active() {
    PreQuietState state;
    state.preQuietActive = true;
    state.savedMainVolume = 6;
    state.savedMuteVolume = 0;

    auto d = evaluatePreQuiet(
        true, true, /*bleConnected=*/false,
        false, false, false,
        3, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d.action);
    TEST_ASSERT_EQUAL(6, d.volume);
    TEST_ASSERT_EQUAL(0, d.muteVolume);
    TEST_ASSERT_FALSE(state.preQuietActive);
}

// ---------------------------------------------------------------------------
// 4. Entry debounce: first call starts timer, second within debounce = NONE
// ---------------------------------------------------------------------------
void test_entry_debounce_waits() {
    PreQuietState state;

    // First call — starts debounce timer.
    auto d1 = evaluatePreQuiet(
        true, true, true,
        false, false, false,
        2, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d1.action);
    TEST_ASSERT_FALSE(state.preQuietActive);
    TEST_ASSERT_EQUAL(T0, state.enteredZoneMs);

    // 100ms later — still debouncing.
    auto d2 = evaluatePreQuiet(
        true, true, true,
        false, false, false,
        2, 6, 0, T0 + 100, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d2.action);
    TEST_ASSERT_FALSE(state.preQuietActive);
}

// ---------------------------------------------------------------------------
// 5. Entry fires DROP_VOLUME after debounce elapses
// ---------------------------------------------------------------------------
void test_entry_drops_volume_after_debounce() {
    PreQuietState state;

    // Start debounce.
    evaluatePreQuiet(true, true, true, false, false, false,
                     2, 6, 0, T0, state);

    // After entry debounce.
    auto d = evaluatePreQuiet(
        true, true, true,
        false, false, false,
        2, 6, 0, T0 + ENTRY_WAIT, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::DROP_VOLUME, d.action);
    TEST_ASSERT_EQUAL(0, d.volume);       // dropped to muteVolume
    TEST_ASSERT_EQUAL(0, d.muteVolume);
    TEST_ASSERT_TRUE(state.preQuietActive);
    TEST_ASSERT_EQUAL(6, state.savedMainVolume);
    TEST_ASSERT_EQUAL(0, state.savedMuteVolume);
}

// ---------------------------------------------------------------------------
// 6. Already pre-quieted + still in zone + no alert → NONE (no repeat)
// ---------------------------------------------------------------------------
void test_already_quiet_in_zone_no_repeat() {
    PreQuietState state;
    state.preQuietActive = true;
    state.savedMainVolume = 6;
    state.savedMuteVolume = 0;

    auto d = evaluatePreQuiet(
        true, true, true,
        false, false, false,
        2, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_TRUE(state.preQuietActive);
}

// ---------------------------------------------------------------------------
// 7. Alert + lockout match → NONE (let mute controller handle)
// ---------------------------------------------------------------------------
void test_alert_lockout_match_stays_quiet() {
    PreQuietState state;
    state.preQuietActive = true;
    state.savedMainVolume = 6;
    state.savedMuteVolume = 0;

    auto d = evaluatePreQuiet(
        true, true, true,
        /*hasAlert=*/true, /*lockoutEvaluated=*/true, /*lockoutShouldMute=*/true,
        2, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_TRUE(state.preQuietActive);
}

// ---------------------------------------------------------------------------
// 8. Alert + NOT lockout match → RESTORE immediately (real threat)
// ---------------------------------------------------------------------------
void test_real_alert_restores_immediately() {
    PreQuietState state;
    state.preQuietActive = true;
    state.savedMainVolume = 6;
    state.savedMuteVolume = 0;

    auto d = evaluatePreQuiet(
        true, true, true,
        /*hasAlert=*/true, /*lockoutEvaluated=*/true, /*lockoutShouldMute=*/false,
        2, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d.action);
    TEST_ASSERT_EQUAL(6, d.volume);
    TEST_ASSERT_EQUAL(0, d.muteVolume);
    TEST_ASSERT_FALSE(state.preQuietActive);
}

// ---------------------------------------------------------------------------
// 9. Exit debounce: leaving zone starts timer, restores after debounce
// ---------------------------------------------------------------------------
void test_exit_debounce_restores_after_wait() {
    PreQuietState state;
    state.preQuietActive = true;
    state.savedMainVolume = 6;
    state.savedMuteVolume = 0;

    // Left zone — start exit debounce.
    auto d1 = evaluatePreQuiet(
        true, true, true,
        false, false, false,
        /*nearbyZoneCount=*/0, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d1.action);
    TEST_ASSERT_TRUE(state.preQuietActive);
    TEST_ASSERT_EQUAL(T0, state.leftZoneMs);

    // 200ms later — still debouncing.
    auto d2 = evaluatePreQuiet(
        true, true, true,
        false, false, false,
        0, 0, 0, T0 + 200, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d2.action);
    TEST_ASSERT_TRUE(state.preQuietActive);

    // After exit debounce.
    auto d3 = evaluatePreQuiet(
        true, true, true,
        false, false, false,
        0, 0, 0, T0 + EXIT_WAIT, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d3.action);
    TEST_ASSERT_EQUAL(6, d3.volume);
    TEST_ASSERT_EQUAL(0, d3.muteVolume);
    TEST_ASSERT_FALSE(state.preQuietActive);
}

// ---------------------------------------------------------------------------
// 10. Re-entry after restore → cycles correctly
// ---------------------------------------------------------------------------
void test_reentry_after_restore_cycles() {
    PreQuietState state;

    // First entry + debounce → DROP.
    evaluatePreQuiet(true, true, true, false, false, false, 2, 6, 0, T0, state);
    auto drop = evaluatePreQuiet(true, true, true, false, false, false,
                                  2, 6, 0, T0 + ENTRY_WAIT, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::DROP_VOLUME, drop.action);
    TEST_ASSERT_TRUE(state.preQuietActive);

    // Leave zone + debounce → RESTORE.
    evaluatePreQuiet(true, true, true, false, false, false, 0, 0, 0, T0 + 1000, state);
    auto restore = evaluatePreQuiet(true, true, true, false, false, false,
                                     0, 0, 0, T0 + 1000 + EXIT_WAIT, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, restore.action);
    TEST_ASSERT_FALSE(state.preQuietActive);

    // Re-enter another zone → should debounce and DROP again.
    evaluatePreQuiet(true, true, true, false, false, false, 1, 6, 0, T0 + 3000, state);
    auto drop2 = evaluatePreQuiet(true, true, true, false, false, false,
                                   1, 6, 0, T0 + 3000 + ENTRY_WAIT, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::DROP_VOLUME, drop2.action);
    TEST_ASSERT_TRUE(state.preQuietActive);
}

// ---------------------------------------------------------------------------
// 11. Feature disabled while active → restores volume
// ---------------------------------------------------------------------------
void test_feature_disabled_while_active_restores() {
    PreQuietState state;
    state.preQuietActive = true;
    state.savedMainVolume = 6;
    state.savedMuteVolume = 0;

    auto d = evaluatePreQuiet(
        /*featureEnabled=*/false, true, true,
        false, false, false,
        2, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d.action);
    TEST_ASSERT_EQUAL(6, d.volume);
    TEST_ASSERT_FALSE(state.preQuietActive);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_feature_disabled_returns_none);
    RUN_TEST(test_non_enforce_mode_returns_none);
    RUN_TEST(test_ble_disconnected_restores_if_active);
    RUN_TEST(test_entry_debounce_waits);
    RUN_TEST(test_entry_drops_volume_after_debounce);
    RUN_TEST(test_already_quiet_in_zone_no_repeat);
    RUN_TEST(test_alert_lockout_match_stays_quiet);
    RUN_TEST(test_real_alert_restores_immediately);
    RUN_TEST(test_exit_debounce_restores_after_wait);
    RUN_TEST(test_reentry_after_restore_cycles);
    RUN_TEST(test_feature_disabled_while_active_restores);
    return UNITY_END();
}

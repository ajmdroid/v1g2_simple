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
// Helper constants
// ---------------------------------------------------------------------------
static constexpr uint32_t T0 = 10000;
static constexpr uint32_t ENTRY_WAIT = 150;  // > 100ms entry debounce
static constexpr uint32_t EXIT_WAIT  = 550;  // > 500ms exit debounce

// Helper: build a DROPPED state as if entry debounce already elapsed.
static PreQuietState droppedState(uint8_t savedMain = 6, uint8_t savedMute = 0,
                                  uint32_t droppedAt = T0) {
    PreQuietState s;
    s.phase = PreQuietPhase::DROPPED;
    s.savedMainVolume = savedMain;
    s.savedMuteVolume = savedMute;
    s.droppedAtMs = droppedAt;
    return s;
}

// ---------------------------------------------------------------------------
// 1. Feature disabled → NONE, state cleared
// ---------------------------------------------------------------------------
void test_feature_disabled_returns_none() {
    PreQuietState state;
    auto d = evaluatePreQuiet(
        /*featureEnabled=*/false, true, true, true,
        false, false, false,
        3, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 2. Non-enforce mode → NONE
// ---------------------------------------------------------------------------
void test_non_enforce_mode_returns_none() {
    PreQuietState state;
    auto d = evaluatePreQuiet(
        true, /*enforceMode=*/false, true, true,
        false, false, false,
        3, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 3. BLE disconnected → NONE, DROPPED state preserved
// ---------------------------------------------------------------------------
void test_ble_disconnected_preserves_state() {
    PreQuietState state = droppedState();

    auto d = evaluatePreQuiet(
        true, true, /*bleConnected=*/false, true,
        false, false, false,
        3, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
    TEST_ASSERT_EQUAL(6, state.savedMainVolume);
}

// ---------------------------------------------------------------------------
// 4. Entry debounce waits before dropping
// ---------------------------------------------------------------------------
void test_entry_debounce_waits() {
    PreQuietState state;

    auto d1 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               2, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d1.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
    TEST_ASSERT_EQUAL(T0, state.enteredZoneMs);

    auto d2 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               2, 6, 0, T0 + 50, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d2.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 5. Entry fires DROP after debounce → phase = DROPPED
// ---------------------------------------------------------------------------
void test_entry_drops_volume_after_debounce() {
    PreQuietState state;

    evaluatePreQuiet(true, true, true, true, false, false, false,
                     2, 6, 0, T0, state);

    auto d = evaluatePreQuiet(true, true, true, true, false, false, false,
                              2, 6, 0, T0 + ENTRY_WAIT, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::DROP_VOLUME, d.action);
    TEST_ASSERT_EQUAL(0, d.volume);
    TEST_ASSERT_EQUAL(0, d.muteVolume);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
    TEST_ASSERT_EQUAL(6, state.savedMainVolume);
}

// ---------------------------------------------------------------------------
// 6. DROPPED + still in zone + no alert → NONE (hold)
// ---------------------------------------------------------------------------
void test_dropped_in_zone_holds() {
    PreQuietState state = droppedState();

    auto d = evaluatePreQuiet(true, true, true, true, false, false, false,
                              2, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
}

// ---------------------------------------------------------------------------
// 7. DROPPED + lockout-matched alert → NONE (mute controller handles)
// ---------------------------------------------------------------------------
void test_dropped_lockout_match_stays_quiet() {
    PreQuietState state = droppedState();

    auto d = evaluatePreQuiet(true, true, true, true,
                              true, true, /*lockoutShouldMute=*/true,
                              2, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
}

// ---------------------------------------------------------------------------
// 8. DROPPED + real alert → RESTORE → DISARMED
// ---------------------------------------------------------------------------
void test_real_alert_restores_to_disarmed() {
    PreQuietState state = droppedState();

    auto d = evaluatePreQuiet(true, true, true, true,
                              true, true, /*lockoutShouldMute=*/false,
                              2, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d.action);
    TEST_ASSERT_EQUAL(6, d.volume);
    TEST_ASSERT_EQUAL(0, d.muteVolume);
    TEST_ASSERT_EQUAL(PreQuietPhase::DISARMED, state.phase);
}

// ---------------------------------------------------------------------------
// 9. DISARMED + still in zone → stays DISARMED, no BLE command
// ---------------------------------------------------------------------------
void test_disarmed_stays_in_zone() {
    PreQuietState state;
    state.phase = PreQuietPhase::DISARMED;

    auto d = evaluatePreQuiet(true, true, true, true, false, false, false,
                              2, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DISARMED, state.phase);
}

// ---------------------------------------------------------------------------
// 10. DISARMED + leave zone → IDLE (no BLE command needed)
// ---------------------------------------------------------------------------
void test_disarmed_leaves_zone_goes_idle() {
    PreQuietState state;
    state.phase = PreQuietPhase::DISARMED;

    auto d = evaluatePreQuiet(true, true, true, true, false, false, false,
                              /*nearbyZoneCount=*/0, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 11. DROPPED + leave zone → exit debounce → RESTORE → IDLE
// ---------------------------------------------------------------------------
void test_dropped_exit_debounce_restores() {
    PreQuietState state = droppedState();

    // Leave zone — start debounce.
    auto d1 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               0, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d1.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);

    // 200ms — still debouncing.
    auto d2 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               0, 0, 0, T0 + 200, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d2.action);

    // After debounce.
    auto d3 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               0, 0, 0, T0 + EXIT_WAIT, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d3.action);
    TEST_ASSERT_EQUAL(6, d3.volume);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 12. DISARMED does NOT re-drop when entering another zone
// ---------------------------------------------------------------------------
void test_disarmed_does_not_redrop_on_reentry() {
    PreQuietState state;
    state.phase = PreQuietPhase::DISARMED;

    // Leave zone → IDLE.
    evaluatePreQuiet(true, true, true, true, false, false, false,
                     0, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);

    // Enter new zone → should debounce and DROP normally (re-armed from IDLE).
    evaluatePreQuiet(true, true, true, true, false, false, false,
                     1, 6, 0, T0 + 100, state);
    auto d = evaluatePreQuiet(true, true, true, true, false, false, false,
                              1, 6, 0, T0 + 100 + ENTRY_WAIT, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::DROP_VOLUME, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
}

// ---------------------------------------------------------------------------
// 13. Feature disabled while DROPPED → RESTORE
// ---------------------------------------------------------------------------
void test_feature_disabled_while_dropped_restores() {
    PreQuietState state = droppedState();

    auto d = evaluatePreQuiet(false, true, true, true, false, false, false,
                              2, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d.action);
    TEST_ASSERT_EQUAL(6, d.volume);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 14. Feature disabled while DISARMED → NONE (already at normal volume)
// ---------------------------------------------------------------------------
void test_feature_disabled_while_disarmed_no_command() {
    PreQuietState state;
    state.phase = PreQuietPhase::DISARMED;
    state.savedMainVolume = 6;

    auto d = evaluatePreQuiet(false, true, true, true, false, false, false,
                              2, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 15. BLE reconnect after disconnect resumes DROPPED correctly
// ---------------------------------------------------------------------------
void test_ble_reconnect_resumes_dropped() {
    PreQuietState state = droppedState();

    // BLE drops.
    auto d1 = evaluatePreQuiet(true, true, false, true, false, false, false,
                               3, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d1.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);

    // BLE back, still in zone → hold.
    auto d2 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               3, 0, 0, T0 + 100, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d2.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
}

// ---------------------------------------------------------------------------
// 16. GPS fix lost while DROPPED → holds state, cancels exit debounce
// ---------------------------------------------------------------------------
void test_gps_lost_while_dropped_holds_state() {
    PreQuietState state = droppedState();

    // Start exit debounce (leave zone with GPS valid).
    auto d1 = evaluatePreQuiet(true, true, true, /*gpsValid=*/true,
                               false, false, false,
                               0, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d1.action);
    TEST_ASSERT_NOT_EQUAL(0, state.leftZoneMs);  // Debounce started

    // GPS drops — should hold DROPPED and cancel exit debounce.
    auto d2 = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                               false, false, false,
                               0, 0, 0, T0 + EXIT_WAIT, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d2.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
    TEST_ASSERT_EQUAL(0, state.leftZoneMs);  // Debounce cancelled

    // GPS returns, still in zone → hold.
    auto d3 = evaluatePreQuiet(true, true, true, /*gpsValid=*/true,
                               false, false, false,
                               2, 0, 0, T0 + EXIT_WAIT + 100, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d3.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
}

// ---------------------------------------------------------------------------
// 17. GPS fix lost while DISARMED → holds DISARMED
// ---------------------------------------------------------------------------
void test_gps_lost_while_disarmed_holds_state() {
    PreQuietState state;
    state.phase = PreQuietPhase::DISARMED;

    // GPS drops — nearbyCount=0 but gpsValid=false, should NOT clear to IDLE.
    auto d = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                              false, false, false,
                              0, 6, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DISARMED, state.phase);

    // GPS returns, actually left zone → IDLE.
    auto d2 = evaluatePreQuiet(true, true, true, /*gpsValid=*/true,
                               false, false, false,
                               0, 6, 0, T0 + 100, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d2.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 18. GPS flicker during DROPPED does not cause flip-flop
// ---------------------------------------------------------------------------
void test_gps_flicker_no_flipflop() {
    PreQuietState state = droppedState();

    // GPS drops repeatedly over 2 seconds — should never restore.
    for (uint32_t t = 0; t < 2000; t += 100) {
        auto d = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                                  false, false, false,
                                  0, 0, 0, T0 + t, state);
        TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
        TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
    }

    // GPS returns, still in zone — still DROPPED
    auto d = evaluatePreQuiet(true, true, true, /*gpsValid=*/true,
                              false, false, false,
                              3, 0, 0, T0 + 2000, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
}

// ---------------------------------------------------------------------------
// 19. DROPPED + GPS lost + real alert → RESTORE (safety-critical path)
// ---------------------------------------------------------------------------
void test_gps_lost_real_alert_still_restores() {
    PreQuietState state = droppedState();

    // GPS is down, but V1 fires a real alert — must restore volume.
    auto d = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                              /*hasAlert=*/true, /*lockoutEvaluated=*/true,
                              /*lockoutShouldMute=*/false,
                              0, 0, 0, T0, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d.action);
    TEST_ASSERT_EQUAL(6, d.volume);
    TEST_ASSERT_EQUAL(0, d.muteVolume);
    TEST_ASSERT_EQUAL(PreQuietPhase::DISARMED, state.phase);
}

// ---------------------------------------------------------------------------
// 20. Full cycle: IDLE → DROP → lockout alert → real alert → DISARMED → IDLE
// ---------------------------------------------------------------------------
void test_full_lifecycle() {
    PreQuietState state;

    // Enter zone, debounce, DROP.
    evaluatePreQuiet(true, true, true, true, false, false, false,
                     2, 6, 0, T0, state);
    auto d1 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               2, 6, 0, T0 + ENTRY_WAIT, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::DROP_VOLUME, d1.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);

    // Lockout alert — NONE.
    auto d2 = evaluatePreQuiet(true, true, true, true, true, true, true,
                               2, 0, 0, T0 + 500, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d2.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);

    // Lockout alert clears, still in zone — hold DROPPED.
    auto d3 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               2, 0, 0, T0 + 800, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d3.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);

    // Real alert — RESTORE → DISARMED.
    auto d4 = evaluatePreQuiet(true, true, true, true, true, true, false,
                               2, 0, 0, T0 + 1000, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d4.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DISARMED, state.phase);

    // Still in zone — stay DISARMED, no BLE.
    auto d5 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               2, 6, 0, T0 + 1200, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d5.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DISARMED, state.phase);

    // Leave zone — IDLE.
    auto d6 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               0, 6, 0, T0 + 2000, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d6.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 21. Safety timeout: DROPPED > 60s restores unconditionally
// ---------------------------------------------------------------------------
void test_dropped_safety_timeout_restores() {
    // Set up DROPPED state that started at T0.
    PreQuietState state = droppedState(6, 0, T0);

    // Just under 60 seconds — still DROPPED.
    uint32_t justBefore = T0 + 60UL * 1000UL - 1;
    auto d1 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               3, 0, 0, justBefore, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d1.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);

    // Exactly 60 seconds — restores.
    uint32_t atTimeout = T0 + 60UL * 1000UL;
    auto d2 = evaluatePreQuiet(true, true, true, true, false, false, false,
                               3, 0, 0, atTimeout, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d2.action);
    TEST_ASSERT_EQUAL(6, d2.volume);
    TEST_ASSERT_EQUAL(0, d2.muteVolume);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 22. Safety timeout fires even with GPS lost
// ---------------------------------------------------------------------------
void test_dropped_safety_timeout_gps_lost() {
    PreQuietState state = droppedState(6, 0, T0);

    // GPS lost + 60 seconds elapsed → still restores.
    uint32_t atTimeout = T0 + 60UL * 1000UL;
    auto d = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                              false, false, false,
                              0, 0, 0, atTimeout, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d.action);
    TEST_ASSERT_EQUAL(6, d.volume);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 23. Ka alert while DROPPED restores volume (even lockout-matched)
// ---------------------------------------------------------------------------
void test_ka_alert_while_dropped_restores() {
    PreQuietState state = droppedState(6, 0, T0);

    // Ka detected + lockout-matched → still restores (safety-critical).
    auto d = evaluatePreQuiet(true, true, true, true,
                              /*hasAlert=*/true, /*lockoutEvaluated=*/true,
                              /*lockoutShouldMute=*/true,
                              2, 0, 0, T0 + 500, state,
                              /*hasKaOrLaser=*/true);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d.action);
    TEST_ASSERT_EQUAL(6, d.volume);
    TEST_ASSERT_EQUAL(0, d.muteVolume);
    TEST_ASSERT_EQUAL(PreQuietPhase::DISARMED, state.phase);
}

// ---------------------------------------------------------------------------
// 24. Laser alert while DROPPED restores volume
// ---------------------------------------------------------------------------
void test_laser_alert_while_dropped_restores() {
    PreQuietState state = droppedState(6, 0, T0);

    // Laser doesn't have lockout match, but test the band flag.
    auto d = evaluatePreQuiet(true, true, true, true,
                              /*hasAlert=*/true, /*lockoutEvaluated=*/true,
                              /*lockoutShouldMute=*/false,
                              2, 0, 0, T0 + 200, state,
                              /*hasKaOrLaser=*/true);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d.action);
    TEST_ASSERT_EQUAL(6, d.volume);
    TEST_ASSERT_EQUAL(PreQuietPhase::DISARMED, state.phase);
}

// ---------------------------------------------------------------------------
// 25. GPS lost >5s while DROPPED restores volume
// ---------------------------------------------------------------------------
void test_gps_lost_5s_while_dropped_restores() {
    PreQuietState state = droppedState(6, 0, T0);

    // GPS drops — first frame starts the timer.
    auto d1 = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                               false, false, false,
                               0, 0, 0, T0 + 1000, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d1.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
    TEST_ASSERT_NOT_EQUAL(0, state.gpsLostMs);

    // 4 seconds later — still DROPPED.
    auto d2 = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                               false, false, false,
                               0, 0, 0, T0 + 5000, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d2.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);

    // 5 seconds after GPS loss — restores.
    auto d3 = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                               false, false, false,
                               0, 0, 0, T0 + 6000, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::RESTORE_VOLUME, d3.action);
    TEST_ASSERT_EQUAL(6, d3.volume);
    TEST_ASSERT_EQUAL(PreQuietPhase::IDLE, state.phase);
}

// ---------------------------------------------------------------------------
// 26. GPS flicker <5s resets the loss timer
// ---------------------------------------------------------------------------
void test_gps_brief_loss_resets_timer() {
    PreQuietState state = droppedState(6, 0, T0);

    // GPS drops for 3s.
    auto d1 = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                               false, false, false,
                               0, 0, 0, T0 + 1000, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d1.action);

    // GPS returns briefly, still in zone.
    auto d2 = evaluatePreQuiet(true, true, true, /*gpsValid=*/true,
                               false, false, false,
                               3, 0, 0, T0 + 4000, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d2.action);
    TEST_ASSERT_EQUAL(0, state.gpsLostMs);  // Timer reset

    // GPS drops again — new 5s timer starts.
    auto d3 = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                               false, false, false,
                               0, 0, 0, T0 + 4500, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d3.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);

    // Only 3s since new loss — still holds.
    auto d4 = evaluatePreQuiet(true, true, true, /*gpsValid=*/false,
                               false, false, false,
                               0, 0, 0, T0 + 7500, state);
    TEST_ASSERT_EQUAL(PreQuietDecision::NONE, d4.action);
    TEST_ASSERT_EQUAL(PreQuietPhase::DROPPED, state.phase);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_feature_disabled_returns_none);
    RUN_TEST(test_non_enforce_mode_returns_none);
    RUN_TEST(test_ble_disconnected_preserves_state);
    RUN_TEST(test_entry_debounce_waits);
    RUN_TEST(test_entry_drops_volume_after_debounce);
    RUN_TEST(test_dropped_in_zone_holds);
    RUN_TEST(test_dropped_lockout_match_stays_quiet);
    RUN_TEST(test_real_alert_restores_to_disarmed);
    RUN_TEST(test_disarmed_stays_in_zone);
    RUN_TEST(test_disarmed_leaves_zone_goes_idle);
    RUN_TEST(test_dropped_exit_debounce_restores);
    RUN_TEST(test_disarmed_does_not_redrop_on_reentry);
    RUN_TEST(test_feature_disabled_while_dropped_restores);
    RUN_TEST(test_feature_disabled_while_disarmed_no_command);
    RUN_TEST(test_ble_reconnect_resumes_dropped);
    RUN_TEST(test_gps_lost_while_dropped_holds_state);
    RUN_TEST(test_gps_lost_while_disarmed_holds_state);
    RUN_TEST(test_gps_flicker_no_flipflop);
    RUN_TEST(test_gps_lost_real_alert_still_restores);
    RUN_TEST(test_full_lifecycle);
    RUN_TEST(test_dropped_safety_timeout_restores);
    RUN_TEST(test_dropped_safety_timeout_gps_lost);
    RUN_TEST(test_ka_alert_while_dropped_restores);
    RUN_TEST(test_laser_alert_while_dropped_restores);
    RUN_TEST(test_gps_lost_5s_while_dropped_restores);
    RUN_TEST(test_gps_brief_loss_resets_timer);
    return UNITY_END();
}

#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"

#include "../../src/modules/lockout/lockout_runtime_mute_controller.h"
#include "../../src/modules/lockout/lockout_runtime_mute_controller.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

void setUp() {}
void tearDown() {}

static LockoutEnforcerResult enforceMuteResult() {
    LockoutEnforcerResult r;
    r.evaluated = true;
    r.shouldMute = true;
    r.mode = static_cast<uint8_t>(LOCKOUT_RUNTIME_ENFORCE);
    return r;
}

static GpsLockoutCoreGuardStatus guardStatus(bool tripped, const char* reason = "none") {
    GpsLockoutCoreGuardStatus g;
    g.enabled = true;
    g.tripped = tripped;
    g.reason = reason;
    return g;
}

void test_enforce_sends_mute_once_per_lockout_cycle() {
    LockoutRuntimeMuteState state;
    const LockoutEnforcerResult lockRes = enforceMuteResult();
    const GpsLockoutCoreGuardStatus guard = guardStatus(false);

    LockoutRuntimeMuteDecision first =
        evaluateLockoutRuntimeMute(lockRes, guard, true, false, false, state);
    TEST_ASSERT_TRUE(first.sendMute);
    TEST_ASSERT_FALSE(first.sendUnmute);
    TEST_ASSERT_FALSE(first.logGuardBlocked);
    TEST_ASSERT_TRUE(state.lockoutMuteActive);
    TEST_ASSERT_FALSE(state.lockoutGuardBlockedLogged);

    LockoutRuntimeMuteDecision second =
        evaluateLockoutRuntimeMute(lockRes, guard, true, true, false, state);
    TEST_ASSERT_FALSE(second.sendMute);
    TEST_ASSERT_FALSE(second.sendUnmute);
    TEST_ASSERT_FALSE(second.logGuardBlocked);
    TEST_ASSERT_TRUE(state.lockoutMuteActive);
}

void test_enforce_guard_blocked_logs_once() {
    LockoutRuntimeMuteState state;
    const LockoutEnforcerResult lockRes = enforceMuteResult();
    const GpsLockoutCoreGuardStatus guard = guardStatus(true, "queueDrops");

    LockoutRuntimeMuteDecision first =
        evaluateLockoutRuntimeMute(lockRes, guard, true, false, false, state);
    TEST_ASSERT_FALSE(first.sendMute);
    TEST_ASSERT_FALSE(first.sendUnmute);
    TEST_ASSERT_TRUE(first.logGuardBlocked);
    TEST_ASSERT_FALSE(state.lockoutMuteActive);
    TEST_ASSERT_TRUE(state.lockoutGuardBlockedLogged);

    LockoutRuntimeMuteDecision second =
        evaluateLockoutRuntimeMute(lockRes, guard, true, false, false, state);
    TEST_ASSERT_FALSE(second.sendMute);
    TEST_ASSERT_FALSE(second.sendUnmute);
    TEST_ASSERT_FALSE(second.logGuardBlocked);
    TEST_ASSERT_TRUE(state.lockoutGuardBlockedLogged);
}

void test_leaving_lockout_resets_state_and_rearms() {
    LockoutRuntimeMuteState state;
    state.lockoutMuteActive = true;
    state.lockoutGuardBlockedLogged = true;
    state.muteWasActiveBeforeLockout = false;

    LockoutEnforcerResult clearRes;
    clearRes.shouldMute = false;
    clearRes.mode = static_cast<uint8_t>(LOCKOUT_RUNTIME_ENFORCE);

    LockoutRuntimeMuteDecision clearDecision =
        evaluateLockoutRuntimeMute(clearRes, guardStatus(false), true, false, false, state);
    TEST_ASSERT_FALSE(clearDecision.sendMute);
    TEST_ASSERT_TRUE(clearDecision.sendUnmute);
    TEST_ASSERT_FALSE(clearDecision.logGuardBlocked);
    TEST_ASSERT_FALSE(state.lockoutMuteActive);
    TEST_ASSERT_FALSE(state.lockoutGuardBlockedLogged);
    TEST_ASSERT_FALSE(state.muteWasActiveBeforeLockout);

    LockoutRuntimeMuteDecision rearmDecision =
        evaluateLockoutRuntimeMute(enforceMuteResult(), guardStatus(false), true, false, false, state);
    TEST_ASSERT_TRUE(rearmDecision.sendMute);
    TEST_ASSERT_FALSE(rearmDecision.sendUnmute);
    TEST_ASSERT_FALSE(rearmDecision.logGuardBlocked);
    TEST_ASSERT_TRUE(state.lockoutMuteActive);
}

void test_non_enforce_modes_do_not_send_or_log() {
    LockoutRuntimeMuteState state;
    LockoutEnforcerResult lockRes;
    lockRes.evaluated = true;
    lockRes.shouldMute = true;
    lockRes.mode = static_cast<uint8_t>(LOCKOUT_RUNTIME_SHADOW);

    LockoutRuntimeMuteDecision d =
        evaluateLockoutRuntimeMute(lockRes, guardStatus(true, "queueDrops"), true, false, false, state);
    TEST_ASSERT_FALSE(d.sendMute);
    TEST_ASSERT_FALSE(d.sendUnmute);
    TEST_ASSERT_FALSE(d.logGuardBlocked);
    TEST_ASSERT_FALSE(state.lockoutMuteActive);
    TEST_ASSERT_FALSE(state.lockoutGuardBlockedLogged);
}

void test_enforce_requires_ble_connection_before_send() {
    LockoutRuntimeMuteState state;
    const LockoutEnforcerResult lockRes = enforceMuteResult();
    const GpsLockoutCoreGuardStatus guard = guardStatus(false);

    LockoutRuntimeMuteDecision disconnected =
        evaluateLockoutRuntimeMute(lockRes, guard, false, false, false, state);
    TEST_ASSERT_FALSE(disconnected.sendMute);
    TEST_ASSERT_FALSE(disconnected.sendUnmute);
    TEST_ASSERT_FALSE(disconnected.logGuardBlocked);
    TEST_ASSERT_FALSE(state.lockoutMuteActive);

    LockoutRuntimeMuteDecision connected =
        evaluateLockoutRuntimeMute(lockRes, guard, true, false, false, state);
    TEST_ASSERT_TRUE(connected.sendMute);
    TEST_ASSERT_FALSE(connected.sendUnmute);
    TEST_ASSERT_FALSE(connected.logGuardBlocked);
    TEST_ASSERT_TRUE(state.lockoutMuteActive);
}

void test_exit_does_not_unmute_if_already_muted_before_lockout() {
    LockoutRuntimeMuteState state;
    state.lockoutMuteActive = true;
    state.muteWasActiveBeforeLockout = true;

    LockoutEnforcerResult clearRes;
    clearRes.evaluated = true;
    clearRes.shouldMute = false;
    clearRes.mode = static_cast<uint8_t>(LOCKOUT_RUNTIME_ENFORCE);

    LockoutRuntimeMuteDecision d =
        evaluateLockoutRuntimeMute(clearRes, guardStatus(false), true, true, false, state);
    TEST_ASSERT_FALSE(d.sendMute);
    TEST_ASSERT_FALSE(d.sendUnmute);
    TEST_ASSERT_FALSE(state.lockoutMuteActive);
    TEST_ASSERT_FALSE(state.muteWasActiveBeforeLockout);
}

void test_override_band_blocks_mute_and_releases_lockout_mute() {
    LockoutRuntimeMuteState state;
    const LockoutEnforcerResult lockRes = enforceMuteResult();
    const GpsLockoutCoreGuardStatus guard = guardStatus(false);

    LockoutRuntimeMuteDecision blocked =
        evaluateLockoutRuntimeMute(lockRes, guard, true, false, true, state);
    TEST_ASSERT_FALSE(blocked.sendMute);
    TEST_ASSERT_FALSE(blocked.sendUnmute);
    TEST_ASSERT_FALSE(state.lockoutMuteActive);

    LockoutRuntimeMuteDecision arm =
        evaluateLockoutRuntimeMute(lockRes, guard, true, false, false, state);
    TEST_ASSERT_TRUE(arm.sendMute);
    TEST_ASSERT_FALSE(arm.sendUnmute);
    TEST_ASSERT_TRUE(state.lockoutMuteActive);

    LockoutRuntimeMuteDecision released =
        evaluateLockoutRuntimeMute(lockRes, guard, true, true, true, state);
    TEST_ASSERT_FALSE(released.sendMute);
    TEST_ASSERT_TRUE(released.sendUnmute);
    TEST_ASSERT_FALSE(state.lockoutMuteActive);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_enforce_sends_mute_once_per_lockout_cycle);
    RUN_TEST(test_enforce_guard_blocked_logs_once);
    RUN_TEST(test_leaving_lockout_resets_state_and_rearms);
    RUN_TEST(test_non_enforce_modes_do_not_send_or_log);
    RUN_TEST(test_enforce_requires_ble_connection_before_send);
    RUN_TEST(test_exit_does_not_unmute_if_already_muted_before_lockout);
    RUN_TEST(test_override_band_blocks_mute_and_releases_lockout_mute);
    return UNITY_END();
}

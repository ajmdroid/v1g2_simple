#include <unity.h>

#include "../../src/modules/obd/obd_state_policy.h"

using namespace ObdStatePolicy;

void setUp() {}
void tearDown() {}

void test_retry_delay_progression_and_cap() {
    TEST_ASSERT_EQUAL_UINT32(3000u, computeRetryDelayMs(0, 3000u, 30000u));
    TEST_ASSERT_EQUAL_UINT32(6000u, computeRetryDelayMs(1, 3000u, 30000u));
    TEST_ASSERT_EQUAL_UINT32(12000u, computeRetryDelayMs(2, 3000u, 30000u));
    TEST_ASSERT_EQUAL_UINT32(24000u, computeRetryDelayMs(3, 3000u, 30000u));
    TEST_ASSERT_EQUAL_UINT32(30000u, computeRetryDelayMs(4, 3000u, 30000u));
    TEST_ASSERT_EQUAL_UINT32(30000u, computeRetryDelayMs(5, 3000u, 30000u));
}

void test_cooldown_progression_and_cap() {
    TEST_ASSERT_EQUAL_UINT32(60000u, computeReconnectCooldownMs(0, 300000u));
    TEST_ASSERT_EQUAL_UINT32(120000u, computeReconnectCooldownMs(1, 300000u));
    TEST_ASSERT_EQUAL_UINT32(240000u, computeReconnectCooldownMs(2, 300000u));
    TEST_ASSERT_EQUAL_UINT32(300000u, computeReconnectCooldownMs(3, 300000u));
    TEST_ASSERT_EQUAL_UINT32(300000u, computeReconnectCooldownMs(9, 300000u));
}

void test_disconnected_no_target_is_noop() {
    const DisconnectedDecision d = evaluateDisconnected(
        false,  // hasTargetDevice
        2,      // connectionFailures
        5,      // maxConnectionFailures
        1,      // reconnectCycleCount
        999999u,
        3000u,
        30000u,
        300000u);

    TEST_ASSERT_FALSE(d.transitionToIdle);
    TEST_ASSERT_FALSE(d.transitionToConnecting);
    TEST_ASSERT_FALSE(d.clearTargetDevice);
    TEST_ASSERT_FALSE(d.resetConnectionFailures);
    TEST_ASSERT_EQUAL_UINT8(1u, d.nextReconnectCycleCount);
    TEST_ASSERT_EQUAL_UINT32(0u, d.waitThresholdMs);
}

void test_disconnected_reconnect_waits_for_retry_delay() {
    const DisconnectedDecision before = evaluateDisconnected(
        true,   // hasTargetDevice
        2,      // connectionFailures
        5,      // maxConnectionFailures
        0,      // reconnectCycleCount
        11999u, // elapsed
        3000u,
        30000u,
        300000u);

    TEST_ASSERT_FALSE(before.transitionToIdle);
    TEST_ASSERT_FALSE(before.transitionToConnecting);
    TEST_ASSERT_EQUAL_UINT32(12000u, before.waitThresholdMs);

    const DisconnectedDecision at = evaluateDisconnected(
        true,   // hasTargetDevice
        2,      // connectionFailures
        5,      // maxConnectionFailures
        0,      // reconnectCycleCount
        12000u, // elapsed
        3000u,
        30000u,
        300000u);

    TEST_ASSERT_FALSE(at.transitionToIdle);
    TEST_ASSERT_TRUE(at.transitionToConnecting);
    TEST_ASSERT_EQUAL_UINT32(12000u, at.waitThresholdMs);
}

void test_disconnected_cooldown_then_idle_with_resets() {
    const DisconnectedDecision before = evaluateDisconnected(
        true,     // hasTargetDevice
        5,        // connectionFailures
        5,        // maxConnectionFailures
        2,        // reconnectCycleCount
        239999u,  // elapsed
        3000u,
        30000u,
        300000u);

    TEST_ASSERT_FALSE(before.transitionToIdle);
    TEST_ASSERT_FALSE(before.transitionToConnecting);
    TEST_ASSERT_EQUAL_UINT32(240000u, before.waitThresholdMs);

    const DisconnectedDecision at = evaluateDisconnected(
        true,     // hasTargetDevice
        5,        // connectionFailures
        5,        // maxConnectionFailures
        2,        // reconnectCycleCount
        240000u,  // elapsed
        3000u,
        30000u,
        300000u);

    TEST_ASSERT_TRUE(at.transitionToIdle);
    TEST_ASSERT_FALSE(at.transitionToConnecting);
    TEST_ASSERT_TRUE(at.clearTargetDevice);
    TEST_ASSERT_TRUE(at.resetConnectionFailures);
    TEST_ASSERT_EQUAL_UINT8(3u, at.nextReconnectCycleCount);
    TEST_ASSERT_EQUAL_UINT32(240000u, at.waitThresholdMs);
}

void test_cooldown_cycle_counter_caps_at_ten() {
    TEST_ASSERT_EQUAL_UINT8(10u, bumpReconnectCycle(10u));
    TEST_ASSERT_EQUAL_UINT8(10u, bumpReconnectCycle(11u));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_retry_delay_progression_and_cap);
    RUN_TEST(test_cooldown_progression_and_cap);
    RUN_TEST(test_disconnected_no_target_is_noop);
    RUN_TEST(test_disconnected_reconnect_waits_for_retry_delay);
    RUN_TEST(test_disconnected_cooldown_then_idle_with_resets);
    RUN_TEST(test_cooldown_cycle_counter_caps_at_ten);
    return UNITY_END();
}

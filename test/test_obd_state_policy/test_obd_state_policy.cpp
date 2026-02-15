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

// ─────────────────────────────────────────────────────────────────────
// ObdConnectPolicy tests
// ─────────────────────────────────────────────────────────────────────
using namespace ObdConnectPolicy;

// --- shouldIdleAutoConnect ---

void test_idle_autoconnect_blocked_when_link_not_ready() {
    TEST_ASSERT_FALSE(shouldIdleAutoConnect(false, 10000, 5000, true));
}

void test_idle_autoconnect_blocked_before_retry_interval() {
    TEST_ASSERT_FALSE(shouldIdleAutoConnect(true, 4999, 5000, true));
}

void test_idle_autoconnect_blocked_when_no_target() {
    TEST_ASSERT_FALSE(shouldIdleAutoConnect(true, 10000, 5000, false));
}

void test_idle_autoconnect_allowed_when_all_conditions_met() {
    TEST_ASSERT_TRUE(shouldIdleAutoConnect(true, 5000, 5000, true));
    TEST_ASSERT_TRUE(shouldIdleAutoConnect(true, 99999, 5000, true));
}

// --- shouldProceedWithConnect ---

void test_proceed_connect_blocked_without_target() {
    TEST_ASSERT_FALSE(shouldProceedWithConnect(true, false));
}

void test_proceed_connect_blocked_without_link() {
    TEST_ASSERT_FALSE(shouldProceedWithConnect(false, true));
}

void test_proceed_connect_allowed_with_link_and_target() {
    TEST_ASSERT_TRUE(shouldProceedWithConnect(true, true));
}

// --- shouldTryAutoConnect ---

void test_try_autoconnect_blocked_in_active_states() {
    TEST_ASSERT_FALSE(shouldTryAutoConnect(State::CONNECTING, true));
    TEST_ASSERT_FALSE(shouldTryAutoConnect(State::INITIALIZING, true));
    TEST_ASSERT_FALSE(shouldTryAutoConnect(State::READY, true));
    TEST_ASSERT_FALSE(shouldTryAutoConnect(State::POLLING, true));
    TEST_ASSERT_FALSE(shouldTryAutoConnect(State::SCANNING, true));
}

void test_try_autoconnect_blocked_without_link() {
    TEST_ASSERT_FALSE(shouldTryAutoConnect(State::IDLE, false));
    TEST_ASSERT_FALSE(shouldTryAutoConnect(State::DISCONNECTED, false));
}

void test_try_autoconnect_allowed_from_idle_with_link() {
    TEST_ASSERT_TRUE(shouldTryAutoConnect(State::IDLE, true));
}

void test_try_autoconnect_allowed_from_disconnected_with_link() {
    TEST_ASSERT_TRUE(shouldTryAutoConnect(State::DISCONNECTED, true));
}

void test_try_autoconnect_allowed_from_failed_with_link() {
    TEST_ASSERT_TRUE(shouldTryAutoConnect(State::FAILED, true));
}

// --- shouldActivateWifiPriorityForObd ---

void test_wifi_priority_off_when_wifi_ap_off() {
    // WiFi AP off → never activate, even if OBD is connecting.
    TEST_ASSERT_FALSE(shouldActivateWifiPriorityForObd(false, true, false, State::CONNECTING));
    TEST_ASSERT_FALSE(shouldActivateWifiPriorityForObd(false, true, true, State::INITIALIZING));
}

void test_wifi_priority_off_when_obd_disabled() {
    TEST_ASSERT_FALSE(shouldActivateWifiPriorityForObd(true, false, false, State::CONNECTING));
}

void test_wifi_priority_off_when_obd_idle() {
    TEST_ASSERT_FALSE(shouldActivateWifiPriorityForObd(true, true, false, State::IDLE));
    TEST_ASSERT_FALSE(shouldActivateWifiPriorityForObd(true, true, false, State::POLLING));
    TEST_ASSERT_FALSE(shouldActivateWifiPriorityForObd(true, true, false, State::READY));
}

void test_wifi_priority_on_when_wifi_ap_on_and_obd_connecting() {
    TEST_ASSERT_TRUE(shouldActivateWifiPriorityForObd(true, true, false, State::CONNECTING));
}

void test_wifi_priority_on_when_wifi_ap_on_and_obd_initializing() {
    TEST_ASSERT_TRUE(shouldActivateWifiPriorityForObd(true, true, false, State::INITIALIZING));
}

void test_wifi_priority_on_when_wifi_ap_on_and_obd_scanning() {
    TEST_ASSERT_TRUE(shouldActivateWifiPriorityForObd(true, true, true, State::IDLE));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_retry_delay_progression_and_cap);
    RUN_TEST(test_cooldown_progression_and_cap);
    RUN_TEST(test_disconnected_no_target_is_noop);
    RUN_TEST(test_disconnected_reconnect_waits_for_retry_delay);
    RUN_TEST(test_disconnected_cooldown_then_idle_with_resets);
    RUN_TEST(test_cooldown_cycle_counter_caps_at_ten);

    // ObdConnectPolicy – idle auto-connect
    RUN_TEST(test_idle_autoconnect_blocked_when_link_not_ready);
    RUN_TEST(test_idle_autoconnect_blocked_before_retry_interval);
    RUN_TEST(test_idle_autoconnect_blocked_when_no_target);
    RUN_TEST(test_idle_autoconnect_allowed_when_all_conditions_met);

    // ObdConnectPolicy – proceed with connect
    RUN_TEST(test_proceed_connect_blocked_without_target);
    RUN_TEST(test_proceed_connect_blocked_without_link);
    RUN_TEST(test_proceed_connect_allowed_with_link_and_target);

    // ObdConnectPolicy – try auto-connect
    RUN_TEST(test_try_autoconnect_blocked_in_active_states);
    RUN_TEST(test_try_autoconnect_blocked_without_link);
    RUN_TEST(test_try_autoconnect_allowed_from_idle_with_link);
    RUN_TEST(test_try_autoconnect_allowed_from_disconnected_with_link);
    RUN_TEST(test_try_autoconnect_allowed_from_failed_with_link);

    // ObdConnectPolicy – WiFi priority activation
    RUN_TEST(test_wifi_priority_off_when_wifi_ap_off);
    RUN_TEST(test_wifi_priority_off_when_obd_disabled);
    RUN_TEST(test_wifi_priority_off_when_obd_idle);
    RUN_TEST(test_wifi_priority_on_when_wifi_ap_on_and_obd_connecting);
    RUN_TEST(test_wifi_priority_on_when_wifi_ap_on_and_obd_initializing);
    RUN_TEST(test_wifi_priority_on_when_wifi_ap_on_and_obd_scanning);

    return UNITY_END();
}

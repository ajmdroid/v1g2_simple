#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/wifi/wifi_auto_start_module.cpp"

static WifiAutoStartModule module;
static int startCalls = 0;
static int markCalls = 0;
static bool wifiAutoStartDone = false;

static void resetState() {
    startCalls = 0;
    markCalls = 0;
    wifiAutoStartDone = false;
}

static void assertGate(WifiAutoStartGate expectedGate,
                       bool expectedShouldAutoStart,
                       bool expectedStartTriggered) {
    const WifiAutoStartDecisionSnapshot& snapshot = module.getLastDecision();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expectedGate), static_cast<uint8_t>(snapshot.gate));
    TEST_ASSERT_EQUAL(expectedShouldAutoStart, snapshot.shouldAutoStart);
    TEST_ASSERT_EQUAL(expectedStartTriggered, snapshot.startTriggered);
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_noop_when_feature_disabled() {
    const bool started = module.process(5000,
                                        1000,
                                        true,
                                        false,
                                        true,
                                        true,
                                        wifiAutoStartDone,
                                        [] { startCalls++; },
                                        [] { markCalls++; });

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    TEST_ASSERT_EQUAL_INT(0, markCalls);
    assertGate(WifiAutoStartGate::WifiAtBootDisabled, false, false);
    TEST_ASSERT_TRUE(module.getLastDecision().enableWifi);
    TEST_ASSERT_FALSE(module.getLastDecision().enableWifiAtBoot);
}

void test_noop_when_already_done() {
    wifiAutoStartDone = true;
    const bool started = module.process(5000,
                                        1000,
                                        true,
                                        true,
                                        true,
                                        true,
                                        wifiAutoStartDone,
                                        [] { startCalls++; },
                                        [] { markCalls++; });

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_TRUE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    TEST_ASSERT_EQUAL_INT(0, markCalls);
    assertGate(WifiAutoStartGate::AlreadyDone, false, false);
    TEST_ASSERT_TRUE(module.getLastDecision().wifiAutoStartDone);
}

void test_noop_before_ble_settle_and_timeout() {
    const bool started = module.process(2500,
                                        1000,
                                        true,
                                        true,
                                        true,
                                        true,
                                        wifiAutoStartDone,
                                        [] { startCalls++; },
                                        [] { markCalls++; });

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    TEST_ASSERT_EQUAL_INT(0, markCalls);
    assertGate(WifiAutoStartGate::WaitingBleSettle, false, false);
    TEST_ASSERT_TRUE(module.getLastDecision().bleConnected);
    TEST_ASSERT_FALSE(module.getLastDecision().bootTimeoutReached);
}

void test_starts_after_ble_settle() {
    const bool started = module.process(5000,
                                        1000,
                                        true,
                                        true,
                                        true,
                                        true,
                                        wifiAutoStartDone,
                                        [] { startCalls++; },
                                        [] { markCalls++; });

    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(1, startCalls);
    TEST_ASSERT_EQUAL_INT(1, markCalls);
    assertGate(WifiAutoStartGate::Starting, true, true);
    TEST_ASSERT_TRUE(module.getLastDecision().bleSettled);
}

void test_starts_on_boot_timeout_without_ble() {
    const bool started = module.process(30000,
                                        0,
                                        true,
                                        true,
                                        false,
                                        true,
                                        wifiAutoStartDone,
                                        [] { startCalls++; },
                                        [] { markCalls++; });

    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(1, startCalls);
    TEST_ASSERT_EQUAL_INT(1, markCalls);
    assertGate(WifiAutoStartGate::Starting, true, true);
    TEST_ASSERT_TRUE(module.getLastDecision().bootTimeoutReached);
}

void test_noop_when_dma_not_available() {
    const bool started = module.process(5000,
                                        1000,
                                        true,
                                        true,
                                        true,
                                        false,
                                        wifiAutoStartDone,
                                        [] { startCalls++; },
                                        [] { markCalls++; });

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    TEST_ASSERT_EQUAL_INT(0, markCalls);
    assertGate(WifiAutoStartGate::WaitingDma, false, false);
    TEST_ASSERT_TRUE(module.getLastDecision().bleSettled);
    TEST_ASSERT_FALSE(module.getLastDecision().canStartDma);
}

void test_v1_timestamp_ahead_of_now_saturates_elapsed() {
    const bool started = module.process(1000,
                                        2000,
                                        true,
                                        true,
                                        true,
                                        true,
                                        wifiAutoStartDone,
                                        [] { startCalls++; },
                                        [] { markCalls++; });

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    TEST_ASSERT_EQUAL_INT(0, markCalls);
    assertGate(WifiAutoStartGate::WaitingBleSettle, false, false);
    TEST_ASSERT_EQUAL_UINT32(0, module.getLastDecision().msSinceV1Connect);
}

void test_noop_when_wifi_master_disabled() {
    const bool started = module.process(5000,
                                        1000,
                                        false,
                                        true,
                                        true,
                                        true,
                                        wifiAutoStartDone,
                                        [] { startCalls++; },
                                        [] { markCalls++; });

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    TEST_ASSERT_EQUAL_INT(0, markCalls);
    assertGate(WifiAutoStartGate::WifiDisabled, false, false);
    TEST_ASSERT_FALSE(module.getLastDecision().enableWifi);
}

void test_waits_for_boot_timeout_without_ble_connection() {
    const bool started = module.process(10000,
                                        0,
                                        true,
                                        true,
                                        false,
                                        true,
                                        wifiAutoStartDone,
                                        [] { startCalls++; },
                                        [] { markCalls++; });

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    TEST_ASSERT_EQUAL_INT(0, markCalls);
    assertGate(WifiAutoStartGate::WaitingBootTimeout, false, false);
    TEST_ASSERT_FALSE(module.getLastDecision().bleConnected);
    TEST_ASSERT_FALSE(module.getLastDecision().bootTimeoutReached);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_noop_when_feature_disabled);
    RUN_TEST(test_noop_when_already_done);
    RUN_TEST(test_noop_before_ble_settle_and_timeout);
    RUN_TEST(test_starts_after_ble_settle);
    RUN_TEST(test_starts_on_boot_timeout_without_ble);
    RUN_TEST(test_noop_when_dma_not_available);
    RUN_TEST(test_v1_timestamp_ahead_of_now_saturates_elapsed);
    RUN_TEST(test_noop_when_wifi_master_disabled);
    RUN_TEST(test_waits_for_boot_timeout_without_ble_connection);
    return UNITY_END();
}

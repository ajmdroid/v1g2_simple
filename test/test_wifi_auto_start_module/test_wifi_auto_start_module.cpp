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

void setUp() {
    resetState();
}

void tearDown() {}

void test_noop_when_feature_disabled() {
    const bool started = module.process(5000,
                                        1000,
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
}

void test_noop_when_already_done() {
    wifiAutoStartDone = true;
    const bool started = module.process(5000,
                                        1000,
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
}

void test_noop_before_ble_settle_and_timeout() {
    const bool started = module.process(2500,
                                        1000,
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
}

void test_starts_after_ble_settle() {
    const bool started = module.process(5000,
                                        1000,
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
}

void test_starts_on_boot_timeout_without_ble() {
    const bool started = module.process(30000,
                                        0,
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
}

void test_noop_when_dma_not_available() {
    const bool started = module.process(5000,
                                        1000,
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
}

void test_v1_timestamp_ahead_of_now_saturates_elapsed() {
    const bool started = module.process(1000,
                                        2000,
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
    return UNITY_END();
}

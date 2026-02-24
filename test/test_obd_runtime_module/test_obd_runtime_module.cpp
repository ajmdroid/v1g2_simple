#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/obd_handler.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/speed/speed_source_selector.cpp"
#include "../../src/modules/obd/obd_runtime_module.cpp"

static OBDHandler obdHandler;
static SpeedSourceSelector speedSelector;
static ObdRuntimeModule module;

void setUp() {
    obdHandler = OBDHandler{};
    speedSelector = SpeedSourceSelector{};
    module.reset();
    speedSelector.begin(false);
}

void tearDown() {}

void test_disabled_mode_latches_disconnect_and_clears_pending() {
    bool pending = true;

    module.process(1000, false, pending, 0, obdHandler, speedSelector);

    TEST_ASSERT_FALSE(pending);
    TEST_ASSERT_EQUAL_INT(1, obdHandler.stopScanCalls);
    TEST_ASSERT_EQUAL_INT(1, obdHandler.disconnectCalls);

    pending = true;
    module.process(2000, false, pending, 0, obdHandler, speedSelector);

    TEST_ASSERT_FALSE(pending);
    TEST_ASSERT_EQUAL_INT(1, obdHandler.stopScanCalls);
    TEST_ASSERT_EQUAL_INT(1, obdHandler.disconnectCalls);
}

void test_autoconnect_triggers_when_due() {
    bool pending = true;

    module.process(1499, true, pending, 1500, obdHandler, speedSelector);
    TEST_ASSERT_TRUE(pending);
    TEST_ASSERT_EQUAL_INT(0, obdHandler.tryAutoConnectCalls);

    module.process(1500, true, pending, 1500, obdHandler, speedSelector);
    TEST_ASSERT_FALSE(pending);
    TEST_ASSERT_EQUAL_INT(1, obdHandler.tryAutoConnectCalls);
}

void test_update_true_forwards_obd_sample_and_connected_state() {
    bool pending = false;
    OBDData sample;
    sample.speed_mph = 37.5f;
    sample.timestamp_ms = 1200;
    sample.valid = true;

    obdHandler.setData(sample);
    obdHandler.updateReturn = true;
    obdHandler.setConnected(true);

    module.process(1300, true, pending, 0, obdHandler, speedSelector);

    const auto status = speedSelector.snapshot(1300);
    TEST_ASSERT_TRUE(status.obdConnected);
    TEST_ASSERT_TRUE(status.obdFresh);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 37.5f, status.obdSpeedMph);
    TEST_ASSERT_EQUAL_UINT32(100, status.obdAgeMs);
}

void test_update_false_does_not_publish_new_sample() {
    bool pending = false;
    obdHandler.updateReturn = false;

    module.process(2200, true, pending, 0, obdHandler, speedSelector);

    const auto status = speedSelector.snapshot(2200);
    TEST_ASSERT_FALSE(status.obdFresh);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, status.obdAgeMs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_disabled_mode_latches_disconnect_and_clears_pending);
    RUN_TEST(test_autoconnect_triggers_when_due);
    RUN_TEST(test_update_true_forwards_obd_sample_and_connected_state);
    RUN_TEST(test_update_false_does_not_publish_new_sample);
    return UNITY_END();
}

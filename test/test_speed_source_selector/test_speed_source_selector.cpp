#include <unity.h>

#include "../../src/modules/speed/speed_source_selector.h"
#include "../../src/modules/speed/speed_source_selector.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static void resetSelector(bool gpsEnabled = true) {
    speedSourceSelector = SpeedSourceSelector();
    mockMillis = 1;
    mockMicros = 1000;
    speedSourceSelector.begin(gpsEnabled);
}

void setUp() {
    resetSelector(true);
}

void tearDown() {
}

void test_obd_wins_when_both_sources_fresh() {
    speedSourceSelector.setObdConnected(true);
    speedSourceSelector.updateObdSample(42.0f, 1000, true);
    speedSourceSelector.updateGpsSample(55.0f, 1000, true);

    SpeedSelection out;
    TEST_ASSERT_TRUE(speedSourceSelector.select(2000, out));
    TEST_ASSERT_EQUAL(SpeedSource::OBD, out.source);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.0f, out.speedMph);
}

void test_gps_selected_when_obd_disconnected_and_gps_fresh() {
    speedSourceSelector.setObdConnected(false);
    speedSourceSelector.updateGpsSample(38.0f, 1500, true);

    SpeedSelection out;
    TEST_ASSERT_TRUE(speedSourceSelector.select(2000, out));
    TEST_ASSERT_EQUAL(SpeedSource::GPS, out.source);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 38.0f, out.speedMph);
}

void test_gps_not_used_when_obd_connected_but_stale() {
    speedSourceSelector.setObdConnected(true);
    speedSourceSelector.updateObdSample(60.0f, 1000, true);    // stale at now=4501
    speedSourceSelector.updateGpsSample(30.0f, 4500, true);    // fresh at now=4501

    SpeedSelection out;
    TEST_ASSERT_FALSE(speedSourceSelector.select(4501, out));
    TEST_ASSERT_EQUAL(SpeedSource::NONE, out.source);
}

void test_gps_fallback_restored_when_obd_disconnects() {
    speedSourceSelector.setObdConnected(true);
    speedSourceSelector.updateObdSample(60.0f, 1000, true);    // stale at now=4501
    speedSourceSelector.updateGpsSample(30.0f, 4500, true);    // fresh at now=4501

    SpeedSelection out;
    TEST_ASSERT_FALSE(speedSourceSelector.select(4501, out));

    speedSourceSelector.setObdConnected(false);
    TEST_ASSERT_TRUE(speedSourceSelector.select(4501, out));
    TEST_ASSERT_EQUAL(SpeedSource::GPS, out.source);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, out.speedMph);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_obd_wins_when_both_sources_fresh);
    RUN_TEST(test_gps_selected_when_obd_disconnected_and_gps_fresh);
    RUN_TEST(test_gps_not_used_when_obd_connected_but_stale);
    RUN_TEST(test_gps_fallback_restored_when_obd_disconnects);
    return UNITY_END();
}

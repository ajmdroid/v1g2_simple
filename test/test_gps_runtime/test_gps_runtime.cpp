#include <unity.h>

#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/gps/gps_runtime_module.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static void resetRuntime() {
    gpsRuntimeModule = GpsRuntimeModule();
    mockMillis = 1;
    mockMicros = 1000;
    gpsRuntimeModule.begin(true);
}

void setUp() {
    resetRuntime();
}

void tearDown() {
}

void test_valid_rmc_updates_speed_and_fix() {
    const bool accepted = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,230394,003.1,W*6F", 1000);
    TEST_ASSERT_TRUE(accepted);

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(1000);
    TEST_ASSERT_TRUE(status.sampleValid);
    TEST_ASSERT_TRUE(status.hasFix);
    TEST_ASSERT_EQUAL_UINT32(1, status.hardwareSamples);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 11.50779f, status.speedMph);

    float speedMph = 0.0f;
    uint32_t tsMs = 0;
    TEST_ASSERT_TRUE(gpsRuntimeModule.getFreshSpeed(2500, speedMph, tsMs));
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 11.50779f, speedMph);
    TEST_ASSERT_EQUAL_UINT32(1000, tsMs);
}

void test_valid_gga_sets_quality_without_speed_sample() {
    const bool accepted = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4D", 2000);
    TEST_ASSERT_TRUE(accepted);

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(2000);
    TEST_ASSERT_TRUE(status.hasFix);
    TEST_ASSERT_FALSE(status.sampleValid);
    TEST_ASSERT_EQUAL_UINT8(8, status.satellites);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, status.hdop);
}

void test_bad_checksum_is_rejected_and_counted() {
    const bool accepted = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,230394,003.1,W*00", 3000);
    TEST_ASSERT_FALSE(accepted);

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(3000);
    TEST_ASSERT_EQUAL_UINT32(1, status.sentencesSeen);
    TEST_ASSERT_EQUAL_UINT32(1, status.checksumFailures);
    TEST_ASSERT_EQUAL_UINT32(1, status.parseFailures);
    TEST_ASSERT_FALSE(status.sampleValid);
}

void test_fix_loss_invalidates_speed_sample() {
    TEST_ASSERT_TRUE(gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,230394,003.1,W*6F", 1000));
    TEST_ASSERT_TRUE(gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123521,V,4807.038,N,01131.000,E,000.0,000.0,230394,003.1,W*7A", 2000));

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(2000);
    TEST_ASSERT_FALSE(status.sampleValid);
    TEST_ASSERT_FALSE(status.hasFix);
}

void test_detection_timeout_disables_runtime_polling() {
    gpsRuntimeModule.update(61010);

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(61010);
    TEST_ASSERT_TRUE(status.detectionTimedOut);
    TEST_ASSERT_FALSE(status.parserActive);
    TEST_ASSERT_FALSE(status.moduleDetected);
}

void test_stale_fix_is_cleared() {
    TEST_ASSERT_TRUE(gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,230394,003.1,W*6F", 1000));

    gpsRuntimeModule.update(7002);
    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(7002);
    TEST_ASSERT_FALSE(status.hasFix);
    TEST_ASSERT_FALSE(status.sampleValid);
}

void test_overlong_sentence_is_rejected() {
    char longSentence[160];
    for (size_t i = 0; i < sizeof(longSentence) - 1; ++i) {
        longSentence[i] = 'A';
    }
    longSentence[sizeof(longSentence) - 1] = '\0';

    TEST_ASSERT_FALSE(gpsRuntimeModule.injectNmeaSentenceForTest(longSentence, 4000));
    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(4000);
    TEST_ASSERT_EQUAL_UINT32(1, status.bufferOverruns);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_valid_rmc_updates_speed_and_fix);
    RUN_TEST(test_valid_gga_sets_quality_without_speed_sample);
    RUN_TEST(test_bad_checksum_is_rejected_and_counted);
    RUN_TEST(test_fix_loss_invalidates_speed_sample);
    RUN_TEST(test_detection_timeout_disables_runtime_polling);
    RUN_TEST(test_stale_fix_is_cleared);
    RUN_TEST(test_overlong_sentence_is_rejected);
    return UNITY_END();
}

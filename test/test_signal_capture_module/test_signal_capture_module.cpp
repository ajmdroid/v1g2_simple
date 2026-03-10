#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/packet_parser.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/lockout/signal_observation_log.h"
#include "../../src/modules/lockout/signal_observation_log.cpp"
#include "../../src/modules/lockout/signal_capture_module.h"
#include "../../src/modules/lockout/lockout_band_policy.cpp"
#include "../../src/modules/lockout/signal_capture_module.cpp"

static uint32_t sdEnqueueCount = 0;
static SignalObservation lastEnqueued = {};
SignalObservationSdLogger signalObservationSdLogger;

bool SignalObservationSdLogger::enqueue(const SignalObservation& observation) {
    lastEnqueued = observation;
    sdEnqueueCount++;
    return true;
}

static bool hasObservation(const SignalObservation* observations,
                           size_t count,
                           uint8_t bandRaw,
                           uint16_t frequencyMHz) {
    for (size_t i = 0; i < count; ++i) {
        if (observations[i].bandRaw == bandRaw &&
            observations[i].frequencyMHz == frequencyMHz) {
            return true;
        }
    }
    return false;
}

static GpsRuntimeStatus makeGps() {
    GpsRuntimeStatus gps;
    gps.hasFix = true;
    gps.locationValid = true;
    gps.latitudeDeg = 10.12345f;
    gps.longitudeDeg = -20.54321f;
    gps.satellites = 7;
    gps.hdop = 1.3f;
    gps.fixAgeMs = 250;
    return gps;
}

void setUp() {
    lockoutSetKaLearningEnabled(false);
    signalCaptureModule.reset();
    signalObservationLog.reset();
    sdEnqueueCount = 0;
    lastEnqueued = SignalObservation{};
}

void tearDown() {}

void test_no_alerts_no_publish() {
    PacketParser parser;
    parser.reset();
    GpsRuntimeStatus gps = makeGps();

    signalCaptureModule.capturePriorityObservation(1000, parser, gps);

    TEST_ASSERT_EQUAL_UINT32(0, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(0, sdEnqueueCount);
}

void test_first_alert_publishes_and_enqueues() {
    PacketParser parser;
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 3, 5, 24148, true, true)});
    GpsRuntimeStatus gps = makeGps();

    signalCaptureModule.capturePriorityObservation(1000, parser, gps);

    SignalObservation out[1] = {};
    const size_t copied = signalObservationLog.copyRecent(out, 1);
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(copied));
    TEST_ASSERT_EQUAL_UINT32(1, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(1, sdEnqueueCount);
    TEST_ASSERT_EQUAL_UINT8(BAND_K, out[0].bandRaw);
    TEST_ASSERT_EQUAL_UINT16(24148, out[0].frequencyMHz);
    TEST_ASSERT_EQUAL_UINT8(5, out[0].strength);
    TEST_ASSERT_TRUE(out[0].hasFix);
    TEST_ASSERT_EQUAL_UINT32(250, out[0].fixAgeMs);
    TEST_ASSERT_TRUE(out[0].locationValid);
}

void test_same_bucket_within_repeat_window_suppressed() {
    PacketParser parser;
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 4, 24148, true, true)});
    GpsRuntimeStatus gps = makeGps();

    signalCaptureModule.capturePriorityObservation(1000, parser, gps);
    signalCaptureModule.capturePriorityObservation(2000, parser, gps);  // 1000ms later

    TEST_ASSERT_EQUAL_UINT32(1, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(1, sdEnqueueCount);
}

void test_same_bucket_after_repeat_window_publishes() {
    PacketParser parser;
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 4, 24148, true, true)});
    GpsRuntimeStatus gps = makeGps();

    signalCaptureModule.capturePriorityObservation(1000, parser, gps);
    signalCaptureModule.capturePriorityObservation(2500, parser, gps);  // 1500ms later

    TEST_ASSERT_EQUAL_UINT32(2, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(2, sdEnqueueCount);
}

void test_different_bucket_publishes_immediately() {
    PacketParser parser;
    GpsRuntimeStatus gps = makeGps();

    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 4, 24148, true, true)});
    signalCaptureModule.capturePriorityObservation(1000, parser, gps);

    // Frequency delta > 5MHz tolerance, should publish even within 1.5s gate.
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 4, 24160, true, true)});
    signalCaptureModule.capturePriorityObservation(1100, parser, gps);

    TEST_ASSERT_EQUAL_UINT32(2, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(2, sdEnqueueCount);
    TEST_ASSERT_EQUAL_UINT16(24160, lastEnqueued.frequencyMHz);
}

void test_multiple_alerts_publish_separately_in_same_frame() {
    PacketParser parser;
    GpsRuntimeStatus gps = makeGps();

    parser.setAlerts({
        AlertData::create(BAND_K, DIR_FRONT, 4, 4, 24148, true, true),
        AlertData::create(BAND_X, DIR_SIDE, 2, 3, 10525, true, false)
    });
    signalCaptureModule.capturePriorityObservation(1000, parser, gps);

    SignalObservation out[4] = {};
    const size_t copied = signalObservationLog.copyRecent(out, 4);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(copied));
    TEST_ASSERT_EQUAL_UINT32(2, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(2, sdEnqueueCount);
    TEST_ASSERT_TRUE(hasObservation(out, copied, BAND_K, 24148));
    TEST_ASSERT_TRUE(hasObservation(out, copied, BAND_X, 10525));
}

void test_multi_alert_repeat_gate_is_per_bucket() {
    PacketParser parser;
    GpsRuntimeStatus gps = makeGps();

    parser.setAlerts({
        AlertData::create(BAND_K, DIR_FRONT, 4, 4, 24148, true, true),
        AlertData::create(BAND_X, DIR_SIDE, 2, 3, 10525, true, false)
    });
    signalCaptureModule.capturePriorityObservation(1000, parser, gps);
    signalCaptureModule.capturePriorityObservation(1100, parser, gps);

    TEST_ASSERT_EQUAL_UINT32(2, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(2, sdEnqueueCount);
}

void test_unsupported_bands_not_published() {
    PacketParser parser;
    GpsRuntimeStatus gps = makeGps();

    parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 4, 4, 34700, true, true)});
    signalCaptureModule.capturePriorityObservation(1000, parser, gps);

    parser.setAlerts({AlertData::create(BAND_LASER, DIR_FRONT, 6, 0, 0, true, true)});
    signalCaptureModule.capturePriorityObservation(2000, parser, gps);

    TEST_ASSERT_EQUAL_UINT32(0, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(0, sdEnqueueCount);
}

void test_ka_band_published_when_policy_enabled() {
    lockoutSetKaLearningEnabled(true);

    PacketParser parser;
    GpsRuntimeStatus gps = makeGps();
    parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 4, 4, 34700, true, true)});
    signalCaptureModule.capturePriorityObservation(1000, parser, gps);

    TEST_ASSERT_EQUAL_UINT32(1, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(1, sdEnqueueCount);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, lastEnqueued.bandRaw);
}

void test_unsupported_bands_enqueued_when_trace_enabled() {
    PacketParser parser;
    GpsRuntimeStatus gps = makeGps();

    parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 4, 4, 34700, true, true)});
    signalCaptureModule.capturePriorityObservation(1000, parser, gps, true);

    TEST_ASSERT_EQUAL_UINT32(0, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(1, sdEnqueueCount);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, lastEnqueued.bandRaw);
}

void test_trace_enabled_enqueues_unsupported_alerts_from_full_alert_set() {
    PacketParser parser;
    GpsRuntimeStatus gps = makeGps();

    parser.setAlerts({
        AlertData::create(BAND_K, DIR_FRONT, 4, 4, 24148, true, true),
        AlertData::create(BAND_KA, DIR_FRONT, 4, 4, 34700, true, false)
    });
    signalCaptureModule.capturePriorityObservation(1000, parser, gps, true);

    SignalObservation out[2] = {};
    const size_t copied = signalObservationLog.copyRecent(out, 2);
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(copied));
    TEST_ASSERT_TRUE(hasObservation(out, copied, BAND_K, 24148));
    TEST_ASSERT_EQUAL_UINT32(1, signalObservationLog.stats().published);
    TEST_ASSERT_EQUAL_UINT32(2, sdEnqueueCount);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, lastEnqueued.bandRaw);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_no_alerts_no_publish);
    RUN_TEST(test_first_alert_publishes_and_enqueues);
    RUN_TEST(test_same_bucket_within_repeat_window_suppressed);
    RUN_TEST(test_same_bucket_after_repeat_window_publishes);
    RUN_TEST(test_different_bucket_publishes_immediately);
    RUN_TEST(test_multiple_alerts_publish_separately_in_same_frame);
    RUN_TEST(test_multi_alert_repeat_gate_is_per_bucket);
    RUN_TEST(test_unsupported_bands_not_published);
    RUN_TEST(test_ka_band_published_when_policy_enabled);
    RUN_TEST(test_unsupported_bands_enqueued_when_trace_enabled);
    RUN_TEST(test_trace_enabled_enqueues_unsupported_alerts_from_full_alert_set);
    return UNITY_END();
}

#include <unity.h>

#include "../../src/modules/lockout/signal_observation_log.h"
#include "../../src/modules/lockout/signal_observation_log.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

void setUp() {
    signalObservationLog.reset();
}

void tearDown() {}

void test_publish_and_copy_recent_order() {
    SignalObservation a;
    a.tsMs = 100;
    a.bandRaw = 2;
    a.frequencyMHz = 34000;

    SignalObservation b;
    b.tsMs = 200;
    b.bandRaw = 4;
    b.frequencyMHz = 24150;

    SignalObservation c;
    c.tsMs = 300;
    c.bandRaw = 2;
    c.frequencyMHz = 34700;

    TEST_ASSERT_TRUE(signalObservationLog.publish(a));
    TEST_ASSERT_TRUE(signalObservationLog.publish(b));
    TEST_ASSERT_TRUE(signalObservationLog.publish(c));

    SignalObservation out[3] = {};
    const size_t count = signalObservationLog.copyRecent(out, 3);
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(count));
    TEST_ASSERT_EQUAL_UINT32(300, out[0].tsMs);
    TEST_ASSERT_EQUAL_UINT32(200, out[1].tsMs);
    TEST_ASSERT_EQUAL_UINT32(100, out[2].tsMs);
}

void test_overflow_drops_oldest() {
    for (uint32_t i = 1; i <= (SignalObservationLog::kCapacity + 1); ++i) {
        SignalObservation sample;
        sample.tsMs = i;
        signalObservationLog.publish(sample);
    }

    const SignalObservationLogStats stats = signalObservationLog.stats();
    TEST_ASSERT_EQUAL_UINT32(SignalObservationLog::kCapacity + 1, stats.published);
    TEST_ASSERT_EQUAL_UINT32(1, stats.drops);
    TEST_ASSERT_EQUAL_UINT32(SignalObservationLog::kCapacity, static_cast<uint32_t>(stats.size));

    SignalObservation out[SignalObservationLog::kCapacity] = {};
    const size_t count = signalObservationLog.copyRecent(out, SignalObservationLog::kCapacity);
    TEST_ASSERT_EQUAL_UINT32(SignalObservationLog::kCapacity, static_cast<uint32_t>(count));
    TEST_ASSERT_EQUAL_UINT32(SignalObservationLog::kCapacity + 1, out[0].tsMs);
    TEST_ASSERT_EQUAL_UINT32(2, out[count - 1].tsMs);
}

void test_copy_recent_respects_limit() {
    for (uint32_t i = 1; i <= 6; ++i) {
        SignalObservation sample;
        sample.tsMs = i * 10;
        sample.bandRaw = static_cast<uint8_t>(i);
        signalObservationLog.publish(sample);
    }

    SignalObservation out[2] = {};
    const size_t count = signalObservationLog.copyRecent(out, 2);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(count));
    TEST_ASSERT_EQUAL_UINT32(60, out[0].tsMs);
    TEST_ASSERT_EQUAL_UINT32(6, out[0].bandRaw);
    TEST_ASSERT_EQUAL_UINT32(50, out[1].tsMs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_publish_and_copy_recent_order);
    RUN_TEST(test_overflow_drops_oldest);
    RUN_TEST(test_copy_recent_respects_limit);
    return UNITY_END();
}

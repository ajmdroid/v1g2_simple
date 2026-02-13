#include <unity.h>

#include "../mocks/settings.h"
#include "../../src/modules/gps/gps_lockout_safety.h"
#include "../../src/modules/gps/gps_lockout_safety.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

void setUp() {}
void tearDown() {}

void test_parse_mode_accepts_symbolic_names() {
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_OFF,
                      gpsLockoutParseRuntimeModeArg("off", LOCKOUT_RUNTIME_ENFORCE));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_SHADOW,
                      gpsLockoutParseRuntimeModeArg("shadow", LOCKOUT_RUNTIME_OFF));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_ADVISORY,
                      gpsLockoutParseRuntimeModeArg("AdVisOrY", LOCKOUT_RUNTIME_OFF));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_ENFORCE,
                      gpsLockoutParseRuntimeModeArg(" enforce ", LOCKOUT_RUNTIME_OFF));
}

void test_parse_mode_accepts_numeric_and_clamps() {
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_OFF,
                      gpsLockoutParseRuntimeModeArg("0", LOCKOUT_RUNTIME_ENFORCE));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_SHADOW,
                      gpsLockoutParseRuntimeModeArg("1", LOCKOUT_RUNTIME_OFF));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_ADVISORY,
                      gpsLockoutParseRuntimeModeArg("2", LOCKOUT_RUNTIME_OFF));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_ENFORCE,
                      gpsLockoutParseRuntimeModeArg("9", LOCKOUT_RUNTIME_OFF));
}

void test_parse_mode_uses_fallback_for_invalid() {
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_SHADOW,
                      gpsLockoutParseRuntimeModeArg("", LOCKOUT_RUNTIME_SHADOW));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_ADVISORY,
                      gpsLockoutParseRuntimeModeArg("unknown", LOCKOUT_RUNTIME_ADVISORY));
}

void test_guard_disabled_never_trips() {
    V1Settings s;
    s.gpsLockoutCoreGuardEnabled = false;
    s.gpsLockoutMaxQueueDrops = 0;
    s.gpsLockoutMaxPerfDrops = 0;
    s.gpsLockoutMaxEventBusDrops = 0;

    const GpsLockoutCoreGuardStatus out =
        gpsLockoutEvaluateCoreGuard(s.gpsLockoutCoreGuardEnabled,
                                    s.gpsLockoutMaxQueueDrops,
                                    s.gpsLockoutMaxPerfDrops,
                                    s.gpsLockoutMaxEventBusDrops,
                                    100,
                                    100,
                                    100);
    TEST_ASSERT_FALSE(out.enabled);
    TEST_ASSERT_FALSE(out.tripped);
    TEST_ASSERT_EQUAL_STRING("none", out.reason);
}

void test_guard_trips_on_queue_drops_first() {
    V1Settings s;
    s.gpsLockoutCoreGuardEnabled = true;
    s.gpsLockoutMaxQueueDrops = 0;
    s.gpsLockoutMaxPerfDrops = 0;
    s.gpsLockoutMaxEventBusDrops = 0;

    const GpsLockoutCoreGuardStatus out =
        gpsLockoutEvaluateCoreGuard(s.gpsLockoutCoreGuardEnabled,
                                    s.gpsLockoutMaxQueueDrops,
                                    s.gpsLockoutMaxPerfDrops,
                                    s.gpsLockoutMaxEventBusDrops,
                                    1,
                                    5,
                                    5);
    TEST_ASSERT_TRUE(out.enabled);
    TEST_ASSERT_TRUE(out.tripped);
    TEST_ASSERT_EQUAL_STRING("queueDrops", out.reason);
}

void test_guard_trips_on_perf_drops() {
    V1Settings s;
    s.gpsLockoutCoreGuardEnabled = true;
    s.gpsLockoutMaxQueueDrops = 5;
    s.gpsLockoutMaxPerfDrops = 0;
    s.gpsLockoutMaxEventBusDrops = 0;

    const GpsLockoutCoreGuardStatus out =
        gpsLockoutEvaluateCoreGuard(s.gpsLockoutCoreGuardEnabled,
                                    s.gpsLockoutMaxQueueDrops,
                                    s.gpsLockoutMaxPerfDrops,
                                    s.gpsLockoutMaxEventBusDrops,
                                    0,
                                    1,
                                    10);
    TEST_ASSERT_TRUE(out.tripped);
    TEST_ASSERT_EQUAL_STRING("perfDrop", out.reason);
}

void test_guard_trips_on_event_bus_drops() {
    V1Settings s;
    s.gpsLockoutCoreGuardEnabled = true;
    s.gpsLockoutMaxQueueDrops = 5;
    s.gpsLockoutMaxPerfDrops = 5;
    s.gpsLockoutMaxEventBusDrops = 0;

    const GpsLockoutCoreGuardStatus out =
        gpsLockoutEvaluateCoreGuard(s.gpsLockoutCoreGuardEnabled,
                                    s.gpsLockoutMaxQueueDrops,
                                    s.gpsLockoutMaxPerfDrops,
                                    s.gpsLockoutMaxEventBusDrops,
                                    0,
                                    0,
                                    1);
    TEST_ASSERT_TRUE(out.tripped);
    TEST_ASSERT_EQUAL_STRING("eventBusDrop", out.reason);
}

void test_guard_clear_when_within_thresholds() {
    V1Settings s;
    s.gpsLockoutCoreGuardEnabled = true;
    s.gpsLockoutMaxQueueDrops = 2;
    s.gpsLockoutMaxPerfDrops = 2;
    s.gpsLockoutMaxEventBusDrops = 2;

    const GpsLockoutCoreGuardStatus out =
        gpsLockoutEvaluateCoreGuard(s.gpsLockoutCoreGuardEnabled,
                                    s.gpsLockoutMaxQueueDrops,
                                    s.gpsLockoutMaxPerfDrops,
                                    s.gpsLockoutMaxEventBusDrops,
                                    2,
                                    2,
                                    2);
    TEST_ASSERT_TRUE(out.enabled);
    TEST_ASSERT_FALSE(out.tripped);
    TEST_ASSERT_EQUAL_STRING("none", out.reason);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_mode_accepts_symbolic_names);
    RUN_TEST(test_parse_mode_accepts_numeric_and_clamps);
    RUN_TEST(test_parse_mode_uses_fallback_for_invalid);
    RUN_TEST(test_guard_disabled_never_trips);
    RUN_TEST(test_guard_trips_on_queue_drops_first);
    RUN_TEST(test_guard_trips_on_perf_drops);
    RUN_TEST(test_guard_trips_on_event_bus_drops);
    RUN_TEST(test_guard_clear_when_within_thresholds);
    return UNITY_END();
}

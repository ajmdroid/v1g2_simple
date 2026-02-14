#include <unity.h>

#include "../../src/modules/camera/camera_event_log.h"
#include "../../src/modules/camera/camera_event_log.cpp"  // Pull implementation for UNIT_TEST.

void setUp() {
    cameraEventLog.reset();
}

void tearDown() {}

void test_publish_and_copy_recent_order() {
    CameraEvent a;
    a.tsMs = 100;
    a.cameraId = 10;

    CameraEvent b;
    b.tsMs = 200;
    b.cameraId = 20;

    CameraEvent c;
    c.tsMs = 300;
    c.cameraId = 30;

    TEST_ASSERT_TRUE(cameraEventLog.publish(a));
    TEST_ASSERT_TRUE(cameraEventLog.publish(b));
    TEST_ASSERT_TRUE(cameraEventLog.publish(c));

    CameraEvent out[3] = {};
    const size_t count = cameraEventLog.copyRecent(out, 3);
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(count));
    TEST_ASSERT_EQUAL_UINT32(300, out[0].tsMs);
    TEST_ASSERT_EQUAL_UINT32(200, out[1].tsMs);
    TEST_ASSERT_EQUAL_UINT32(100, out[2].tsMs);
}

void test_overflow_drops_oldest_and_returns_false() {
    for (uint32_t i = 1; i <= CameraEventLog::kCapacity; ++i) {
        CameraEvent event;
        event.tsMs = i;
        TEST_ASSERT_TRUE(cameraEventLog.publish(event));
    }

    CameraEvent overflow;
    overflow.tsMs = CameraEventLog::kCapacity + 1u;
    TEST_ASSERT_FALSE(cameraEventLog.publish(overflow));

    const CameraEventLogStats stats = cameraEventLog.stats();
    TEST_ASSERT_EQUAL_UINT32(CameraEventLog::kCapacity + 1u, stats.published);
    TEST_ASSERT_EQUAL_UINT32(1, stats.drops);
    TEST_ASSERT_EQUAL_UINT32(CameraEventLog::kCapacity, static_cast<uint32_t>(stats.size));

    CameraEvent out[CameraEventLog::kCapacity] = {};
    const size_t count = cameraEventLog.copyRecent(out, CameraEventLog::kCapacity);
    TEST_ASSERT_EQUAL_UINT32(CameraEventLog::kCapacity, static_cast<uint32_t>(count));
    TEST_ASSERT_EQUAL_UINT32(CameraEventLog::kCapacity + 1u, out[0].tsMs);
    TEST_ASSERT_EQUAL_UINT32(2, out[count - 1].tsMs);
}

void test_copy_recent_respects_limit_and_null_output() {
    for (uint32_t i = 1; i <= 5; ++i) {
        CameraEvent event;
        event.tsMs = i * 10u;
        cameraEventLog.publish(event);
    }

    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(cameraEventLog.copyRecent(nullptr, 3)));

    CameraEvent out[2] = {};
    const size_t count = cameraEventLog.copyRecent(out, 2);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(count));
    TEST_ASSERT_EQUAL_UINT32(50, out[0].tsMs);
    TEST_ASSERT_EQUAL_UINT32(40, out[1].tsMs);
}

void test_reset_clears_stats_and_contents() {
    CameraEvent event;
    event.tsMs = 111;
    cameraEventLog.publish(event);
    cameraEventLog.reset();

    const CameraEventLogStats stats = cameraEventLog.stats();
    TEST_ASSERT_EQUAL_UINT32(0, stats.published);
    TEST_ASSERT_EQUAL_UINT32(0, stats.drops);
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(stats.size));

    CameraEvent out[1] = {};
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(cameraEventLog.copyRecent(out, 1)));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_publish_and_copy_recent_order);
    RUN_TEST(test_overflow_drops_oldest_and_returns_false);
    RUN_TEST(test_copy_recent_respects_limit_and_null_output);
    RUN_TEST(test_reset_clears_stats_and_contents);
    return UNITY_END();
}

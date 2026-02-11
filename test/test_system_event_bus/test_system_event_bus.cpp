#include <unity.h>
#include "../../src/modules/system/system_event_bus.h"

static SystemEventBus bus;

void setUp(void) {
    bus.reset();
}

void tearDown(void) {
}

void test_publish_consume_fifo_order() {
    SystemEvent a;
    a.type = SystemEventType::BLE_FRAME_PARSED;
    a.tsMs = 100;
    a.seq = 1;

    SystemEvent b;
    b.type = SystemEventType::GPS_UPDATED;
    b.tsMs = 200;
    b.seq = 2;

    TEST_ASSERT_TRUE(bus.publish(a));
    TEST_ASSERT_TRUE(bus.publish(b));
    TEST_ASSERT_EQUAL_UINT32(2, bus.getPublishCount());
    TEST_ASSERT_EQUAL_UINT32(0, bus.getDropCount());
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(bus.size()));

    SystemEvent out;
    TEST_ASSERT_TRUE(bus.consume(out));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SystemEventType::BLE_FRAME_PARSED), static_cast<uint8_t>(out.type));
    TEST_ASSERT_EQUAL_UINT32(100, out.tsMs);
    TEST_ASSERT_EQUAL_UINT32(1, out.seq);

    TEST_ASSERT_TRUE(bus.consume(out));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SystemEventType::GPS_UPDATED), static_cast<uint8_t>(out.type));
    TEST_ASSERT_EQUAL_UINT32(200, out.tsMs);
    TEST_ASSERT_EQUAL_UINT32(2, out.seq);

    TEST_ASSERT_FALSE(bus.consume(out));
}

void test_publish_overflow_drops_oldest() {
    for (size_t i = 0; i < SystemEventBus::kCapacity; ++i) {
        SystemEvent event;
        event.type = SystemEventType::BLE_FRAME_PARSED;
        event.seq = static_cast<uint32_t>(i + 1);
        TEST_ASSERT_TRUE(bus.publish(event));
    }

    SystemEvent overflowEvent;
    overflowEvent.type = SystemEventType::BLE_FRAME_PARSED;
    overflowEvent.seq = 999;

    TEST_ASSERT_FALSE(bus.publish(overflowEvent));
    TEST_ASSERT_EQUAL_UINT32(1, bus.getDropCount());
    TEST_ASSERT_EQUAL_UINT32(SystemEventBus::kCapacity + 1, bus.getPublishCount());
    TEST_ASSERT_EQUAL_UINT32(SystemEventBus::kCapacity, static_cast<uint32_t>(bus.size()));

    SystemEvent out;
    TEST_ASSERT_TRUE(bus.consume(out));
    TEST_ASSERT_EQUAL_UINT32(2, out.seq);  // First event was dropped

    while (bus.size() > 1) {
        TEST_ASSERT_TRUE(bus.consume(out));
    }

    TEST_ASSERT_TRUE(bus.consume(out));
    TEST_ASSERT_EQUAL_UINT32(999, out.seq);  // Latest event is retained
}

void test_reset_clears_queue_and_counters() {
    SystemEvent event;
    event.type = SystemEventType::OBD_UPDATED;
    event.seq = 42;

    TEST_ASSERT_TRUE(bus.publish(event));
    TEST_ASSERT_EQUAL_UINT32(1, bus.getPublishCount());

    bus.reset();
    TEST_ASSERT_EQUAL_UINT32(0, bus.getPublishCount());
    TEST_ASSERT_EQUAL_UINT32(0, bus.getDropCount());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(bus.size()));

    SystemEvent out;
    TEST_ASSERT_FALSE(bus.consume(out));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_publish_consume_fifo_order);
    RUN_TEST(test_publish_overflow_drops_oldest);
    RUN_TEST(test_reset_clears_queue_and_counters);
    return UNITY_END();
}

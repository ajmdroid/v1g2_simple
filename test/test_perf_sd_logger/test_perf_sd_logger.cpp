#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/mock_heap_caps_state.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/perf_metrics.h"
#include "../../src/time_service.h"

PerfCounters perfCounters;

void perfRecordSdFlushUs(uint32_t) {}

TimeService timeService;

void TimeService::begin() {}
int64_t TimeService::nowEpochMsOr0() const { return 0; }
uint32_t TimeService::epochAgeMsOr0() const { return 0; }
void TimeService::setEpochBaseMs(int64_t, int32_t, Source) {}
void TimeService::clear() {}
void TimeService::persistCurrentTime() {}
void TimeService::periodicSave(uint32_t) {}

#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/storage_manager.cpp"
#include "../../src/perf_sd_logger.cpp"

void setUp() {
    mock_reset_heap_caps();
    mock_reset_queue_create_state();
    mock_reset_task_create_state();
    perfCounters.reset();
}

void tearDown() {}

void test_receive_snapshot_times_out_when_queue_empty() {
    PerfSdLogger logger;

    logger.begin(true);

    PerfSdSnapshot snapshot{};
    TEST_ASSERT_FALSE(logger.receiveSnapshotForTest(snapshot, pdMS_TO_TICKS(1000)));
}

void test_receive_snapshot_dequeues_enqueued_item() {
    PerfSdLogger logger;
    logger.begin(true);

    PerfSdSnapshot expected{};
    expected.millisTs = 1234;
    expected.rx = 77;
    expected.queueHighWater = 5;
    expected.displayUpdates = 9;
    expected.audioPlayBusy = 2;
    expected.wifiPriorityMode = 1;
    TEST_ASSERT_TRUE(logger.enqueue(expected));

    PerfSdSnapshot actual{};
    TEST_ASSERT_TRUE(logger.receiveSnapshotForTest(actual, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL_UINT32(expected.millisTs, actual.millisTs);
    TEST_ASSERT_EQUAL_UINT32(expected.rx, actual.rx);
    TEST_ASSERT_EQUAL_UINT32(expected.queueHighWater, actual.queueHighWater);
    TEST_ASSERT_EQUAL_UINT32(expected.displayUpdates, actual.displayUpdates);
    TEST_ASSERT_EQUAL_UINT32(expected.audioPlayBusy, actual.audioPlayBusy);
    TEST_ASSERT_EQUAL_UINT8(expected.wifiPriorityMode, actual.wifiPriorityMode);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_receive_snapshot_times_out_when_queue_empty);
    RUN_TEST(test_receive_snapshot_dequeues_enqueued_item);
    return UNITY_END();
}

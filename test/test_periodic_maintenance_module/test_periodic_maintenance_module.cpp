#include <unity.h>
#include <initializer_list>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/periodic_maintenance_module.cpp"

static PeriodicMaintenanceModule module;

enum CallId {
    CALL_PERF = 1,
    CALL_TIME_SAVE = 2,
    CALL_LEARNER = 3,
    CALL_LOCKOUT_SAVE = 4,
    CALL_PENDING_SAVE = 5,
};

static int callLog[16];
static size_t callLogCount = 0;

static uint32_t timestampSequence[16];
static size_t timestampSequenceCount = 0;
static size_t timestampSequenceIndex = 0;

static uint32_t perfElapsedUs = 0;
static uint32_t timeElapsedUs = 0;
static int perfRecordCalls = 0;
static int timeRecordCalls = 0;
static uint32_t timeSaveNowMs = 0;
static int64_t epochNowMs = 0;
static int64_t learnerEpochMs = -1;
static uint32_t learnerNowMs = 0;
static uint32_t lockoutSaveNowMs = 0;
static uint32_t pendingSaveNowMs = 0;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void setTimestampSequence(std::initializer_list<uint32_t> values) {
    timestampSequenceCount = values.size();
    timestampSequenceIndex = 0;
    size_t i = 0;
    for (uint32_t value : values) {
        timestampSequence[i++] = value;
    }
}

static uint32_t nextTimestampUs(void*) {
    if (timestampSequenceCount == 0) {
        return 0;
    }
    if (timestampSequenceIndex >= timestampSequenceCount) {
        return timestampSequence[timestampSequenceCount - 1];
    }
    return timestampSequence[timestampSequenceIndex++];
}

static void runPerfReport(void*) {
    noteCall(CALL_PERF);
}

static void recordPerfReportUs(void*, uint32_t elapsedUs) {
    perfRecordCalls++;
    perfElapsedUs = elapsedUs;
}

static void runTimeSave(void*, uint32_t nowMs) {
    noteCall(CALL_TIME_SAVE);
    timeSaveNowMs = nowMs;
}

static void recordTimeSaveUs(void*, uint32_t elapsedUs) {
    timeRecordCalls++;
    timeElapsedUs = elapsedUs;
}

static int64_t nowEpochMsOr0(void*) {
    return epochNowMs;
}

static void runLockoutLearner(void*, uint32_t nowMs, int64_t epochMs) {
    noteCall(CALL_LEARNER);
    learnerNowMs = nowMs;
    learnerEpochMs = epochMs;
}

static void runLockoutStoreSave(void*, uint32_t nowMs) {
    noteCall(CALL_LOCKOUT_SAVE);
    lockoutSaveNowMs = nowMs;
}

static void runLearnerPendingSave(void*, uint32_t nowMs) {
    noteCall(CALL_PENDING_SAVE);
    pendingSaveNowMs = nowMs;
}

static void resetState() {
    callLogCount = 0;
    timestampSequenceCount = 0;
    timestampSequenceIndex = 0;
    perfElapsedUs = 0;
    timeElapsedUs = 0;
    perfRecordCalls = 0;
    timeRecordCalls = 0;
    timeSaveNowMs = 0;
    epochNowMs = 0;
    learnerEpochMs = -1;
    learnerNowMs = 0;
    lockoutSaveNowMs = 0;
    pendingSaveNowMs = 0;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_process_runs_full_bundle_in_order_with_timing_records() {
    PeriodicMaintenanceModule::Providers providers;
    providers.timestampUs = nextTimestampUs;
    providers.runPerfReport = runPerfReport;
    providers.recordPerfReportUs = recordPerfReportUs;
    providers.runTimeSave = runTimeSave;
    providers.recordTimeSaveUs = recordTimeSaveUs;
    providers.nowEpochMsOr0 = nowEpochMsOr0;
    providers.runLockoutLearner = runLockoutLearner;
    providers.runLockoutStoreSave = runLockoutStoreSave;
    providers.runLearnerPendingSave = runLearnerPendingSave;
    module.begin(providers);

    setTimestampSequence({100, 130, 200, 245});
    epochNowMs = 456789;
    module.process(5000);

    TEST_ASSERT_EQUAL(5, callLogCount);
    TEST_ASSERT_EQUAL(CALL_PERF, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_TIME_SAVE, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_LEARNER, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_LOCKOUT_SAVE, callLog[3]);
    TEST_ASSERT_EQUAL(CALL_PENDING_SAVE, callLog[4]);

    TEST_ASSERT_EQUAL(1, perfRecordCalls);
    TEST_ASSERT_EQUAL(30u, perfElapsedUs);
    TEST_ASSERT_EQUAL(1, timeRecordCalls);
    TEST_ASSERT_EQUAL(45u, timeElapsedUs);

    TEST_ASSERT_EQUAL(5000u, timeSaveNowMs);
    TEST_ASSERT_EQUAL(5000u, learnerNowMs);
    TEST_ASSERT_EQUAL_INT64(456789, learnerEpochMs);
    TEST_ASSERT_EQUAL(5000u, lockoutSaveNowMs);
    TEST_ASSERT_EQUAL(5000u, pendingSaveNowMs);
}

void test_process_defaults_epoch_to_zero_when_provider_missing() {
    PeriodicMaintenanceModule::Providers providers;
    providers.runPerfReport = runPerfReport;
    providers.runTimeSave = runTimeSave;
    providers.runLockoutLearner = runLockoutLearner;
    providers.runLockoutStoreSave = runLockoutStoreSave;
    providers.runLearnerPendingSave = runLearnerPendingSave;
    module.begin(providers);

    module.process(1200);

    TEST_ASSERT_EQUAL(5, callLogCount);
    TEST_ASSERT_EQUAL(CALL_PERF, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_TIME_SAVE, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_LEARNER, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_LOCKOUT_SAVE, callLog[3]);
    TEST_ASSERT_EQUAL(CALL_PENDING_SAVE, callLog[4]);

    TEST_ASSERT_EQUAL(0, perfRecordCalls);
    TEST_ASSERT_EQUAL(0, timeRecordCalls);
    TEST_ASSERT_EQUAL_INT64(0, learnerEpochMs);
}

void test_perf_elapsed_is_wrap_safe() {
    PeriodicMaintenanceModule::Providers providers;
    providers.timestampUs = nextTimestampUs;
    providers.runPerfReport = runPerfReport;
    providers.recordPerfReportUs = recordPerfReportUs;
    module.begin(providers);

    setTimestampSequence({0xFFFFFFF0u, 0x00000020u});
    module.process(10);

    TEST_ASSERT_EQUAL(1, perfRecordCalls);
    TEST_ASSERT_EQUAL(0x30u, perfElapsedUs);
    TEST_ASSERT_EQUAL(1, callLogCount);
    TEST_ASSERT_EQUAL(CALL_PERF, callLog[0]);
}

void test_empty_providers_is_safe_noop() {
    PeriodicMaintenanceModule::Providers providers;
    module.begin(providers);

    module.process(250);

    TEST_ASSERT_EQUAL(0, callLogCount);
    TEST_ASSERT_EQUAL(0, perfRecordCalls);
    TEST_ASSERT_EQUAL(0, timeRecordCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_runs_full_bundle_in_order_with_timing_records);
    RUN_TEST(test_process_defaults_epoch_to_zero_when_provider_missing);
    RUN_TEST(test_perf_elapsed_is_wrap_safe);
    RUN_TEST(test_empty_providers_is_safe_noop);
    return UNITY_END();
}

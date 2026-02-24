#include <unity.h>
#include <initializer_list>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_ingest_module.cpp"

static LoopIngestModule module;

enum CallId {
    CALL_RUNTIME_BLE_PROCESS = 1,
    CALL_PROVIDER_BLE_PROCESS = 2,
    CALL_RECORD_BLE_PROCESS = 3,
    CALL_RUNTIME_BLE_DRAIN = 4,
    CALL_PROVIDER_BLE_DRAIN = 5,
    CALL_RECORD_BLE_DRAIN = 6,
    CALL_OBD = 7,
    CALL_RECORD_OBD = 8,
    CALL_GPS = 9,
    CALL_RECORD_GPS = 10,
};

static int callLog[32];
static size_t callLogCount = 0;

static uint32_t timestampSequence[16];
static size_t timestampSequenceCount = 0;
static size_t timestampSequenceIndex = 0;

static int providerBleProcessCalls = 0;
static int runtimeBleProcessCalls = 0;
static int providerBleDrainCalls = 0;
static int runtimeBleDrainCalls = 0;
static bool scriptedBackpressure = false;

static int obdCalls = 0;
static uint32_t lastObdNowMs = 0;
static bool lastObdServiceEnabled = false;

static int gpsCalls = 0;
static uint32_t lastGpsNowMs = 0;

static int recordBleProcessCalls = 0;
static uint32_t bleProcessElapsedUs = 0;
static int recordBleDrainCalls = 0;
static uint32_t bleDrainElapsedUs = 0;
static int recordObdCalls = 0;
static uint32_t obdElapsedUs = 0;
static int recordGpsCalls = 0;
static uint32_t gpsElapsedUs = 0;

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

static void providerRunBleProcess(void*) {
    providerBleProcessCalls++;
    noteCall(CALL_PROVIDER_BLE_PROCESS);
}

static void runtimeRunBleProcess() {
    runtimeBleProcessCalls++;
    noteCall(CALL_RUNTIME_BLE_PROCESS);
}

static void recordBleProcessUs(void*, uint32_t elapsedUs) {
    recordBleProcessCalls++;
    bleProcessElapsedUs = elapsedUs;
    noteCall(CALL_RECORD_BLE_PROCESS);
}

static void providerRunBleDrain(void*) {
    providerBleDrainCalls++;
    noteCall(CALL_PROVIDER_BLE_DRAIN);
}

static void runtimeRunBleDrain() {
    runtimeBleDrainCalls++;
    noteCall(CALL_RUNTIME_BLE_DRAIN);
}

static void recordBleDrainUs(void*, uint32_t elapsedUs) {
    recordBleDrainCalls++;
    bleDrainElapsedUs = elapsedUs;
    noteCall(CALL_RECORD_BLE_DRAIN);
}

static bool readBleBackpressure(void*) {
    return scriptedBackpressure;
}

static void runObdRuntime(void*, uint32_t nowMs, bool obdServiceEnabled) {
    obdCalls++;
    lastObdNowMs = nowMs;
    lastObdServiceEnabled = obdServiceEnabled;
    noteCall(CALL_OBD);
}

static void recordObdUs(void*, uint32_t elapsedUs) {
    recordObdCalls++;
    obdElapsedUs = elapsedUs;
    noteCall(CALL_RECORD_OBD);
}

static void runGpsUpdate(void*, uint32_t nowMs) {
    gpsCalls++;
    lastGpsNowMs = nowMs;
    noteCall(CALL_GPS);
}

static void recordGpsUs(void*, uint32_t elapsedUs) {
    recordGpsCalls++;
    gpsElapsedUs = elapsedUs;
    noteCall(CALL_RECORD_GPS);
}

static LoopIngestModule::Providers makeDefaultProviders() {
    LoopIngestModule::Providers providers;
    providers.timestampUs = nextTimestampUs;
    providers.runBleProcess = providerRunBleProcess;
    providers.recordBleProcessUs = recordBleProcessUs;
    providers.runBleDrain = providerRunBleDrain;
    providers.recordBleDrainUs = recordBleDrainUs;
    providers.readBleBackpressure = readBleBackpressure;
    providers.runObdRuntime = runObdRuntime;
    providers.recordObdUs = recordObdUs;
    providers.runGpsRuntimeUpdate = runGpsUpdate;
    providers.recordGpsUs = recordGpsUs;
    return providers;
}

static void resetState() {
    callLogCount = 0;
    timestampSequenceCount = 0;
    timestampSequenceIndex = 0;
    providerBleProcessCalls = 0;
    runtimeBleProcessCalls = 0;
    providerBleDrainCalls = 0;
    runtimeBleDrainCalls = 0;
    scriptedBackpressure = false;
    obdCalls = 0;
    lastObdNowMs = 0;
    lastObdServiceEnabled = false;
    gpsCalls = 0;
    lastGpsNowMs = 0;
    recordBleProcessCalls = 0;
    bleProcessElapsedUs = 0;
    recordBleDrainCalls = 0;
    bleDrainElapsedUs = 0;
    recordObdCalls = 0;
    obdElapsedUs = 0;
    recordGpsCalls = 0;
    gpsElapsedUs = 0;
}

void setUp() {
    resetState();
    module.begin(makeDefaultProviders());
}

void tearDown() {}

void test_process_runs_full_pipeline_with_runtime_ble_callbacks_and_perf_records() {
    scriptedBackpressure = true;
    setTimestampSequence({100, 130, 200, 260, 400, 460, 800, 830});

    LoopIngestContext ctx;
    ctx.nowMs = 5000;
    ctx.bleProcessEnabled = true;
    ctx.runBleProcess = runtimeRunBleProcess;
    ctx.runBleDrain = runtimeRunBleDrain;
    ctx.skipNonCoreThisLoop = false;
    ctx.overloadThisLoop = false;
    ctx.obdServiceEnabled = true;

    const LoopIngestResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(0, providerBleProcessCalls);
    TEST_ASSERT_EQUAL(1, runtimeBleProcessCalls);
    TEST_ASSERT_EQUAL(0, providerBleDrainCalls);
    TEST_ASSERT_EQUAL(1, runtimeBleDrainCalls);
    TEST_ASSERT_EQUAL(1, recordBleProcessCalls);
    TEST_ASSERT_EQUAL(30u, bleProcessElapsedUs);
    TEST_ASSERT_EQUAL(1, recordBleDrainCalls);
    TEST_ASSERT_EQUAL(60u, bleDrainElapsedUs);

    TEST_ASSERT_EQUAL(1, obdCalls);
    TEST_ASSERT_EQUAL(5000u, lastObdNowMs);
    TEST_ASSERT_TRUE(lastObdServiceEnabled);
    TEST_ASSERT_EQUAL(1, recordObdCalls);
    TEST_ASSERT_EQUAL(60u, obdElapsedUs);

    TEST_ASSERT_EQUAL(1, gpsCalls);
    TEST_ASSERT_EQUAL(5000u, lastGpsNowMs);
    TEST_ASSERT_EQUAL(1, recordGpsCalls);
    TEST_ASSERT_EQUAL(30u, gpsElapsedUs);

    TEST_ASSERT_TRUE(result.bleBackpressure);
    TEST_ASSERT_TRUE(result.skipLateNonCoreThisLoop);
    TEST_ASSERT_TRUE(result.overloadLateThisLoop);

    TEST_ASSERT_EQUAL(8, callLogCount);
    TEST_ASSERT_EQUAL(CALL_RUNTIME_BLE_PROCESS, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_RECORD_BLE_PROCESS, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_RUNTIME_BLE_DRAIN, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_RECORD_BLE_DRAIN, callLog[3]);
    TEST_ASSERT_EQUAL(CALL_OBD, callLog[4]);
    TEST_ASSERT_EQUAL(CALL_RECORD_OBD, callLog[5]);
    TEST_ASSERT_EQUAL(CALL_GPS, callLog[6]);
    TEST_ASSERT_EQUAL(CALL_RECORD_GPS, callLog[7]);
}

void test_process_uses_provider_ble_callbacks_when_runtime_callbacks_missing() {
    scriptedBackpressure = false;
    setTimestampSequence({10, 20, 30, 40, 50, 65, 70, 90});

    LoopIngestContext ctx;
    ctx.nowMs = 2500;
    ctx.bleProcessEnabled = true;
    ctx.skipNonCoreThisLoop = true;
    ctx.overloadThisLoop = false;
    ctx.obdServiceEnabled = true;

    const LoopIngestResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(1, providerBleProcessCalls);
    TEST_ASSERT_EQUAL(0, runtimeBleProcessCalls);
    TEST_ASSERT_EQUAL(1, providerBleDrainCalls);
    TEST_ASSERT_EQUAL(0, runtimeBleDrainCalls);
    TEST_ASSERT_EQUAL(1, recordBleProcessCalls);
    TEST_ASSERT_EQUAL(10u, bleProcessElapsedUs);
    TEST_ASSERT_EQUAL(1, recordBleDrainCalls);
    TEST_ASSERT_EQUAL(10u, bleDrainElapsedUs);
    TEST_ASSERT_FALSE(result.bleBackpressure);
    TEST_ASSERT_TRUE(result.skipLateNonCoreThisLoop);
    TEST_ASSERT_FALSE(result.overloadLateThisLoop);
}

void test_ble_process_disabled_skips_ble_process_only() {
    setTimestampSequence({1000, 1010, 1100, 1110, 1200, 1210});

    LoopIngestContext ctx;
    ctx.nowMs = 99;
    ctx.bleProcessEnabled = false;
    ctx.runBleDrain = runtimeRunBleDrain;
    ctx.obdServiceEnabled = false;

    module.process(ctx);

    TEST_ASSERT_EQUAL(0, providerBleProcessCalls);
    TEST_ASSERT_EQUAL(0, runtimeBleProcessCalls);
    TEST_ASSERT_EQUAL(1, runtimeBleDrainCalls);
    TEST_ASSERT_EQUAL(0, recordBleProcessCalls);
    TEST_ASSERT_EQUAL(1, recordBleDrainCalls);
}

void test_obd_disabled_runs_obd_without_perf_record() {
    setTimestampSequence({1, 2, 3, 4});

    LoopIngestContext ctx;
    ctx.nowMs = 42;
    ctx.bleProcessEnabled = false;
    ctx.obdServiceEnabled = false;

    module.process(ctx);

    TEST_ASSERT_EQUAL(1, obdCalls);
    TEST_ASSERT_FALSE(lastObdServiceEnabled);
    TEST_ASSERT_EQUAL(0, recordObdCalls);
}

void test_missing_timing_hooks_still_runs_operations() {
    LoopIngestModule::Providers providers = makeDefaultProviders();
    providers.timestampUs = nullptr;
    module.begin(providers);

    LoopIngestContext ctx;
    ctx.nowMs = 88;
    ctx.bleProcessEnabled = true;
    ctx.obdServiceEnabled = true;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, providerBleProcessCalls);
    TEST_ASSERT_EQUAL(1, providerBleDrainCalls);
    TEST_ASSERT_EQUAL(1, obdCalls);
    TEST_ASSERT_EQUAL(1, gpsCalls);
    TEST_ASSERT_EQUAL(0, recordBleProcessCalls);
    TEST_ASSERT_EQUAL(0, recordBleDrainCalls);
    TEST_ASSERT_EQUAL(0, recordObdCalls);
    TEST_ASSERT_EQUAL(0, recordGpsCalls);
}

void test_empty_providers_is_safe_and_merges_flags() {
    LoopIngestModule::Providers providers;
    module.begin(providers);

    LoopIngestContext ctx;
    ctx.skipNonCoreThisLoop = true;
    ctx.overloadThisLoop = true;
    const LoopIngestResult result = module.process(ctx);

    TEST_ASSERT_FALSE(result.bleBackpressure);
    TEST_ASSERT_TRUE(result.skipLateNonCoreThisLoop);
    TEST_ASSERT_TRUE(result.overloadLateThisLoop);
    TEST_ASSERT_EQUAL(0, callLogCount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_runs_full_pipeline_with_runtime_ble_callbacks_and_perf_records);
    RUN_TEST(test_process_uses_provider_ble_callbacks_when_runtime_callbacks_missing);
    RUN_TEST(test_ble_process_disabled_skips_ble_process_only);
    RUN_TEST(test_obd_disabled_runs_obd_without_perf_record);
    RUN_TEST(test_missing_timing_hooks_still_runs_operations);
    RUN_TEST(test_empty_providers_is_safe_and_merges_flags);
    return UNITY_END();
}

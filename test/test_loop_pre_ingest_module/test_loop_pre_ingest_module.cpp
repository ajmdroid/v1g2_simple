#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_pre_ingest_module.cpp"

static LoopPreIngestModule module;

enum CallId {
    CALL_OPEN_BOOT_READY = 1,
    CALL_WIFI_PRIORITY = 2,
    CALL_DEBUG_API = 3,
};

static int callLog[24];
static size_t callLogCount = 0;

static uint32_t openedAtMs = 0;
static uint32_t wifiNowMs = 0;
static bool wifiObdEnabled = false;
static uint32_t debugNowMs = 0;

static int runtimeOpenCalls = 0;
static int runtimeWifiCalls = 0;
static int runtimeDebugCalls = 0;
static int providerOpenCalls = 0;
static int providerWifiCalls = 0;
static int providerDebugCalls = 0;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void resetState() {
    callLogCount = 0;

    openedAtMs = 0;
    wifiNowMs = 0;
    wifiObdEnabled = false;
    debugNowMs = 0;

    runtimeOpenCalls = 0;
    runtimeWifiCalls = 0;
    runtimeDebugCalls = 0;
    providerOpenCalls = 0;
    providerWifiCalls = 0;
    providerDebugCalls = 0;
}

static void runtimeOpenBootReady(uint32_t nowMs) {
    runtimeOpenCalls++;
    openedAtMs = nowMs;
    noteCall(CALL_OPEN_BOOT_READY);
}

static void runtimeWifiPriority(uint32_t nowMs, bool obdServiceEnabled) {
    runtimeWifiCalls++;
    wifiNowMs = nowMs;
    wifiObdEnabled = obdServiceEnabled;
    noteCall(CALL_WIFI_PRIORITY);
}

static void runtimeDebugApi(uint32_t nowMs) {
    runtimeDebugCalls++;
    debugNowMs = nowMs;
    noteCall(CALL_DEBUG_API);
}

static void providerOpenBootReady(void*, uint32_t nowMs) {
    providerOpenCalls++;
    openedAtMs = nowMs;
    noteCall(CALL_OPEN_BOOT_READY);
}

static void providerWifiPriority(void*, uint32_t nowMs, bool obdServiceEnabled) {
    providerWifiCalls++;
    wifiNowMs = nowMs;
    wifiObdEnabled = obdServiceEnabled;
    noteCall(CALL_WIFI_PRIORITY);
}

static void providerDebugApi(void*, uint32_t nowMs) {
    providerDebugCalls++;
    debugNowMs = nowMs;
    noteCall(CALL_DEBUG_API);
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_non_replay_timeout_opens_boot_ready_then_runs_wifi_and_debug() {
    LoopPreIngestModule::Providers providers;
    module.begin(providers);

    LoopPreIngestContext ctx;
    ctx.nowMs = 5000;
    ctx.bootReady = false;
    ctx.bootReadyDeadlineMs = 4000;
    ctx.obdServiceEnabled = true;
    ctx.replayMode = false;
    ctx.openBootReadyGate = runtimeOpenBootReady;
    ctx.runWifiPriorityApply = runtimeWifiPriority;
    ctx.runDebugApiProcess = runtimeDebugApi;

    const LoopPreIngestResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.bootReady);
    TEST_ASSERT_TRUE(result.bootReadyOpenedByTimeout);
    TEST_ASSERT_TRUE(result.runBleProcessThisLoop);

    TEST_ASSERT_EQUAL(1, runtimeOpenCalls);
    TEST_ASSERT_EQUAL(1, runtimeWifiCalls);
    TEST_ASSERT_EQUAL(1, runtimeDebugCalls);
    TEST_ASSERT_EQUAL(5000u, openedAtMs);
    TEST_ASSERT_EQUAL(5000u, wifiNowMs);
    TEST_ASSERT_TRUE(wifiObdEnabled);
    TEST_ASSERT_EQUAL(5000u, debugNowMs);

    TEST_ASSERT_EQUAL(3, callLogCount);
    TEST_ASSERT_EQUAL(CALL_OPEN_BOOT_READY, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_WIFI_PRIORITY, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_DEBUG_API, callLog[2]);
}

void test_non_replay_before_deadline_skips_boot_open_but_runs_wifi_and_debug() {
    LoopPreIngestModule::Providers providers;
    module.begin(providers);

    LoopPreIngestContext ctx;
    ctx.nowMs = 2999;
    ctx.bootReady = false;
    ctx.bootReadyDeadlineMs = 3000;
    ctx.obdServiceEnabled = false;
    ctx.openBootReadyGate = runtimeOpenBootReady;
    ctx.runWifiPriorityApply = runtimeWifiPriority;
    ctx.runDebugApiProcess = runtimeDebugApi;

    const LoopPreIngestResult result = module.process(ctx);

    TEST_ASSERT_FALSE(result.bootReady);
    TEST_ASSERT_FALSE(result.bootReadyOpenedByTimeout);
    TEST_ASSERT_TRUE(result.runBleProcessThisLoop);
    TEST_ASSERT_EQUAL(0, runtimeOpenCalls);
    TEST_ASSERT_EQUAL(1, runtimeWifiCalls);
    TEST_ASSERT_EQUAL(1, runtimeDebugCalls);
    TEST_ASSERT_FALSE(wifiObdEnabled);

    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_WIFI_PRIORITY, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_DEBUG_API, callLog[1]);
}

void test_replay_mode_skips_boot_open_and_wifi_but_keeps_debug() {
    LoopPreIngestModule::Providers providers;
    module.begin(providers);

    LoopPreIngestContext ctx;
    ctx.nowMs = 7000;
    ctx.bootReady = false;
    ctx.bootReadyDeadlineMs = 1000;
    ctx.obdServiceEnabled = true;
    ctx.replayMode = true;
    ctx.openBootReadyGate = runtimeOpenBootReady;
    ctx.runWifiPriorityApply = runtimeWifiPriority;
    ctx.runDebugApiProcess = runtimeDebugApi;

    const LoopPreIngestResult result = module.process(ctx);

    TEST_ASSERT_FALSE(result.bootReady);
    TEST_ASSERT_FALSE(result.bootReadyOpenedByTimeout);
    TEST_ASSERT_FALSE(result.runBleProcessThisLoop);

    TEST_ASSERT_EQUAL(0, runtimeOpenCalls);
    TEST_ASSERT_EQUAL(0, runtimeWifiCalls);
    TEST_ASSERT_EQUAL(1, runtimeDebugCalls);
    TEST_ASSERT_EQUAL(7000u, debugNowMs);

    TEST_ASSERT_EQUAL(1, callLogCount);
    TEST_ASSERT_EQUAL(CALL_DEBUG_API, callLog[0]);
}

void test_provider_fallback_path_works() {
    LoopPreIngestModule::Providers providers;
    providers.openBootReadyGate = providerOpenBootReady;
    providers.runWifiPriorityApply = providerWifiPriority;
    providers.runDebugApiProcess = providerDebugApi;
    module.begin(providers);

    LoopPreIngestContext ctx;
    ctx.nowMs = 900;
    ctx.bootReady = false;
    ctx.bootReadyDeadlineMs = 100;
    ctx.obdServiceEnabled = true;

    const LoopPreIngestResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.bootReady);
    TEST_ASSERT_TRUE(result.bootReadyOpenedByTimeout);
    TEST_ASSERT_TRUE(result.runBleProcessThisLoop);
    TEST_ASSERT_EQUAL(1, providerOpenCalls);
    TEST_ASSERT_EQUAL(1, providerWifiCalls);
    TEST_ASSERT_EQUAL(1, providerDebugCalls);
}

void test_empty_handlers_is_safe_and_keeps_expected_flags() {
    LoopPreIngestModule::Providers providers;
    module.begin(providers);

    LoopPreIngestContext ctx;
    ctx.nowMs = 10;
    ctx.bootReady = true;
    ctx.bootReadyDeadlineMs = 20;
    ctx.replayMode = false;

    const LoopPreIngestResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.bootReady);
    TEST_ASSERT_FALSE(result.bootReadyOpenedByTimeout);
    TEST_ASSERT_TRUE(result.runBleProcessThisLoop);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_non_replay_timeout_opens_boot_ready_then_runs_wifi_and_debug);
    RUN_TEST(test_non_replay_before_deadline_skips_boot_open_but_runs_wifi_and_debug);
    RUN_TEST(test_replay_mode_skips_boot_open_and_wifi_but_keeps_debug);
    RUN_TEST(test_provider_fallback_path_works);
    RUN_TEST(test_empty_handlers_is_safe_and_keeps_expected_flags);
    return UNITY_END();
}

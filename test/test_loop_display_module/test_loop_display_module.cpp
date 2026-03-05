#include <unity.h>
#include <initializer_list>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_display_module.cpp"

static LoopDisplayModule module;

enum CallId {
    CALL_COLLECT = 1,
    CALL_PARSED = 2,
    CALL_LOCKOUT_PERF = 3,
    CALL_NOTIFY_PERF = 4,
    CALL_PIPELINE = 5,
    CALL_DISP_PIPE_PERF = 6,
    CALL_REFRESH = 7,
};

static int callLog[32];
static size_t callLogCount = 0;

static uint32_t perfTsSequence[8];
static size_t perfTsCount = 0;
static size_t perfTsIndex = 0;

static uint32_t displayNowMs = 0;
static ParsedFrameSignal parsedSignal;
static DisplayOrchestrationParsedResult parsedResult;
static DisplayOrchestrationRefreshResult refreshResult;

static DisplayOrchestrationParsedContext lastParsedCtx;
static DisplayOrchestrationRefreshContext lastRefreshCtx;
static uint32_t lastPipelineNowMs = 0;
static bool lastPipelineSuppressed = false;

static uint32_t lockoutElapsedUs = 0;
static uint32_t dispPipeElapsedUs = 0;
static uint32_t notifyElapsedMs = 0;

static int collectCalls = 0;
static int parsedCalls = 0;
static int refreshCalls = 0;
static int runtimePipelineCalls = 0;
static int providerPipelineCalls = 0;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void setPerfTsSequence(std::initializer_list<uint32_t> values) {
    perfTsCount = values.size();
    perfTsIndex = 0;
    size_t i = 0;
    for (uint32_t value : values) {
        perfTsSequence[i++] = value;
    }
}

static void resetState() {
    callLogCount = 0;
    perfTsCount = 0;
    perfTsIndex = 0;

    displayNowMs = 0;
    parsedSignal = ParsedFrameSignal{};
    parsedResult = DisplayOrchestrationParsedResult{};
    refreshResult = DisplayOrchestrationRefreshResult{};
    lastParsedCtx = DisplayOrchestrationParsedContext{};
    lastRefreshCtx = DisplayOrchestrationRefreshContext{};
    lastPipelineNowMs = 0;
    lastPipelineSuppressed = false;
    lockoutElapsedUs = 0;
    dispPipeElapsedUs = 0;
    notifyElapsedMs = 0;
    collectCalls = 0;
    parsedCalls = 0;
    refreshCalls = 0;
    runtimePipelineCalls = 0;
    providerPipelineCalls = 0;
}

static uint32_t readDisplayNowMs(void*) {
    return displayNowMs;
}

static ParsedFrameSignal collectParsedSignal(void*) {
    collectCalls++;
    noteCall(CALL_COLLECT);
    return parsedSignal;
}

static DisplayOrchestrationParsedResult runParsedFrame(
    void*,
    const DisplayOrchestrationParsedContext& ctx) {
    parsedCalls++;
    lastParsedCtx = ctx;
    noteCall(CALL_PARSED);
    return parsedResult;
}

static DisplayOrchestrationRefreshResult runRefresh(
    void*,
    const DisplayOrchestrationRefreshContext& ctx) {
    refreshCalls++;
    lastRefreshCtx = ctx;
    noteCall(CALL_REFRESH);
    return refreshResult;
}

static void runRuntimePipeline(uint32_t nowMs, bool lockoutPrioritySuppressed) {
    runtimePipelineCalls++;
    lastPipelineNowMs = nowMs;
    lastPipelineSuppressed = lockoutPrioritySuppressed;
    noteCall(CALL_PIPELINE);
}

static void runProviderPipeline(void*, uint32_t nowMs, bool lockoutPrioritySuppressed) {
    providerPipelineCalls++;
    lastPipelineNowMs = nowMs;
    lastPipelineSuppressed = lockoutPrioritySuppressed;
    noteCall(CALL_PIPELINE);
}

static uint32_t nextPerfTs(void*) {
    if (perfTsCount == 0) {
        return 0;
    }
    if (perfTsIndex >= perfTsCount) {
        return perfTsSequence[perfTsCount - 1];
    }
    return perfTsSequence[perfTsIndex++];
}

static void recordLockoutUs(void*, uint32_t elapsedUs) {
    lockoutElapsedUs = elapsedUs;
    noteCall(CALL_LOCKOUT_PERF);
}

static void recordDispPipeUs(void*, uint32_t elapsedUs) {
    dispPipeElapsedUs = elapsedUs;
    noteCall(CALL_DISP_PIPE_PERF);
}

static void recordNotifyToDisplayMs(void*, uint32_t elapsedMs) {
    notifyElapsedMs = elapsedMs;
    noteCall(CALL_NOTIFY_PERF);
}

static LoopDisplayModule::Providers makeDefaultProviders() {
    LoopDisplayModule::Providers providers;
    providers.readDisplayNowMs = readDisplayNowMs;
    providers.collectParsedSignal = collectParsedSignal;
    providers.runParsedFrame = runParsedFrame;
    providers.runLightweightRefresh = runRefresh;
    providers.timestampUs = nextPerfTs;
    providers.recordLockoutUs = recordLockoutUs;
    providers.recordDispPipeUs = recordDispPipeUs;
    providers.recordNotifyToDisplayMs = recordNotifyToDisplayMs;
    return providers;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_process_full_pipeline_with_runtime_callback_and_perf_records() {
    LoopDisplayModule::Providers providers = makeDefaultProviders();
    module.begin(providers);

    displayNowMs = 1300;
    parsedSignal = ParsedFrameSignal{true, 1200};
    parsedResult = DisplayOrchestrationParsedResult{true, true, true};
    refreshResult.signalPriorityActive = true;
    setPerfTsSequence({100, 155, 200, 260});

    LoopDisplayContext ctx;
    ctx.nowMs = 900;
    ctx.bootSplashHoldActive = false;
    ctx.overloadLateThisLoop = true;
    ctx.enableSignalTraceLogging = true;
    ctx.runDisplayPipeline = runRuntimePipeline;

    const LoopDisplayResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.signalPriorityActive);
    TEST_ASSERT_EQUAL(1, collectCalls);
    TEST_ASSERT_EQUAL(1, parsedCalls);
    TEST_ASSERT_EQUAL(1, refreshCalls);
    TEST_ASSERT_EQUAL(1, runtimePipelineCalls);
    TEST_ASSERT_EQUAL(0, providerPipelineCalls);
    TEST_ASSERT_EQUAL(55u, lockoutElapsedUs);
    TEST_ASSERT_EQUAL(60u, dispPipeElapsedUs);
    TEST_ASSERT_EQUAL(100u, notifyElapsedMs);
    TEST_ASSERT_EQUAL(1300u, lastPipelineNowMs);
    TEST_ASSERT_TRUE(lastPipelineSuppressed);

    TEST_ASSERT_TRUE(lastParsedCtx.parsedReady);
    TEST_ASSERT_EQUAL(1300u, lastParsedCtx.nowMs);
    TEST_ASSERT_FALSE(lastParsedCtx.bootSplashHoldActive);
    TEST_ASSERT_TRUE(lastParsedCtx.enableSignalTraceLogging);

    TEST_ASSERT_EQUAL(1300u, lastRefreshCtx.nowMs);
    TEST_ASSERT_FALSE(lastRefreshCtx.bootSplashHoldActive);
    TEST_ASSERT_TRUE(lastRefreshCtx.overloadLateThisLoop);
    TEST_ASSERT_TRUE(lastRefreshCtx.pipelineRanThisLoop);

    TEST_ASSERT_EQUAL(7, callLogCount);
    TEST_ASSERT_EQUAL(CALL_COLLECT, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_PARSED, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_LOCKOUT_PERF, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_NOTIFY_PERF, callLog[3]);
    TEST_ASSERT_EQUAL(CALL_PIPELINE, callLog[4]);
    TEST_ASSERT_EQUAL(CALL_DISP_PIPE_PERF, callLog[5]);
    TEST_ASSERT_EQUAL(CALL_REFRESH, callLog[6]);
}

void test_process_uses_provider_pipeline_when_runtime_callback_missing() {
    LoopDisplayModule::Providers providers = makeDefaultProviders();
    providers.runDisplayPipeline = runProviderPipeline;
    module.begin(providers);

    displayNowMs = 2200;
    parsedSignal = ParsedFrameSignal{true, 2100};
    parsedResult = DisplayOrchestrationParsedResult{false, false, true};
    refreshResult.signalPriorityActive = false;
    setPerfTsSequence({500, 525, 700, 725});

    LoopDisplayContext ctx;
    ctx.nowMs = 2200;
    const LoopDisplayResult result = module.process(ctx);

    TEST_ASSERT_FALSE(result.signalPriorityActive);
    TEST_ASSERT_EQUAL(0, runtimePipelineCalls);
    TEST_ASSERT_EQUAL(1, providerPipelineCalls);
    TEST_ASSERT_EQUAL(175u, dispPipeElapsedUs);
    TEST_ASSERT_EQUAL(100u, notifyElapsedMs);
    TEST_ASSERT_EQUAL(2200u, lastPipelineNowMs);
    TEST_ASSERT_FALSE(lastPipelineSuppressed);
}

void test_process_skips_pipeline_when_parsed_result_disables_pipeline() {
    LoopDisplayModule::Providers providers = makeDefaultProviders();
    providers.runDisplayPipeline = runProviderPipeline;
    module.begin(providers);

    displayNowMs = 5000;
    parsedSignal = ParsedFrameSignal{true, 4900};
    parsedResult = DisplayOrchestrationParsedResult{true, true, false};
    refreshResult.signalPriorityActive = true;
    setPerfTsSequence({1000, 1100});

    LoopDisplayContext ctx;
    ctx.nowMs = 5000;
    ctx.overloadLateThisLoop = true;
    const LoopDisplayResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.signalPriorityActive);
    TEST_ASSERT_EQUAL(1, parsedCalls);
    TEST_ASSERT_EQUAL(1, refreshCalls);
    TEST_ASSERT_EQUAL(0, runtimePipelineCalls);
    TEST_ASSERT_EQUAL(0, providerPipelineCalls);
    TEST_ASSERT_EQUAL(100u, lockoutElapsedUs);
    TEST_ASSERT_EQUAL(0u, dispPipeElapsedUs);
    TEST_ASSERT_EQUAL(0u, notifyElapsedMs);
    TEST_ASSERT_FALSE(lastRefreshCtx.pipelineRanThisLoop);
}

void test_notify_to_display_skips_for_zero_or_future_timestamp() {
    LoopDisplayModule::Providers providers = makeDefaultProviders();
    providers.runDisplayPipeline = runProviderPipeline;
    module.begin(providers);

    displayNowMs = 3000;
    parsedSignal = ParsedFrameSignal{true, 0};
    parsedResult = DisplayOrchestrationParsedResult{false, false, true};
    module.process(LoopDisplayContext{});
    TEST_ASSERT_EQUAL(0u, notifyElapsedMs);

    resetState();
    module.begin(providers);
    displayNowMs = 3000;
    parsedSignal = ParsedFrameSignal{true, 3200};
    parsedResult = DisplayOrchestrationParsedResult{false, false, true};
    module.process(LoopDisplayContext{});
    TEST_ASSERT_EQUAL(0u, notifyElapsedMs);
}

void test_wrap_safe_perf_elapsed_for_lockout_and_pipeline() {
    LoopDisplayModule::Providers providers = makeDefaultProviders();
    providers.runDisplayPipeline = runProviderPipeline;
    module.begin(providers);

    displayNowMs = 1000;
    parsedSignal = ParsedFrameSignal{true, 900};
    parsedResult = DisplayOrchestrationParsedResult{true, false, true};
    setPerfTsSequence({0xFFFFFFF0u, 0x00000010u, 0xFFFFFF00u, 0x00000020u});

    module.process(LoopDisplayContext{});

    TEST_ASSERT_EQUAL(0x20u, lockoutElapsedUs);
    TEST_ASSERT_EQUAL(0x120u, dispPipeElapsedUs);
}

void test_empty_providers_is_safe_noop() {
    LoopDisplayModule::Providers providers;
    module.begin(providers);

    LoopDisplayContext ctx;
    ctx.nowMs = 1234;
    const LoopDisplayResult result = module.process(ctx);

    TEST_ASSERT_FALSE(result.signalPriorityActive);
    TEST_ASSERT_EQUAL(0, collectCalls);
    TEST_ASSERT_EQUAL(0, parsedCalls);
    TEST_ASSERT_EQUAL(0, refreshCalls);
    TEST_ASSERT_EQUAL(0, runtimePipelineCalls);
    TEST_ASSERT_EQUAL(0, providerPipelineCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_full_pipeline_with_runtime_callback_and_perf_records);
    RUN_TEST(test_process_uses_provider_pipeline_when_runtime_callback_missing);
    RUN_TEST(test_process_skips_pipeline_when_parsed_result_disables_pipeline);
    RUN_TEST(test_notify_to_display_skips_for_zero_or_future_timestamp);
    RUN_TEST(test_wrap_safe_perf_elapsed_for_lockout_and_pipeline);
    RUN_TEST(test_empty_providers_is_safe_noop);
    return UNITY_END();
}

#include <unity.h>
#include <initializer_list>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_post_display_module.cpp"

static LoopPostDisplayModule module;

enum CallId {
    CALL_AUTO_PUSH = 1,
    CALL_CAMERA = 2,
    CALL_CAMERA_PERF = 3,
    CALL_SPEED_VOLUME = 4,
    CALL_DISPATCH = 5,
};

static int callLog[32];
static size_t callLogCount = 0;

static uint32_t perfTsSequence[8];
static size_t perfTsCount = 0;
static size_t perfTsIndex = 0;

static uint32_t providerDispatchNowMs = 0;
static bool providerBleConnected = false;

static uint32_t cameraNowMs = 0;
static bool cameraSkipLateNonCore = false;
static bool cameraOverloadLate = false;
static bool cameraSignalPriority = false;
static uint32_t cameraElapsedUs = 0;

static SpeedVolumeRuntimeContext lastSpeedCtx;
static ConnectionStateDispatchContext lastDispatchCtx;

static int runtimeAutoPushCalls = 0;
static int runtimeCameraCalls = 0;
static int runtimeSpeedCalls = 0;
static int runtimeDispatchCalls = 0;

static int providerAutoPushCalls = 0;
static int providerCameraCalls = 0;
static int providerSpeedCalls = 0;
static int providerDispatchCalls = 0;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void resetState() {
    callLogCount = 0;
    perfTsCount = 0;
    perfTsIndex = 0;

    providerDispatchNowMs = 0;
    providerBleConnected = false;

    cameraNowMs = 0;
    cameraSkipLateNonCore = false;
    cameraOverloadLate = false;
    cameraSignalPriority = false;
    cameraElapsedUs = 0;
    lastSpeedCtx = SpeedVolumeRuntimeContext{};
    lastDispatchCtx = ConnectionStateDispatchContext{};

    runtimeAutoPushCalls = 0;
    runtimeCameraCalls = 0;
    runtimeSpeedCalls = 0;
    runtimeDispatchCalls = 0;

    providerAutoPushCalls = 0;
    providerCameraCalls = 0;
    providerSpeedCalls = 0;
    providerDispatchCalls = 0;
}

static void setPerfTsSequence(std::initializer_list<uint32_t> values) {
    perfTsCount = values.size();
    perfTsIndex = 0;
    size_t i = 0;
    for (uint32_t value : values) {
        perfTsSequence[i++] = value;
    }
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

static void runRuntimeAutoPush() {
    runtimeAutoPushCalls++;
    noteCall(CALL_AUTO_PUSH);
}

static void runRuntimeCamera(uint32_t nowMs,
                             bool skipLateNonCoreThisLoop,
                             bool overloadLateThisLoop,
                             bool loopSignalPriorityActive) {
    runtimeCameraCalls++;
    cameraNowMs = nowMs;
    cameraSkipLateNonCore = skipLateNonCoreThisLoop;
    cameraOverloadLate = overloadLateThisLoop;
    cameraSignalPriority = loopSignalPriorityActive;
    noteCall(CALL_CAMERA);
}

static void runRuntimeSpeed(const SpeedVolumeRuntimeContext& speedVolumeCtx) {
    runtimeSpeedCalls++;
    lastSpeedCtx = speedVolumeCtx;
    noteCall(CALL_SPEED_VOLUME);
}

static void runRuntimeDispatch(const ConnectionStateDispatchContext& dispatchCtx) {
    runtimeDispatchCalls++;
    lastDispatchCtx = dispatchCtx;
    noteCall(CALL_DISPATCH);
}

static void runProviderAutoPush(void*) {
    providerAutoPushCalls++;
    noteCall(CALL_AUTO_PUSH);
}

static void runProviderCamera(void*,
                              uint32_t nowMs,
                              bool skipLateNonCoreThisLoop,
                              bool overloadLateThisLoop,
                              bool loopSignalPriorityActive) {
    providerCameraCalls++;
    cameraNowMs = nowMs;
    cameraSkipLateNonCore = skipLateNonCoreThisLoop;
    cameraOverloadLate = overloadLateThisLoop;
    cameraSignalPriority = loopSignalPriorityActive;
    noteCall(CALL_CAMERA);
}

static void recordCameraUs(void*, uint32_t elapsedUs) {
    cameraElapsedUs = elapsedUs;
    noteCall(CALL_CAMERA_PERF);
}

static void runProviderSpeed(void*, const SpeedVolumeRuntimeContext& speedVolumeCtx) {
    providerSpeedCalls++;
    lastSpeedCtx = speedVolumeCtx;
    noteCall(CALL_SPEED_VOLUME);
}

static uint32_t readDispatchNowMs(void*) {
    return providerDispatchNowMs;
}

static bool readBleConnectedNow(void*) {
    return providerBleConnected;
}

static void runProviderDispatch(void*, const ConnectionStateDispatchContext& dispatchCtx) {
    providerDispatchCalls++;
    lastDispatchCtx = dispatchCtx;
    noteCall(CALL_DISPATCH);
}

static LoopPostDisplayModule::Providers makeDefaultProviders() {
    LoopPostDisplayModule::Providers providers;
    providers.timestampUs = nextPerfTs;
    providers.recordCameraUs = recordCameraUs;
    providers.readDispatchNowMs = readDispatchNowMs;
    providers.readBleConnectedNow = readBleConnectedNow;
    return providers;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_process_runtime_callbacks_with_perf_and_dispatch_outputs() {
    LoopPostDisplayModule::Providers providers = makeDefaultProviders();
    module.begin(providers);

    providerDispatchNowMs = 2222;
    providerBleConnected = true;
    setPerfTsSequence({100, 135});

    LoopPostDisplayContext ctx;
    ctx.nowMs = 1500;
    ctx.skipLateNonCoreThisLoop = true;
    ctx.overloadLateThisLoop = true;
    ctx.loopSignalPriorityActive = true;
    ctx.configuredVoiceVolume = 8;
    ctx.displayUpdateIntervalMs = 60;
    ctx.scanScreenDwellMs = 333;
    ctx.bootSplashHoldActive = false;
    ctx.displayPreviewRunning = true;
    ctx.maxProcessGapMs = 1200;
    ctx.runAutoPush = runRuntimeAutoPush;
    ctx.runCameraRuntime = runRuntimeCamera;
    ctx.runSpeedVolumeRuntime = runRuntimeSpeed;
    ctx.runConnectionStateDispatch = runRuntimeDispatch;

    const LoopPostDisplayResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(2222u, result.dispatchNowMs);
    TEST_ASSERT_TRUE(result.bleConnectedNow);
    TEST_ASSERT_EQUAL(1, runtimeAutoPushCalls);
    TEST_ASSERT_EQUAL(1, runtimeCameraCalls);
    TEST_ASSERT_EQUAL(1, runtimeSpeedCalls);
    TEST_ASSERT_EQUAL(1, runtimeDispatchCalls);
    TEST_ASSERT_EQUAL(35u, cameraElapsedUs);

    TEST_ASSERT_EQUAL(1500u, cameraNowMs);
    TEST_ASSERT_TRUE(cameraSkipLateNonCore);
    TEST_ASSERT_TRUE(cameraOverloadLate);
    TEST_ASSERT_TRUE(cameraSignalPriority);
    TEST_ASSERT_EQUAL(1500u, lastSpeedCtx.nowMs);
    TEST_ASSERT_EQUAL(8u, lastSpeedCtx.configuredVoiceVolume);

    TEST_ASSERT_EQUAL(2222u, lastDispatchCtx.nowMs);
    TEST_ASSERT_EQUAL(60u, lastDispatchCtx.displayUpdateIntervalMs);
    TEST_ASSERT_EQUAL(333u, lastDispatchCtx.scanScreenDwellMs);
    TEST_ASSERT_TRUE(lastDispatchCtx.bleConnectedNow);
    TEST_ASSERT_FALSE(lastDispatchCtx.bootSplashHoldActive);
    TEST_ASSERT_TRUE(lastDispatchCtx.displayPreviewRunning);
    TEST_ASSERT_EQUAL(1200u, lastDispatchCtx.maxProcessGapMs);

    TEST_ASSERT_EQUAL(5, callLogCount);
    TEST_ASSERT_EQUAL(CALL_AUTO_PUSH, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_CAMERA, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_CAMERA_PERF, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_SPEED_VOLUME, callLog[3]);
    TEST_ASSERT_EQUAL(CALL_DISPATCH, callLog[4]);
}

void test_process_provider_fallback_path_and_ctx_dispatch_fallback() {
    LoopPostDisplayModule::Providers providers = makeDefaultProviders();
    providers.runAutoPush = runProviderAutoPush;
    providers.runCameraRuntime = runProviderCamera;
    providers.runSpeedVolumeRuntime = runProviderSpeed;
    providers.runConnectionStateDispatch = runProviderDispatch;
    providers.readDispatchNowMs = nullptr;
    providers.readBleConnectedNow = nullptr;
    module.begin(providers);

    setPerfTsSequence({500, 545});
    LoopPostDisplayContext ctx;
    ctx.nowMs = 900;
    ctx.skipLateNonCoreThisLoop = false;
    ctx.overloadLateThisLoop = true;
    ctx.loopSignalPriorityActive = false;
    ctx.configuredVoiceVolume = 6;
    ctx.displayUpdateIntervalMs = 50;
    ctx.scanScreenDwellMs = 77;
    ctx.bootSplashHoldActive = true;
    ctx.displayPreviewRunning = false;
    ctx.maxProcessGapMs = 1000;
    ctx.bleConnectedNow = true;

    const LoopPostDisplayResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(900u, result.dispatchNowMs);
    TEST_ASSERT_TRUE(result.bleConnectedNow);
    TEST_ASSERT_EQUAL(1, providerAutoPushCalls);
    TEST_ASSERT_EQUAL(1, providerCameraCalls);
    TEST_ASSERT_EQUAL(1, providerSpeedCalls);
    TEST_ASSERT_EQUAL(1, providerDispatchCalls);
    TEST_ASSERT_EQUAL(45u, cameraElapsedUs);

    TEST_ASSERT_EQUAL(900u, lastDispatchCtx.nowMs);
    TEST_ASSERT_TRUE(lastDispatchCtx.bleConnectedNow);
    TEST_ASSERT_TRUE(lastDispatchCtx.bootSplashHoldActive);
    TEST_ASSERT_FALSE(lastDispatchCtx.displayPreviewRunning);
}

void test_missing_timing_hooks_still_runs_camera() {
    LoopPostDisplayModule::Providers providers;
    module.begin(providers);

    LoopPostDisplayContext ctx;
    ctx.nowMs = 3333;
    ctx.runCameraRuntime = runRuntimeCamera;

    module.process(ctx);

    TEST_ASSERT_EQUAL(1, runtimeCameraCalls);
    TEST_ASSERT_EQUAL(0u, cameraElapsedUs);
}

void test_camera_perf_elapsed_wrap_safe() {
    LoopPostDisplayModule::Providers providers = makeDefaultProviders();
    module.begin(providers);

    setPerfTsSequence({0xFFFFFFF0u, 0x00000010u});
    LoopPostDisplayContext ctx;
    ctx.runCameraRuntime = runRuntimeCamera;
    module.process(ctx);

    TEST_ASSERT_EQUAL(0x20u, cameraElapsedUs);
}

void test_empty_providers_returns_ctx_fallbacks() {
    LoopPostDisplayModule::Providers providers;
    module.begin(providers);

    LoopPostDisplayContext ctx;
    ctx.nowMs = 77;
    ctx.bleConnectedNow = true;

    const LoopPostDisplayResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(77u, result.dispatchNowMs);
    TEST_ASSERT_TRUE(result.bleConnectedNow);
}

void test_auto_push_camera_only_skips_speed_and_dispatch() {
    LoopPostDisplayModule::Providers providers = makeDefaultProviders();
    module.begin(providers);

    setPerfTsSequence({1000, 1012});
    LoopPostDisplayContext ctx;
    ctx.runAutoPushAndCamera = true;
    ctx.runSpeedAndDispatch = false;
    ctx.nowMs = 4321;
    ctx.skipLateNonCoreThisLoop = true;
    ctx.overloadLateThisLoop = false;
    ctx.loopSignalPriorityActive = true;
    ctx.bleConnectedNow = true;
    ctx.runAutoPush = runRuntimeAutoPush;
    ctx.runCameraRuntime = runRuntimeCamera;
    ctx.runSpeedVolumeRuntime = runRuntimeSpeed;
    ctx.runConnectionStateDispatch = runRuntimeDispatch;

    const LoopPostDisplayResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(1, runtimeAutoPushCalls);
    TEST_ASSERT_EQUAL(1, runtimeCameraCalls);
    TEST_ASSERT_EQUAL(0, runtimeSpeedCalls);
    TEST_ASSERT_EQUAL(0, runtimeDispatchCalls);
    TEST_ASSERT_EQUAL(12u, cameraElapsedUs);
    TEST_ASSERT_EQUAL(4321u, result.dispatchNowMs);
    TEST_ASSERT_TRUE(result.bleConnectedNow);
    TEST_ASSERT_EQUAL(3, callLogCount);
    TEST_ASSERT_EQUAL(CALL_AUTO_PUSH, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_CAMERA, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_CAMERA_PERF, callLog[2]);
}

void test_speed_dispatch_only_skips_auto_push_and_camera() {
    LoopPostDisplayModule::Providers providers = makeDefaultProviders();
    module.begin(providers);

    providerDispatchNowMs = 2468;
    providerBleConnected = false;
    LoopPostDisplayContext ctx;
    ctx.runAutoPushAndCamera = false;
    ctx.runSpeedAndDispatch = true;
    ctx.nowMs = 1111;
    ctx.configuredVoiceVolume = 9;
    ctx.displayUpdateIntervalMs = 66;
    ctx.scanScreenDwellMs = 777;
    ctx.bootSplashHoldActive = false;
    ctx.displayPreviewRunning = true;
    ctx.maxProcessGapMs = 3210;
    ctx.runAutoPush = runRuntimeAutoPush;
    ctx.runCameraRuntime = runRuntimeCamera;
    ctx.runSpeedVolumeRuntime = runRuntimeSpeed;
    ctx.runConnectionStateDispatch = runRuntimeDispatch;

    const LoopPostDisplayResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(0, runtimeAutoPushCalls);
    TEST_ASSERT_EQUAL(0, runtimeCameraCalls);
    TEST_ASSERT_EQUAL(1, runtimeSpeedCalls);
    TEST_ASSERT_EQUAL(1, runtimeDispatchCalls);
    TEST_ASSERT_EQUAL(2468u, result.dispatchNowMs);
    TEST_ASSERT_FALSE(result.bleConnectedNow);
    TEST_ASSERT_EQUAL(1111u, lastSpeedCtx.nowMs);
    TEST_ASSERT_EQUAL(9u, lastSpeedCtx.configuredVoiceVolume);
    TEST_ASSERT_EQUAL(2468u, lastDispatchCtx.nowMs);
    TEST_ASSERT_EQUAL(66u, lastDispatchCtx.displayUpdateIntervalMs);
    TEST_ASSERT_EQUAL(777u, lastDispatchCtx.scanScreenDwellMs);
    TEST_ASSERT_FALSE(lastDispatchCtx.bleConnectedNow);
    TEST_ASSERT_FALSE(lastDispatchCtx.bootSplashHoldActive);
    TEST_ASSERT_TRUE(lastDispatchCtx.displayPreviewRunning);
    TEST_ASSERT_EQUAL(3210u, lastDispatchCtx.maxProcessGapMs);
    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_SPEED_VOLUME, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_DISPATCH, callLog[1]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_runtime_callbacks_with_perf_and_dispatch_outputs);
    RUN_TEST(test_process_provider_fallback_path_and_ctx_dispatch_fallback);
    RUN_TEST(test_missing_timing_hooks_still_runs_camera);
    RUN_TEST(test_camera_perf_elapsed_wrap_safe);
    RUN_TEST(test_empty_providers_returns_ctx_fallbacks);
    RUN_TEST(test_auto_push_camera_only_skips_speed_and_dispatch);
    RUN_TEST(test_speed_dispatch_only_skips_auto_push_and_camera);
    return UNITY_END();
}

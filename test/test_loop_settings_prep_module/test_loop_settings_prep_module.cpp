#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_settings_prep_module.cpp"

static LoopSettingsPrepModule module;

enum CallId {
    CALL_TAP = 1,
    CALL_SETTINGS = 2,
};

static int callLog[16];
static size_t callLogCount = 0;

static uint32_t tapNowMs = 0;
static int runtimeTapCalls = 0;
static int providerTapCalls = 0;
static int runtimeSettingsCalls = 0;
static int providerSettingsCalls = 0;

static LoopSettingsPrepValues runtimeValues;
static LoopSettingsPrepValues providerValues;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void resetState() {
    callLogCount = 0;
    tapNowMs = 0;
    runtimeTapCalls = 0;
    providerTapCalls = 0;
    runtimeSettingsCalls = 0;
    providerSettingsCalls = 0;
    runtimeValues = LoopSettingsPrepValues{};
    providerValues = LoopSettingsPrepValues{};
}

static void runRuntimeTap(uint32_t nowMs) {
    runtimeTapCalls++;
    tapNowMs = nowMs;
    noteCall(CALL_TAP);
}

static void runProviderTap(void*, uint32_t nowMs) {
    providerTapCalls++;
    tapNowMs = nowMs;
    noteCall(CALL_TAP);
}

static LoopSettingsPrepValues readRuntimeSettings() {
    runtimeSettingsCalls++;
    noteCall(CALL_SETTINGS);
    return runtimeValues;
}

static LoopSettingsPrepValues readProviderSettings(void*) {
    providerSettingsCalls++;
    noteCall(CALL_SETTINGS);
    return providerValues;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_runtime_callbacks_path_runs_tap_then_reads_settings() {
    LoopSettingsPrepModule::Providers providers;
    module.begin(providers);

    runtimeValues.obdServiceEnabled = true;
    runtimeValues.enableWifiAtBoot = true;
    runtimeValues.enableSignalTraceLogging = false;
    runtimeValues.configuredVoiceVolume = 88;

    LoopSettingsPrepContext ctx;
    ctx.nowMs = 123;
    ctx.runTapGesture = runRuntimeTap;
    ctx.readSettingsValues = readRuntimeSettings;

    const LoopSettingsPrepValues result = module.process(ctx);

    TEST_ASSERT_EQUAL(1, runtimeTapCalls);
    TEST_ASSERT_EQUAL(0, providerTapCalls);
    TEST_ASSERT_EQUAL(1, runtimeSettingsCalls);
    TEST_ASSERT_EQUAL(0, providerSettingsCalls);
    TEST_ASSERT_EQUAL(123u, tapNowMs);

    TEST_ASSERT_TRUE(result.obdServiceEnabled);
    TEST_ASSERT_TRUE(result.enableWifiAtBoot);
    TEST_ASSERT_FALSE(result.enableSignalTraceLogging);
    TEST_ASSERT_EQUAL_UINT8(88, result.configuredVoiceVolume);

    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_TAP, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_SETTINGS, callLog[1]);
}

void test_provider_fallback_path_runs_tap_then_reads_settings() {
    LoopSettingsPrepModule::Providers providers;
    providers.runTapGesture = runProviderTap;
    providers.readSettingsValues = readProviderSettings;
    module.begin(providers);

    providerValues.obdServiceEnabled = false;
    providerValues.enableWifiAtBoot = true;
    providerValues.enableSignalTraceLogging = true;
    providerValues.configuredVoiceVolume = 42;

    LoopSettingsPrepContext ctx;
    ctx.nowMs = 456;

    const LoopSettingsPrepValues result = module.process(ctx);

    TEST_ASSERT_EQUAL(0, runtimeTapCalls);
    TEST_ASSERT_EQUAL(1, providerTapCalls);
    TEST_ASSERT_EQUAL(0, runtimeSettingsCalls);
    TEST_ASSERT_EQUAL(1, providerSettingsCalls);
    TEST_ASSERT_EQUAL(456u, tapNowMs);

    TEST_ASSERT_FALSE(result.obdServiceEnabled);
    TEST_ASSERT_TRUE(result.enableWifiAtBoot);
    TEST_ASSERT_TRUE(result.enableSignalTraceLogging);
    TEST_ASSERT_EQUAL_UINT8(42, result.configuredVoiceVolume);

    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_TAP, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_SETTINGS, callLog[1]);
}

void test_runtime_read_overrides_provider_read() {
    LoopSettingsPrepModule::Providers providers;
    providers.readSettingsValues = readProviderSettings;
    module.begin(providers);

    runtimeValues.obdServiceEnabled = true;
    runtimeValues.enableWifiAtBoot = false;
    runtimeValues.enableSignalTraceLogging = true;
    runtimeValues.configuredVoiceVolume = 71;

    LoopSettingsPrepContext ctx;
    ctx.readSettingsValues = readRuntimeSettings;

    const LoopSettingsPrepValues result = module.process(ctx);

    TEST_ASSERT_EQUAL(1, runtimeSettingsCalls);
    TEST_ASSERT_EQUAL(0, providerSettingsCalls);
    TEST_ASSERT_TRUE(result.obdServiceEnabled);
    TEST_ASSERT_FALSE(result.enableWifiAtBoot);
    TEST_ASSERT_TRUE(result.enableSignalTraceLogging);
    TEST_ASSERT_EQUAL_UINT8(71, result.configuredVoiceVolume);
}

void test_empty_providers_and_context_returns_defaults() {
    LoopSettingsPrepModule::Providers providers;
    module.begin(providers);

    LoopSettingsPrepContext ctx;
    const LoopSettingsPrepValues result = module.process(ctx);

    TEST_ASSERT_FALSE(result.obdServiceEnabled);
    TEST_ASSERT_FALSE(result.enableWifiAtBoot);
    TEST_ASSERT_FALSE(result.enableSignalTraceLogging);
    TEST_ASSERT_EQUAL_UINT8(0, result.configuredVoiceVolume);
    TEST_ASSERT_EQUAL(0, runtimeTapCalls);
    TEST_ASSERT_EQUAL(0, providerTapCalls);
    TEST_ASSERT_EQUAL(0, runtimeSettingsCalls);
    TEST_ASSERT_EQUAL(0, providerSettingsCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_runtime_callbacks_path_runs_tap_then_reads_settings);
    RUN_TEST(test_provider_fallback_path_runs_tap_then_reads_settings);
    RUN_TEST(test_runtime_read_overrides_provider_read);
    RUN_TEST(test_empty_providers_and_context_returns_defaults);
    return UNITY_END();
}

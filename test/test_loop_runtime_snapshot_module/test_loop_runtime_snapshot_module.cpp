#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_runtime_snapshot_module.cpp"

static LoopRuntimeSnapshotModule module;

static int runtimeBleCalls = 0;
static int runtimeDmaCalls = 0;
static int runtimePreviewCalls = 0;

static int providerBleCalls = 0;
static int providerDmaCalls = 0;
static int providerPreviewCalls = 0;

static bool runtimeBleValue = false;
static bool runtimeDmaValue = false;
static bool runtimePreviewValue = false;

static bool providerBleValue = false;
static bool providerDmaValue = false;
static bool providerPreviewValue = false;

static void resetState() {
    runtimeBleCalls = 0;
    runtimeDmaCalls = 0;
    runtimePreviewCalls = 0;

    providerBleCalls = 0;
    providerDmaCalls = 0;
    providerPreviewCalls = 0;

    runtimeBleValue = false;
    runtimeDmaValue = false;
    runtimePreviewValue = false;

    providerBleValue = false;
    providerDmaValue = false;
    providerPreviewValue = false;
}

static bool readRuntimeBle() {
    runtimeBleCalls++;
    return runtimeBleValue;
}

static bool readRuntimeDma() {
    runtimeDmaCalls++;
    return runtimeDmaValue;
}

static bool readRuntimePreview() {
    runtimePreviewCalls++;
    return runtimePreviewValue;
}

static bool readProviderBle(void*) {
    providerBleCalls++;
    return providerBleValue;
}

static bool readProviderDma(void*) {
    providerDmaCalls++;
    return providerDmaValue;
}

static bool readProviderPreview(void*) {
    providerPreviewCalls++;
    return providerPreviewValue;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_runtime_callbacks_override_provider_values() {
    LoopRuntimeSnapshotModule::Providers providers;
    providers.readBleConnected = readProviderBle;
    providers.readCanStartDma = readProviderDma;
    providers.readDisplayPreviewRunning = readProviderPreview;
    module.begin(providers);

    runtimeBleValue = true;
    runtimeDmaValue = true;
    runtimePreviewValue = false;
    providerBleValue = false;
    providerDmaValue = false;
    providerPreviewValue = true;

    LoopRuntimeSnapshotContext ctx;
    ctx.readBleConnected = readRuntimeBle;
    ctx.readCanStartDma = readRuntimeDma;
    ctx.readDisplayPreviewRunning = readRuntimePreview;

    const LoopRuntimeSnapshotValues result = module.process(ctx);

    TEST_ASSERT_TRUE(result.bleConnected);
    TEST_ASSERT_TRUE(result.canStartDma);
    TEST_ASSERT_FALSE(result.displayPreviewRunning);
    TEST_ASSERT_EQUAL(1, runtimeBleCalls);
    TEST_ASSERT_EQUAL(1, runtimeDmaCalls);
    TEST_ASSERT_EQUAL(1, runtimePreviewCalls);
    TEST_ASSERT_EQUAL(0, providerBleCalls);
    TEST_ASSERT_EQUAL(0, providerDmaCalls);
    TEST_ASSERT_EQUAL(0, providerPreviewCalls);
}

void test_provider_fallback_used_when_runtime_callbacks_missing() {
    LoopRuntimeSnapshotModule::Providers providers;
    providers.readBleConnected = readProviderBle;
    providers.readCanStartDma = readProviderDma;
    providers.readDisplayPreviewRunning = readProviderPreview;
    module.begin(providers);

    providerBleValue = true;
    providerDmaValue = false;
    providerPreviewValue = true;

    LoopRuntimeSnapshotContext ctx;
    const LoopRuntimeSnapshotValues result = module.process(ctx);

    TEST_ASSERT_TRUE(result.bleConnected);
    TEST_ASSERT_FALSE(result.canStartDma);
    TEST_ASSERT_TRUE(result.displayPreviewRunning);
    TEST_ASSERT_EQUAL(1, providerBleCalls);
    TEST_ASSERT_EQUAL(1, providerDmaCalls);
    TEST_ASSERT_EQUAL(1, providerPreviewCalls);
    TEST_ASSERT_EQUAL(0, runtimeBleCalls);
    TEST_ASSERT_EQUAL(0, runtimeDmaCalls);
    TEST_ASSERT_EQUAL(0, runtimePreviewCalls);
}

void test_mixed_runtime_and_provider_paths_are_independent() {
    LoopRuntimeSnapshotModule::Providers providers;
    providers.readBleConnected = readProviderBle;
    providers.readCanStartDma = readProviderDma;
    providers.readDisplayPreviewRunning = readProviderPreview;
    module.begin(providers);

    runtimeBleValue = false;
    providerDmaValue = true;
    providerPreviewValue = false;

    LoopRuntimeSnapshotContext ctx;
    ctx.readBleConnected = readRuntimeBle;
    const LoopRuntimeSnapshotValues result = module.process(ctx);

    TEST_ASSERT_FALSE(result.bleConnected);
    TEST_ASSERT_TRUE(result.canStartDma);
    TEST_ASSERT_FALSE(result.displayPreviewRunning);
    TEST_ASSERT_EQUAL(1, runtimeBleCalls);
    TEST_ASSERT_EQUAL(0, runtimeDmaCalls);
    TEST_ASSERT_EQUAL(0, runtimePreviewCalls);
    TEST_ASSERT_EQUAL(0, providerBleCalls);
    TEST_ASSERT_EQUAL(1, providerDmaCalls);
    TEST_ASSERT_EQUAL(1, providerPreviewCalls);
}

void test_empty_providers_and_context_returns_safe_defaults() {
    LoopRuntimeSnapshotModule::Providers providers;
    module.begin(providers);

    LoopRuntimeSnapshotContext ctx;
    const LoopRuntimeSnapshotValues result = module.process(ctx);

    TEST_ASSERT_FALSE(result.bleConnected);
    TEST_ASSERT_FALSE(result.canStartDma);
    TEST_ASSERT_FALSE(result.displayPreviewRunning);
    TEST_ASSERT_EQUAL(0, runtimeBleCalls);
    TEST_ASSERT_EQUAL(0, runtimeDmaCalls);
    TEST_ASSERT_EQUAL(0, runtimePreviewCalls);
    TEST_ASSERT_EQUAL(0, providerBleCalls);
    TEST_ASSERT_EQUAL(0, providerDmaCalls);
    TEST_ASSERT_EQUAL(0, providerPreviewCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_runtime_callbacks_override_provider_values);
    RUN_TEST(test_provider_fallback_used_when_runtime_callbacks_missing);
    RUN_TEST(test_mixed_runtime_and_provider_paths_are_independent);
    RUN_TEST(test_empty_providers_and_context_returns_safe_defaults);
    return UNITY_END();
}

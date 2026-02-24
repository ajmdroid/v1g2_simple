#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/voice/voice_speed_sync_module.cpp"

static VoiceSpeedSyncModule module;

static bool scriptedSelectResult = false;
static SpeedSelection scriptedSelection;
static int selectCalls = 0;
static uint32_t lastSelectNowMs = 0;

static int updateCalls = 0;
static float lastUpdateSpeedMph = 0.0f;
static uint32_t lastUpdateTimestampMs = 0;

static int clearCalls = 0;

static void resetState() {
    scriptedSelectResult = false;
    scriptedSelection = SpeedSelection{};
    selectCalls = 0;
    lastSelectNowMs = 0;
    updateCalls = 0;
    lastUpdateSpeedMph = 0.0f;
    lastUpdateTimestampMs = 0;
    clearCalls = 0;
}

static bool selectSpeedSample(void*, uint32_t nowMs, SpeedSelection& selection) {
    selectCalls++;
    lastSelectNowMs = nowMs;
    selection = scriptedSelection;
    return scriptedSelectResult;
}

static void updateVoiceSpeedSample(void*, float speedMph, uint32_t timestampMs) {
    updateCalls++;
    lastUpdateSpeedMph = speedMph;
    lastUpdateTimestampMs = timestampMs;
}

static void clearVoiceSpeedSample(void*) {
    clearCalls++;
}

void setUp() {
    resetState();
    VoiceSpeedSyncModule::Providers providers;
    providers.selectSpeedSample = selectSpeedSample;
    providers.updateVoiceSpeedSample = updateVoiceSpeedSample;
    providers.clearVoiceSpeedSample = clearVoiceSpeedSample;
    module.begin(providers);
}

void tearDown() {}

void test_select_hit_updates_voice_and_does_not_clear() {
    scriptedSelectResult = true;
    scriptedSelection.speedMph = 61.5f;
    scriptedSelection.timestampMs = 1234;

    VoiceSpeedSyncContext ctx;
    ctx.nowMs = 2000;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, selectCalls);
    TEST_ASSERT_EQUAL(2000u, lastSelectNowMs);
    TEST_ASSERT_EQUAL(1, updateCalls);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 61.5f, lastUpdateSpeedMph);
    TEST_ASSERT_EQUAL(1234u, lastUpdateTimestampMs);
    TEST_ASSERT_EQUAL(0, clearCalls);
}

void test_select_miss_clears_voice_sample() {
    scriptedSelectResult = false;

    VoiceSpeedSyncContext ctx;
    ctx.nowMs = 3000;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, selectCalls);
    TEST_ASSERT_EQUAL(3000u, lastSelectNowMs);
    TEST_ASSERT_EQUAL(0, updateCalls);
    TEST_ASSERT_EQUAL(1, clearCalls);
}

void test_missing_update_provider_is_safe_on_hit() {
    VoiceSpeedSyncModule::Providers providers;
    providers.selectSpeedSample = selectSpeedSample;
    providers.clearVoiceSpeedSample = clearVoiceSpeedSample;
    module.begin(providers);

    scriptedSelectResult = true;
    scriptedSelection.speedMph = 42.0f;
    scriptedSelection.timestampMs = 99;

    VoiceSpeedSyncContext ctx;
    ctx.nowMs = 111;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, selectCalls);
    TEST_ASSERT_EQUAL(0, updateCalls);
    TEST_ASSERT_EQUAL(0, clearCalls);
}

void test_missing_clear_provider_is_safe_on_miss() {
    VoiceSpeedSyncModule::Providers providers;
    providers.selectSpeedSample = selectSpeedSample;
    providers.updateVoiceSpeedSample = updateVoiceSpeedSample;
    module.begin(providers);

    scriptedSelectResult = false;

    VoiceSpeedSyncContext ctx;
    ctx.nowMs = 222;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, selectCalls);
    TEST_ASSERT_EQUAL(0, updateCalls);
    TEST_ASSERT_EQUAL(0, clearCalls);
}

void test_empty_providers_is_safe_noop() {
    VoiceSpeedSyncModule::Providers providers;
    module.begin(providers);

    VoiceSpeedSyncContext ctx;
    ctx.nowMs = 333;
    module.process(ctx);

    TEST_ASSERT_EQUAL(0, selectCalls);
    TEST_ASSERT_EQUAL(0, updateCalls);
    TEST_ASSERT_EQUAL(0, clearCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_select_hit_updates_voice_and_does_not_clear);
    RUN_TEST(test_select_miss_clears_voice_sample);
    RUN_TEST(test_missing_update_provider_is_safe_on_hit);
    RUN_TEST(test_missing_clear_provider_is_safe_on_miss);
    RUN_TEST(test_empty_providers_is_safe_noop);
    return UNITY_END();
}

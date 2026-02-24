#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/speed_volume/speed_volume_runtime_module.cpp"

static SpeedVolumeRuntimeModule module;

enum CallId {
    CALL_SPEED_PROCESS = 1,
    CALL_SPEAKER_SYNC = 2,
};

static int callLog[16];
static size_t callLogCount = 0;

static int speedProcessCalls = 0;
static uint32_t lastSpeedProcessNowMs = 0;
static bool scriptedQuietActive = false;
static uint8_t scriptedQuietVolume = 0;

static int speakerSyncCalls = 0;
static bool lastSpeakerQuietNow = false;
static uint8_t lastSpeakerQuietVolume = 0;
static uint8_t lastSpeakerConfiguredVoiceVolume = 0;

static void resetState() {
    callLogCount = 0;
    speedProcessCalls = 0;
    lastSpeedProcessNowMs = 0;
    scriptedQuietActive = false;
    scriptedQuietVolume = 0;
    speakerSyncCalls = 0;
    lastSpeakerQuietNow = false;
    lastSpeakerQuietVolume = 0;
    lastSpeakerConfiguredVoiceVolume = 0;
}

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void runSpeedVolumeProcess(void*, uint32_t nowMs) {
    speedProcessCalls++;
    lastSpeedProcessNowMs = nowMs;
    noteCall(CALL_SPEED_PROCESS);
}

static bool readSpeedQuietActive(void*) {
    return scriptedQuietActive;
}

static uint8_t readSpeedQuietVolume(void*) {
    return scriptedQuietVolume;
}

static void runSpeakerQuietSync(void*, bool quietNow, uint8_t quietVolume, uint8_t configuredVoiceVolume) {
    speakerSyncCalls++;
    lastSpeakerQuietNow = quietNow;
    lastSpeakerQuietVolume = quietVolume;
    lastSpeakerConfiguredVoiceVolume = configuredVoiceVolume;
    noteCall(CALL_SPEAKER_SYNC);
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_process_runs_speed_then_speaker_sync_with_forwarded_values() {
    SpeedVolumeRuntimeModule::Providers providers;
    providers.runSpeedVolumeProcess = runSpeedVolumeProcess;
    providers.readSpeedQuietActive = readSpeedQuietActive;
    providers.readSpeedQuietVolume = readSpeedQuietVolume;
    providers.runSpeakerQuietSync = runSpeakerQuietSync;
    module.begin(providers);

    scriptedQuietActive = true;
    scriptedQuietVolume = 3;
    SpeedVolumeRuntimeContext ctx;
    ctx.nowMs = 2500;
    ctx.configuredVoiceVolume = 8;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, speedProcessCalls);
    TEST_ASSERT_EQUAL(2500u, lastSpeedProcessNowMs);
    TEST_ASSERT_EQUAL(1, speakerSyncCalls);
    TEST_ASSERT_TRUE(lastSpeakerQuietNow);
    TEST_ASSERT_EQUAL_UINT8(3, lastSpeakerQuietVolume);
    TEST_ASSERT_EQUAL_UINT8(8, lastSpeakerConfiguredVoiceVolume);
    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_SPEED_PROCESS, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_SPEAKER_SYNC, callLog[1]);
}

void test_non_quiet_path_still_forwards_to_speaker_sync() {
    SpeedVolumeRuntimeModule::Providers providers;
    providers.runSpeedVolumeProcess = runSpeedVolumeProcess;
    providers.readSpeedQuietActive = readSpeedQuietActive;
    providers.readSpeedQuietVolume = readSpeedQuietVolume;
    providers.runSpeakerQuietSync = runSpeakerQuietSync;
    module.begin(providers);

    scriptedQuietActive = false;
    scriptedQuietVolume = 6;
    SpeedVolumeRuntimeContext ctx;
    ctx.nowMs = 4000;
    ctx.configuredVoiceVolume = 5;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, speedProcessCalls);
    TEST_ASSERT_EQUAL(1, speakerSyncCalls);
    TEST_ASSERT_FALSE(lastSpeakerQuietNow);
    TEST_ASSERT_EQUAL_UINT8(6, lastSpeakerQuietVolume);
    TEST_ASSERT_EQUAL_UINT8(5, lastSpeakerConfiguredVoiceVolume);
}

void test_missing_speaker_provider_skips_sync() {
    SpeedVolumeRuntimeModule::Providers providers;
    providers.runSpeedVolumeProcess = runSpeedVolumeProcess;
    providers.readSpeedQuietActive = readSpeedQuietActive;
    providers.readSpeedQuietVolume = readSpeedQuietVolume;
    module.begin(providers);

    SpeedVolumeRuntimeContext ctx;
    ctx.nowMs = 100;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, speedProcessCalls);
    TEST_ASSERT_EQUAL(0, speakerSyncCalls);
    TEST_ASSERT_EQUAL(1, callLogCount);
    TEST_ASSERT_EQUAL(CALL_SPEED_PROCESS, callLog[0]);
}

void test_missing_quiet_providers_skips_sync() {
    SpeedVolumeRuntimeModule::Providers providers;
    providers.runSpeedVolumeProcess = runSpeedVolumeProcess;
    providers.runSpeakerQuietSync = runSpeakerQuietSync;
    module.begin(providers);

    SpeedVolumeRuntimeContext ctx;
    ctx.nowMs = 900;
    ctx.configuredVoiceVolume = 7;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, speedProcessCalls);
    TEST_ASSERT_EQUAL(0, speakerSyncCalls);
    TEST_ASSERT_EQUAL(1, callLogCount);
    TEST_ASSERT_EQUAL(CALL_SPEED_PROCESS, callLog[0]);
}

void test_empty_providers_is_safe_noop() {
    SpeedVolumeRuntimeModule::Providers providers;
    module.begin(providers);

    SpeedVolumeRuntimeContext ctx;
    ctx.nowMs = 321;
    ctx.configuredVoiceVolume = 9;
    module.process(ctx);

    TEST_ASSERT_EQUAL(0, speedProcessCalls);
    TEST_ASSERT_EQUAL(0, speakerSyncCalls);
    TEST_ASSERT_EQUAL(0, callLogCount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_runs_speed_then_speaker_sync_with_forwarded_values);
    RUN_TEST(test_non_quiet_path_still_forwards_to_speaker_sync);
    RUN_TEST(test_missing_speaker_provider_skips_sync);
    RUN_TEST(test_missing_quiet_providers_skips_sync);
    RUN_TEST(test_empty_providers_is_safe_noop);
    return UNITY_END();
}

#include <unity.h>

#include "../mocks/settings.h"
#include "../../src/modules/voice/voice_module.h"
#include "../../src/modules/voice/voice_module.cpp"  // Pull implementation for UNIT_TEST

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

SettingsManager settingsManager;
static VoiceModule voiceModule;
static V1BLEClient bleClient;

void setUp() {
    settingsManager = SettingsManager();
    bleClient.reset();
    voiceModule = VoiceModule();
    voiceModule.begin(&settingsManager, &bleClient);
}

void tearDown() {
}

void test_speed_sample_valid_within_ttl() {
    voiceModule.updateSpeedSample(42.5f, 1000);

    float speedMph = 0.0f;
    TEST_ASSERT_TRUE(voiceModule.getCurrentSpeedSample(1200, speedMph));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.5f, speedMph);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.5f, voiceModule.getCurrentSpeedMph(1200));
}

void test_speed_sample_expires_after_ttl() {
    voiceModule.updateSpeedSample(63.0f, 1000);

    float speedMph = 0.0f;
    TEST_ASSERT_FALSE(voiceModule.getCurrentSpeedSample(7000, speedMph));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, voiceModule.getCurrentSpeedMph(7000));
}

void test_invalid_sample_preserves_previous_cache() {
    voiceModule.updateSpeedSample(30.0f, 2000);
    voiceModule.updateSpeedSample(-1.0f, 2100);  // ignored

    float speedMph = 0.0f;
    TEST_ASSERT_TRUE(voiceModule.getCurrentSpeedSample(2500, speedMph));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, speedMph);
}

void test_clear_speed_sample_invalidates_cache() {
    voiceModule.updateSpeedSample(48.0f, 1000);
    voiceModule.clearSpeedSample();

    float speedMph = 0.0f;
    TEST_ASSERT_FALSE(voiceModule.getCurrentSpeedSample(1200, speedMph));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, voiceModule.getCurrentSpeedMph(1200));
}

void runAllTests() {
    RUN_TEST(test_speed_sample_valid_within_ttl);
    RUN_TEST(test_speed_sample_expires_after_ttl);
    RUN_TEST(test_invalid_sample_preserves_previous_cache);
    RUN_TEST(test_clear_speed_sample_invalidates_cache);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main(int argc, char** argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif

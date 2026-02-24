#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/speed_volume/speaker_quiet_sync_module.cpp"

static SpeakerQuietSyncModule module;
static int setCalls = 0;
static uint8_t lastSetVolume = 0;

static void resetState() {
    setCalls = 0;
    lastSetVolume = 0;
}

void setUp() {
    module.reset();
    resetState();
}

void tearDown() {}

void test_enter_quiet_scales_volume_and_latches_active() {
    module.process(true, 3, 9, [](uint8_t volume) {
        setCalls++;
        lastSetVolume = volume;
    });

    TEST_ASSERT_TRUE(module.isQuietActive());
    TEST_ASSERT_EQUAL_UINT8(9, module.originalVolume());
    TEST_ASSERT_EQUAL_INT(1, setCalls);
    TEST_ASSERT_EQUAL_UINT8(3, lastSetVolume);
}

void test_enter_quiet_with_zero_quiet_volume_mutes_speaker() {
    module.process(true, 0, 7, [](uint8_t volume) {
        setCalls++;
        lastSetVolume = volume;
    });

    TEST_ASSERT_TRUE(module.isQuietActive());
    TEST_ASSERT_EQUAL_INT(1, setCalls);
    TEST_ASSERT_EQUAL_UINT8(0, lastSetVolume);
}

void test_staying_quiet_does_not_reapply_volume() {
    module.process(true, 2, 8, [](uint8_t volume) {
        setCalls++;
        lastSetVolume = volume;
    });
    TEST_ASSERT_EQUAL_INT(1, setCalls);

    module.process(true, 5, 8, [](uint8_t volume) {
        setCalls++;
        lastSetVolume = volume;
    });

    TEST_ASSERT_TRUE(module.isQuietActive());
    TEST_ASSERT_EQUAL_INT(1, setCalls);
    TEST_ASSERT_EQUAL_UINT8(1, lastSetVolume);
}

void test_exit_quiet_restores_current_configured_volume() {
    module.process(true, 4, 8, [](uint8_t volume) {
        setCalls++;
        lastSetVolume = volume;
    });
    TEST_ASSERT_TRUE(module.isQuietActive());
    TEST_ASSERT_EQUAL_INT(1, setCalls);

    module.process(false, 4, 6, [](uint8_t volume) {
        setCalls++;
        lastSetVolume = volume;
    });

    TEST_ASSERT_FALSE(module.isQuietActive());
    TEST_ASSERT_EQUAL_INT(2, setCalls);
    TEST_ASSERT_EQUAL_UINT8(6, lastSetVolume);
}

void test_non_quiet_inactive_is_noop() {
    module.process(false, 3, 7, [](uint8_t volume) {
        setCalls++;
        lastSetVolume = volume;
    });

    TEST_ASSERT_FALSE(module.isQuietActive());
    TEST_ASSERT_EQUAL_INT(0, setCalls);
}

void test_reenter_quiet_uses_new_configured_baseline() {
    module.process(true, 3, 9, [](uint8_t volume) {
        setCalls++;
        lastSetVolume = volume;
    });
    module.process(false, 3, 7, [](uint8_t volume) {
        setCalls++;
        lastSetVolume = volume;
    });
    TEST_ASSERT_FALSE(module.isQuietActive());
    TEST_ASSERT_EQUAL_INT(2, setCalls);

    module.process(true, 6, 8, [](uint8_t volume) {
        setCalls++;
        lastSetVolume = volume;
    });

    TEST_ASSERT_TRUE(module.isQuietActive());
    TEST_ASSERT_EQUAL_UINT8(8, module.originalVolume());
    TEST_ASSERT_EQUAL_INT(3, setCalls);
    TEST_ASSERT_EQUAL_UINT8(5, lastSetVolume);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_enter_quiet_scales_volume_and_latches_active);
    RUN_TEST(test_enter_quiet_with_zero_quiet_volume_mutes_speaker);
    RUN_TEST(test_staying_quiet_does_not_reapply_volume);
    RUN_TEST(test_exit_quiet_restores_current_configured_volume);
    RUN_TEST(test_non_quiet_inactive_is_noop);
    RUN_TEST(test_reenter_quiet_uses_new_configured_baseline);
    return UNITY_END();
}

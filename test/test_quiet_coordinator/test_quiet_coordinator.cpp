#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/packet_parser.h"
#include "../mocks/modules/volume_fade/volume_fade_module.h"
#include "../mocks/modules/speed_mute/speed_mute_module.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// Perf stubs — quiet_coordinator_templates.h calls these but perf_metrics.h
// is excluded in UNIT_TEST builds.
void perfRecordSpeedVolDrop() {}
void perfRecordSpeedVolRestore() {}
void perfRecordSpeedVolRetry() {}

#include "../../src/modules/voice/voice_module.h"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_templates.h"
#include "../../src/modules/quiet/quiet_coordinator_voice_templates.h"

static V1BLEClient ble;
static PacketParser parser;
static VolumeFadeModule volumeFade;
static SpeedMuteModule speedMute;
static QuietCoordinatorModule module;

static void beginModule() {
    module.begin(&ble, &parser);
}

void setUp() {
    ble.reset();
    parser.reset();
    volumeFade = VolumeFadeModule{};
    speedMute = SpeedMuteModule{};
    mockMillis = 0;
    mockMicros = 0;
    beginModule();
}

void tearDown() {}

void test_send_mute_tracks_desired_state_and_owner() {
    parser.state.muted = false;

    const bool sent = module.sendMute(QuietOwner::TapGesture, true);

    TEST_ASSERT_TRUE(sent);
    TEST_ASSERT_EQUAL(1, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QuietOwner::TapGesture),
                          static_cast<int>(module.getDesiredState().muteOwner));
    TEST_ASSERT_TRUE(module.getDesiredState().mutePending);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QuietOwner::TapGesture),
                          static_cast<int>(module.getPresentationState().activeMuteOwner));
}

static void enableSpeedVol(uint8_t targetVolume) {
    speedMute.begin(true, 25, 3, targetVolume);
    speedMute.state_.muteActive = true;
}

void test_speed_volume_drop_restore_and_zero_presentation() {
    parser.setMainVolume(6);
    parser.setMuteVolume(2);
    enableSpeedVol(0);

    TEST_ASSERT_TRUE(module.processSpeedVolume(1000, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    TEST_ASSERT_TRUE(module.getPresentationState().speedVolZeroActive);

    speedMute.state_.muteActive = false;
    parser.setMainVolume(0);
    TEST_ASSERT_TRUE(module.processSpeedVolume(1200, speedMute, &volumeFade));
    TEST_ASSERT_EQUAL(2, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL(1, volumeFade.setBaselineHintCalls);
    TEST_ASSERT_FALSE(module.getPresentationState().speedVolZeroActive);
}

void test_voice_presentation_suppresses_k_band_and_bypasses_ka_at_vol_zero() {
    VoiceContext ctx;
    ctx.isMuted = true;
    ctx.mainVolume = 0;
    enableSpeedVol(0);

    module.applyVoicePresentation(ctx, &speedMute, true, BAND_K);
    TEST_ASSERT_TRUE(ctx.isSuppressed);
    TEST_ASSERT_TRUE(module.getPresentationState().voiceSuppressed);

    ctx = VoiceContext{};
    ctx.isMuted = true;
    ctx.mainVolume = 0;
    module.applyVoicePresentation(ctx, &speedMute, true, BAND_KA);
    TEST_ASSERT_FALSE(ctx.isSuppressed);
    TEST_ASSERT_FALSE(ctx.isMuted);
    TEST_ASSERT_EQUAL_UINT8(1, ctx.mainVolume);
    TEST_ASSERT_TRUE(module.getPresentationState().voiceAllowVolZeroBypass);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_send_mute_tracks_desired_state_and_owner);
    RUN_TEST(test_speed_volume_drop_restore_and_zero_presentation);
    RUN_TEST(test_voice_presentation_suppresses_k_band_and_bypasses_ka_at_vol_zero);
    return UNITY_END();
}

#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/packet_parser.h"
#include "../mocks/modules/lockout/lockout_orchestration_module.h"
#include "../mocks/modules/volume_fade/volume_fade_module.h"
#include "../mocks/modules/speed_mute/speed_mute_module.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/voice/voice_module.h"
#include "../../src/modules/lockout/lockout_runtime_mute_controller.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_templates.h"
#include "../../src/modules/quiet/quiet_coordinator_voice_templates.h"

static V1BLEClient ble;
static PacketParser parser;
static LockoutOrchestrationModule lockout;
static VolumeFadeModule volumeFade;
static SpeedMuteModule speedMute;
static QuietCoordinatorModule module;

static void beginModule() {
    module.begin(&ble, &parser);
}

void setUp() {
    ble.reset();
    parser.reset();
    lockout = LockoutOrchestrationModule{};
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

void test_process_lockout_mute_sends_once_and_override_unmutes() {
    LockoutEnforcerResult lockRes;
    lockRes.evaluated = true;
    lockRes.shouldMute = true;
    lockRes.mode = static_cast<uint8_t>(LOCKOUT_RUNTIME_ENFORCE);

    GpsLockoutCoreGuardStatus guard;
    guard.enabled = true;
    guard.tripped = false;
    guard.reason = "none";

    parser.state.muted = false;
    module.processLockoutMute(lockRes, guard, true, false, false, 1000);
    TEST_ASSERT_EQUAL(1, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);

    parser.state.muted = true;
    parser.state.activeBands = static_cast<uint8_t>(BAND_KA);
    module.processLockoutMute(lockRes, guard, true, true, true, 1000);
    TEST_ASSERT_EQUAL(3, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QuietOwner::None),
                          static_cast<int>(module.getPresentationState().activeMuteOwner));
}

void test_prequiet_restore_retries_until_v1_confirms() {
    LockoutVolumeCommand command;
    command.type = LockoutVolumeCommandType::PreQuietRestore;
    command.volume = 7;
    command.muteVolume = 2;

    parser.setMainVolume(5);
    module.handleLockoutVolumeCommand(command, 1000, &volumeFade);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL(1, volumeFade.setBaselineHintCalls);

    module.retryPendingPreQuietRestore(1040);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);

    module.retryPendingPreQuietRestore(1080);
    TEST_ASSERT_EQUAL(2, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);

    parser.setMainVolume(7);
    TEST_ASSERT_FALSE(module.retryPendingPreQuietRestore(1160));
}

static void enableSpeedVol(uint8_t targetVolume) {
    speedMute.begin(true, 25, 3, targetVolume);
    speedMute.state_.muteActive = true;
}

void test_speed_volume_drop_restore_and_zero_presentation() {
    parser.setMainVolume(6);
    parser.setMuteVolume(2);
    enableSpeedVol(0);

    TEST_ASSERT_TRUE(module.processSpeedVolume(1000, speedMute, &lockout, &volumeFade));
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    TEST_ASSERT_TRUE(module.getPresentationState().speedVolZeroActive);
    TEST_ASSERT_EQUAL(1, lockout.setVolumeHintCalls);

    speedMute.state_.muteActive = false;
    parser.setMainVolume(0);
    TEST_ASSERT_TRUE(module.processSpeedVolume(1200, speedMute, &lockout, &volumeFade));
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

    module.applyVoicePresentation(ctx, &speedMute, false, true, BAND_K);
    TEST_ASSERT_TRUE(ctx.isSuppressed);
    TEST_ASSERT_TRUE(module.getPresentationState().voiceSuppressed);

    ctx = VoiceContext{};
    ctx.isMuted = true;
    ctx.mainVolume = 0;
    module.applyVoicePresentation(ctx, &speedMute, false, true, BAND_KA);
    TEST_ASSERT_FALSE(ctx.isSuppressed);
    TEST_ASSERT_FALSE(ctx.isMuted);
    TEST_ASSERT_EQUAL_UINT8(1, ctx.mainVolume);
    TEST_ASSERT_TRUE(module.getPresentationState().voiceAllowVolZeroBypass);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_send_mute_tracks_desired_state_and_owner);
    RUN_TEST(test_process_lockout_mute_sends_once_and_override_unmutes);
    RUN_TEST(test_prequiet_restore_retries_until_v1_confirms);
    RUN_TEST(test_speed_volume_drop_restore_and_zero_presentation);
    RUN_TEST(test_voice_presentation_suppresses_k_band_and_bypasses_ka_at_vol_zero);
    return UNITY_END();
}

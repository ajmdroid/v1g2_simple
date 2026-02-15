#include <unity.h>

#include "../mocks/settings.h"
#include "../mocks/ble_client.h"
#include "../mocks/packet_parser.h"
#include "../mocks/modules/voice/voice_module.h"
#include "../mocks/modules/volume_fade/volume_fade_module.h"
#include "../../src/modules/speed_volume/speed_volume_module.h"
#include "../../src/modules/speed_volume/speed_volume_module.cpp"  // pull implementation for UNIT_TEST

#ifndef ARDUINO
SerialClass Serial;
#endif
SettingsManager settingsManager;
static SpeedVolumeModule speedModule;
static V1BLEClient bleClient;
static PacketParser parser;
static VoiceModule voiceModule;
static VolumeFadeModule volumeFadeModule;

static SpeedVolumeContext makeCtx(float speedMph, uint8_t vol, uint8_t muteVol,
                                  bool ble = true, bool fadeTaking = false, unsigned long now = 0) {
    SpeedVolumeContext ctx;
    ctx.bleConnected = ble;
    ctx.fadeTakingControl = fadeTaking;
    ctx.currentVolume = vol;
    ctx.currentMuteVolume = muteVol;
    ctx.speedMph = speedMph;
    ctx.now = now;
    return ctx;
}

void setUp() {
    settingsManager = SettingsManager();
    speedModule = SpeedVolumeModule();
    speedModule.begin(&settingsManager);
    bleClient.reset();
    parser.reset();
    voiceModule.resetMock();
    volumeFadeModule.reset();
}

void test_disabled_returns_none() {
    settingsManager.settings.speedVolumeEnabled = false;
    auto a = speedModule.process(makeCtx(70, 5, 2, true, false, 0));
    TEST_ASSERT_EQUAL(SpeedVolumeAction::Type::NONE, a.type);
    TEST_ASSERT_FALSE(speedModule.isBoostActive());
}

void test_boost_then_restore() {
    settingsManager.settings.speedVolumeEnabled = true;
    settingsManager.settings.speedVolumeThresholdMph = 60;
    settingsManager.settings.speedVolumeBoost = 2;

    // First check below threshold
    auto ctx = makeCtx(55, 5, 2, true, false, 0);
    auto a1 = speedModule.process(ctx);
    TEST_ASSERT_EQUAL(SpeedVolumeAction::Type::NONE, a1.type);
    TEST_ASSERT_FALSE(speedModule.isBoostActive());

    // Above threshold after interval
    ctx = makeCtx(70, 5, 2, true, false, 2500);
    auto a2 = speedModule.process(ctx);
    TEST_ASSERT_EQUAL(SpeedVolumeAction::Type::BOOST, a2.type);
    TEST_ASSERT_EQUAL_UINT8(7, a2.volume);  // 5 + 2
    TEST_ASSERT_TRUE(speedModule.isBoostActive());
    uint8_t orig = speedModule.getOriginalVolume();
    TEST_ASSERT_EQUAL_UINT8(5, orig);

    // Drop below threshold -> restore
    ctx = makeCtx(40, a2.volume, 2, true, false, 5000);
    auto a3 = speedModule.process(ctx);
    TEST_ASSERT_EQUAL(SpeedVolumeAction::Type::RESTORE, a3.type);
    TEST_ASSERT_EQUAL_UINT8(5, a3.volume);
    TEST_ASSERT_FALSE(speedModule.isBoostActive());
}

void test_clamps_to_max_volume() {
    settingsManager.settings.speedVolumeEnabled = true;
    settingsManager.settings.speedVolumeThresholdMph = 50;
    settingsManager.settings.speedVolumeBoost = 4;

    auto ctx = makeCtx(60, 8, 0, true, false, 2500);  // beyond check interval
    auto a = speedModule.process(ctx);
    TEST_ASSERT_EQUAL(SpeedVolumeAction::Type::BOOST, a.type);
    TEST_ASSERT_EQUAL_UINT8(9, a.volume);  // clamp at 9
}

void test_fade_blocks_boost() {
    settingsManager.settings.speedVolumeEnabled = true;
    auto ctx = makeCtx(80, 5, 0, true, true, 0);  // fadeTakingControl true
    auto a = speedModule.process(ctx);
    TEST_ASSERT_EQUAL(SpeedVolumeAction::Type::NONE, a.type);
    TEST_ASSERT_FALSE(speedModule.isBoostActive());
}

void test_proxy_connected_disables_speed_volume_and_restores_once() {
    settingsManager.settings.speedVolumeEnabled = true;
    settingsManager.settings.speedVolumeThresholdMph = 60;
    settingsManager.settings.speedVolumeBoost = 2;
    speedModule.begin(&settingsManager, &bleClient, &parser, &voiceModule, &volumeFadeModule);

    // Build active boost state (5 -> 7) using pure decision path.
    auto boosted = speedModule.process(makeCtx(70, 5, 2, true, false, 2500));
    TEST_ASSERT_EQUAL(SpeedVolumeAction::Type::BOOST, boosted.type);
    TEST_ASSERT_TRUE(speedModule.isBoostActive());

    // Simulate V1 currently at boosted volume, then proxy connects.
    parser.setMainVolume(7);
    parser.setMuteVolume(2);
    bleClient.setProxyConnected(true);

    speedModule.process(3000);
    TEST_ASSERT_FALSE(speedModule.isBoostActive());
    TEST_ASSERT_EQUAL_INT(1, bleClient.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(5, bleClient.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, bleClient.lastMuteVolume);

    // Should not repeat restore while proxy remains connected.
    speedModule.process(5000);
    TEST_ASSERT_EQUAL_INT(1, bleClient.setVolumeCalls);
}

void runAllTests() {
    RUN_TEST(test_disabled_returns_none);
    RUN_TEST(test_boost_then_restore);
    RUN_TEST(test_clamps_to_max_volume);
    RUN_TEST(test_fade_blocks_boost);
    RUN_TEST(test_proxy_connected_disables_speed_volume_and_restores_once);
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

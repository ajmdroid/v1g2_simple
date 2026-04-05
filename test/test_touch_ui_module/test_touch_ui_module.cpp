#include <unity.h>

#include "../mocks/display.h"
#include "../mocks/settings.h"
#include "../mocks/touch_handler.h"
#include "../../src/modules/touch/touch_ui_module.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

namespace {

V1Display display;
TouchHandler touchHandler;
SettingsManager settingsManager;
TouchUiModule touchUiModule;

bool wifiSetupActive = false;
int wifiStartCalls = 0;
int wifiStopCalls = 0;
int manualPairRequests = 0;
bool obdPairSafe = true;
ObdRuntimeStatus obdStatus;

TouchUiModule::Callbacks makeCallbacks() {
    return TouchUiModule::Callbacks{
        .isWifiSetupActive = [](void* /*ctx*/) { return wifiSetupActive; },
        .stopWifiSetup = [](void* /*ctx*/) { ++wifiStopCalls; wifiSetupActive = false; },
        .startWifi = [](void* /*ctx*/) { ++wifiStartCalls; wifiSetupActive = true; },
        .drawWifiIndicator = [](void* /*ctx*/) { display.drawWiFiIndicator(); },
        .restoreDisplay = [](void* /*ctx*/) {},
        .readObdStatus = [](uint32_t, void* /*ctx*/) { return obdStatus; },
        .requestObdManualPairScan = [](uint32_t, void* /*ctx*/) {
            ++manualPairRequests;
            obdStatus.manualScanPending = true;
            return true;
        },
        .isObdPairGestureSafe = [](uint32_t, void* /*ctx*/) { return obdPairSafe; },
    };
}

void resetFixture() {
    display.reset();
    touchHandler.reset();
    settingsManager = SettingsManager();
    settingsManager.settings.brightness = 240;
    settingsManager.settings.voiceVolume = 31;

    wifiSetupActive = false;
    wifiStartCalls = 0;
    wifiStopCalls = 0;
    manualPairRequests = 0;
    obdPairSafe = true;
    obdStatus = ObdRuntimeStatus{};

    touchUiModule = TouchUiModule();
    touchUiModule.begin(&display, &touchHandler, &settingsManager, makeCallbacks());
}

}  // namespace

void audio_set_volume(uint8_t) {}
void play_test_voice() {}
void play_vol0_beep() {}
void play_alert_voice(AlertBand, AlertDirection) {}
void play_frequency_voice(AlertBand, uint16_t, AlertDirection, VoiceAlertMode, bool, uint8_t) {}
void play_direction_only(AlertDirection, uint8_t) {}
void play_threat_escalation(AlertBand, uint16_t, AlertDirection, uint8_t, uint8_t, uint8_t, uint8_t) {}
void play_band_only(AlertBand) {}
void audio_init_sd() {}
void audio_init_buffers() {}
void audio_process_amp_timeout() {}

void setUp() {
    resetFixture();
}

void tearDown() {}

void test_short_press_keeps_existing_settings_mode_behavior() {
    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    TEST_ASSERT_TRUE(touchUiModule.process(350, false));

    TEST_ASSERT_EQUAL_INT(1, display.showSettingsSlidersCalls);
    TEST_ASSERT_EQUAL_INT(0, wifiStartCalls);
    TEST_ASSERT_EQUAL_INT(0, manualPairRequests);
    TEST_ASSERT_EQUAL_INT(0, display.setObdAttentionCalls);
}

void test_four_second_press_keeps_existing_wifi_toggle_behavior() {
    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    // WiFi should fire at 4s while still held
    TEST_ASSERT_FALSE(touchUiModule.process(4000, true));
    TEST_ASSERT_EQUAL_INT(1, wifiStartCalls);
    TEST_ASSERT_EQUAL_INT(1, display.drawWiFiIndicatorCalls);
    TEST_ASSERT_EQUAL_INT(1, display.flushCalls);

    // Release should not re-trigger or enter adjust mode
    TEST_ASSERT_FALSE(touchUiModule.process(4500, false));
    TEST_ASSERT_EQUAL_INT(1, wifiStartCalls);
    TEST_ASSERT_EQUAL_INT(0, manualPairRequests);
    TEST_ASSERT_EQUAL_INT(0, display.showSettingsSlidersCalls);
}

void test_ten_second_press_arms_obd_pair_and_suppresses_wifi_toggle() {
    obdStatus.enabled = true;

    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    TEST_ASSERT_FALSE(touchUiModule.process(3999, true));
    TEST_ASSERT_EQUAL_INT(0, wifiStartCalls);  // not yet at 4s

    // WiFi fires at 4s threshold
    TEST_ASSERT_FALSE(touchUiModule.process(4000, true));
    TEST_ASSERT_EQUAL_INT(1, wifiStartCalls);

    // OBD arm fires at 10s
    TEST_ASSERT_FALSE(touchUiModule.process(10000, true));
    TEST_ASSERT_EQUAL_INT(1, display.setObdAttentionCalls);
    TEST_ASSERT_TRUE(display.lastObdAttention);
    TEST_ASSERT_EQUAL_INT(1, display.drawObdIndicatorCalls);
    TEST_ASSERT_EQUAL_INT(1, display.flushRegionCalls);

    TEST_ASSERT_FALSE(touchUiModule.process(10050, false));

    TEST_ASSERT_EQUAL_INT(1, manualPairRequests);
    TEST_ASSERT_EQUAL_INT(2, display.setObdAttentionCalls);
    TEST_ASSERT_FALSE(display.lastObdAttention);
}

void test_ten_second_press_falls_back_to_wifi_when_obd_pair_not_eligible() {
    obdStatus.enabled = true;
    obdStatus.connected = true;

    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    // WiFi fires at 4s (OBD already connected, pair gesture won't arm)
    TEST_ASSERT_FALSE(touchUiModule.process(4000, true));
    TEST_ASSERT_EQUAL_INT(1, wifiStartCalls);

    TEST_ASSERT_FALSE(touchUiModule.process(10000, true));
    TEST_ASSERT_FALSE(touchUiModule.process(10050, false));

    TEST_ASSERT_EQUAL_INT(0, manualPairRequests);
    TEST_ASSERT_EQUAL_INT(1, wifiStartCalls);  // still just 1 — no double fire
    TEST_ASSERT_EQUAL_INT(0, display.setObdAttentionCalls);
}

void test_obd_pair_arm_clears_when_safety_disappears_before_release() {
    obdStatus.enabled = true;

    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    // WiFi fires at 4s
    TEST_ASSERT_FALSE(touchUiModule.process(4000, true));
    TEST_ASSERT_EQUAL_INT(1, wifiStartCalls);

    TEST_ASSERT_FALSE(touchUiModule.process(10000, true));
    TEST_ASSERT_TRUE(display.lastObdAttention);

    obdPairSafe = false;
    TEST_ASSERT_FALSE(touchUiModule.process(10001, true));
    TEST_ASSERT_FALSE(display.lastObdAttention);

    TEST_ASSERT_FALSE(touchUiModule.process(10050, false));

    TEST_ASSERT_EQUAL_INT(0, manualPairRequests);
    TEST_ASSERT_EQUAL_INT(1, wifiStartCalls);  // already fired at 4s, not again
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_short_press_keeps_existing_settings_mode_behavior);
    RUN_TEST(test_four_second_press_keeps_existing_wifi_toggle_behavior);
    RUN_TEST(test_ten_second_press_arms_obd_pair_and_suppresses_wifi_toggle);
    RUN_TEST(test_ten_second_press_falls_back_to_wifi_when_obd_pair_not_eligible);
    RUN_TEST(test_obd_pair_arm_clears_when_safety_disappears_before_release);

    return UNITY_END();
}

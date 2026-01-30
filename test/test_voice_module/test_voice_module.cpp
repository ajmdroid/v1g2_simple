#include <unity.h>

#include "../mocks/settings.h"
#include "../mocks/ble_client.h"
#include "../mocks/obd_handler.h"
#include "../mocks/gps_handler.h"
#include "../../src/modules/voice/voice_module.h"
#include "../../src/modules/voice/voice_module.cpp"  // Include implementation for UNIT_TEST build

#ifndef ARDUINO
SerialClass Serial;
#endif
SettingsManager settingsManager;
static V1BLEClient bleClient;
static OBDHandler obdHandler;
static GPSHandler gpsHandler;
static VoiceModule voice;

// Helpers
static AlertData makeAlert(Band band, Direction dir, uint32_t freq, uint8_t front = 5, uint8_t rear = 0, bool isPriority = true) {
    AlertData a;
    a.band = band;
    a.direction = dir;
    a.frontStrength = front;
    a.rearStrength = rear;
    a.frequency = freq;
    a.isValid = true;
    a.isPriority = isPriority;
    return a;
}

void setUp() {
    settingsManager = SettingsManager();  // reset settings
    voice = VoiceModule();
    voice.begin(&settingsManager, &bleClient, &obdHandler, &gpsHandler);
}

void test_priority_new_alert() {
    auto priority = makeAlert(BAND_KA, DIR_FRONT, 34700);
    AlertData alerts[1] = {priority};

    VoiceContext ctx;
    ctx.alerts = alerts;
    ctx.alertCount = 1;
    ctx.priority = &priority;
    ctx.isMuted = false;
    ctx.isProxyConnected = false;
    ctx.mainVolume = 5;
    ctx.isInLockout = false;
    ctx.now = 6000;  // satisfies cooldown

    auto action = voice.process(ctx);
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_PRIORITY, action.type);
    TEST_ASSERT_EQUAL(AlertBand::KA, action.band);
    TEST_ASSERT_EQUAL_UINT16(34700, action.freq);
    TEST_ASSERT_EQUAL(AlertDirection::AHEAD, action.dir);
    TEST_ASSERT_EQUAL_UINT8(1, action.bogeyCount);  // with announceBogeyCount default true
}

void test_direction_change_after_priority() {
    // First call to set last announced
    auto priority = makeAlert(BAND_K, DIR_FRONT, 24150);
    AlertData alerts[1] = {priority};
    VoiceContext ctx;
    ctx.alerts = alerts;
    ctx.alertCount = 1;
    ctx.priority = &priority;
    ctx.mainVolume = 5;
    ctx.now = 6000;
    auto first = voice.process(ctx);  // announce priority, sets state
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_PRIORITY, first.type);

    // Direction change
    priority.direction = DIR_REAR;
    alerts[0] = priority;
    ctx.now = 12000;  // cooldown passed
    auto action = voice.process(ctx);
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_DIRECTION, action.type);
    TEST_ASSERT_EQUAL(AlertDirection::BEHIND, action.dir);
    TEST_ASSERT_EQUAL_UINT8(0, action.bogeyCount);  // bogey count unchanged => 0
}

void test_secondary_after_gap() {
    // Initial priority announce
    auto primary = makeAlert(BAND_KA, DIR_FRONT, 34700);
    AlertData alerts[2];
    alerts[0] = primary;
    alerts[1] = makeAlert(BAND_X, DIR_SIDE, 10525, 4, 0, false);

    VoiceContext ctx;
    ctx.alerts = alerts;
    ctx.alertCount = 2;
    ctx.priority = &alerts[0];
    ctx.mainVolume = 5;
    ctx.now = 6000;
    auto first = voice.process(ctx);  // priority announced
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_PRIORITY, first.type);

    // Later, allow secondary
    ctx.now = 8200;  // satisfies stability + post priority gap
    ctx.mainVolume = 5;
    auto action = voice.process(ctx);
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_SECONDARY, action.type);
    TEST_ASSERT_EQUAL(AlertBand::X, action.band);
    TEST_ASSERT_EQUAL_UINT16(10525, action.freq);
}

void runAllTests() {
    RUN_TEST(test_priority_new_alert);
    RUN_TEST(test_direction_change_after_priority);
    RUN_TEST(test_secondary_after_gap);
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
int main(int argc, char **argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif

#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/display.h"
#include "../mocks/ble_client.h"
#include "../mocks/packet_parser.h"
#include "../mocks/touch_handler.h"
#include "../mocks/modules/auto_push/auto_push_module.h"
#include "../mocks/modules/alert_persistence/alert_persistence_module.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

SettingsManager settingsManager;

#include "../../src/modules/touch/tap_gesture_module.cpp"

namespace {

V1Display display;
V1BLEClient bleClient;
PacketParser parser;
TouchHandler touch;
AutoPushModule autoPush;
AlertPersistenceModule alertPersistence;
DisplayMode displayMode = DisplayMode::LIVE;
TapGestureModule module;

AlertData makeAlert() {
    return AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34520, true, true);
}

void setTime(unsigned long nowMs) {
    mockMillis = nowMs;
    mockMicros = nowMs * 1000;
}

void processAt(unsigned long nowMs) {
    setTime(nowMs);
    module.process(nowMs);
}

}  // namespace

void setUp() {
    setTime(0);
    ::settingsManager = SettingsManager{};
    ::settingsManager.settings.activeSlot = 0;
    ::settingsManager.settings.autoPushEnabled = false;
    display.reset();
    bleClient.reset();
    parser.reset();
    touch.reset();
    autoPush.reset();
    alertPersistence.reset();
    displayMode = DisplayMode::LIVE;

    module = TapGestureModule{};
    module.begin(
        &touch,
        &::settingsManager,
        &display,
        &bleClient,
        &parser,
        &autoPush,
        &alertPersistence,
        &displayMode);
}

void tearDown() {}

void test_alert_tap_toggles_mute_immediately() {
    parser.setAlerts({makeAlert()});
    parser.state.muted = false;
    touch.queueTouch(40, 20);

    processAt(200);

    TEST_ASSERT_EQUAL(1, bleClient.setMuteCalls);
    TEST_ASSERT_TRUE(bleClient.lastMuteValue);
    TEST_ASSERT_EQUAL(0, autoPush.queueSlotPushCalls);
    TEST_ASSERT_EQUAL(0, display.drawProfileIndicatorCalls);
}

void test_idle_triple_tap_cycles_slot_and_pushes_when_connected() {
    ::settingsManager.settings.autoPushEnabled = true;
    bleClient.setConnected(true);
    touch.queueTouch(10, 10);
    touch.queueTouch(10, 10);
    touch.queueTouch(10, 10);

    processAt(200);
    TEST_ASSERT_EQUAL(0, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL(0, autoPush.queueSlotPushCalls);

    processAt(400);
    TEST_ASSERT_EQUAL(0, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL(0, autoPush.queueSlotPushCalls);

    processAt(600);

    TEST_ASSERT_EQUAL(1, ::settingsManager.settings.activeSlot);
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, alertPersistence.clearPersistenceCalls);
    TEST_ASSERT_EQUAL(1, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL(1, display.lastProfileIndicatorSlot);
    TEST_ASSERT_EQUAL(1, autoPush.queueSlotPushCalls);
    TEST_ASSERT_EQUAL(1, autoPush.lastQueueSlotPushSlot);
}

void test_idle_profile_cycle_resets_after_tap_window_expires() {
    touch.queueTouch(10, 10);
    touch.queueTouch(10, 10);
    touch.queueTouch(10, 10);

    processAt(200);
    processAt(400);
    processAt(1201);

    TEST_ASSERT_EQUAL(0, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL(0, autoPush.queueSlotPushCalls);
    TEST_ASSERT_EQUAL(0, ::settingsManager.settings.activeSlot);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_alert_tap_toggles_mute_immediately);
    RUN_TEST(test_idle_triple_tap_cycles_slot_and_pushes_when_connected);
    RUN_TEST(test_idle_profile_cycle_resets_after_tap_window_expires);
    return UNITY_END();
}

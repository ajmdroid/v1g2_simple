#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/battery_manager.h"
#include "../mocks/display.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#define private public
#include "../../src/modules/power/power_module.cpp"
#undef private

namespace {

BatteryManager battery;
V1Display display;
SettingsManager testSettings;
PowerModule module;

void setTime(unsigned long nowMs) {
    mockMillis = nowMs;
    mockMicros = nowMs * 1000;
}

void advanceTime(unsigned long deltaMs) {
    setTime(mockMillis + deltaMs);
}

}  // namespace

void setUp() {
    setTime(0);
    battery.reset();
    battery.setOnBattery(true);
    battery.setHasBattery(true);
    battery.setBatteryPercent(60);
    battery.setVoltage(3.8f);

    display.reset();

    testSettings = SettingsManager{};
    testSettings.settings.autoPowerOffMinutes = 10;

    module = PowerModule{};
    module.begin(&battery, &display, &testSettings);
}

void tearDown() {}

void test_critical_battery_shows_warning_before_shutdown() {
    battery.setCritical(true);

    module.process(1000);

    TEST_ASSERT_EQUAL(1, display.showLowBatteryCalls);
    TEST_ASSERT_TRUE(module.lowBatteryWarningShown);
    TEST_ASSERT_FALSE(battery.powerOffCalled);
}

void test_critical_battery_shutdown_occurs_after_grace_period() {
    battery.setCritical(true);

    module.process(1000);
    module.process(6001);

    TEST_ASSERT_TRUE(battery.powerOffCalled);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

void test_critical_battery_recovery_clears_warning_without_shutdown() {
    battery.setCritical(true);
    module.process(1000);

    battery.setCritical(false);
    module.process(3000);
    module.process(7000);

    TEST_ASSERT_FALSE(module.lowBatteryWarningShown);
    TEST_ASSERT_FALSE(battery.powerOffCalled);
}

void test_usb_power_skips_critical_battery_shutdown_path() {
    battery.setOnBattery(false);
    battery.setCritical(true);

    module.process(1000);
    module.process(10000);

    TEST_ASSERT_EQUAL(0, display.showLowBatteryCalls);
    TEST_ASSERT_FALSE(battery.powerOffCalled);
}

void test_v1_data_arms_auto_power_off() {
    TEST_ASSERT_FALSE(module.autoPowerOffArmed);

    module.onV1DataReceived();

    TEST_ASSERT_TRUE(module.autoPowerOffArmed);
}

void test_disconnect_starts_auto_power_timer_when_armed() {
    module.onV1DataReceived();
    setTime(5000);

    module.onV1ConnectionChange(false);

    TEST_ASSERT_EQUAL(5000UL, module.autoPowerOffTimerStart);
}

void test_disconnect_does_not_start_timer_when_not_armed_or_disabled() {
    module.onV1ConnectionChange(false);
    TEST_ASSERT_EQUAL(0UL, module.autoPowerOffTimerStart);

    module.onV1DataReceived();
    testSettings.settings.autoPowerOffMinutes = 0;
    module.onV1ConnectionChange(false);

    TEST_ASSERT_EQUAL(0UL, module.autoPowerOffTimerStart);
}

void test_reconnect_cancels_running_auto_power_timer() {
    module.onV1DataReceived();
    setTime(1000);
    module.onV1ConnectionChange(false);
    TEST_ASSERT_EQUAL(1000UL, module.autoPowerOffTimerStart);

    module.onV1ConnectionChange(true);

    TEST_ASSERT_EQUAL(0UL, module.autoPowerOffTimerStart);
}

void test_auto_power_off_shuts_down_after_timeout() {
    testSettings.settings.autoPowerOffMinutes = 1;
    module.onV1DataReceived();
    setTime(1000);
    module.onV1ConnectionChange(false);

    advanceTime(30000);
    module.process(mockMillis);
    TEST_ASSERT_FALSE(battery.powerOffCalled);

    advanceTime(30001);
    module.process(mockMillis);

    TEST_ASSERT_TRUE(battery.powerOffCalled);
}

void test_process_updates_battery_every_call() {
    module.process(1000);
    module.process(2000);
    module.process(3000);

    TEST_ASSERT_EQUAL(3, battery.updateCalls);
    TEST_ASSERT_EQUAL(3, battery.processPowerButtonCalls);
}

void test_critical_battery_still_wins_while_auto_power_timer_is_running() {
    testSettings.settings.autoPowerOffMinutes = 10;
    module.onV1DataReceived();
    setTime(1000);
    module.onV1ConnectionChange(false);

    battery.setCritical(true);
    advanceTime(1000);
    module.process(mockMillis);
    TEST_ASSERT_EQUAL(1, display.showLowBatteryCalls);
    TEST_ASSERT_FALSE(battery.powerOffCalled);

    advanceTime(5001);
    module.process(mockMillis);

    TEST_ASSERT_TRUE(battery.powerOffCalled);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_critical_battery_shows_warning_before_shutdown);
    RUN_TEST(test_critical_battery_shutdown_occurs_after_grace_period);
    RUN_TEST(test_critical_battery_recovery_clears_warning_without_shutdown);
    RUN_TEST(test_usb_power_skips_critical_battery_shutdown_path);
    RUN_TEST(test_v1_data_arms_auto_power_off);
    RUN_TEST(test_disconnect_starts_auto_power_timer_when_armed);
    RUN_TEST(test_disconnect_does_not_start_timer_when_not_armed_or_disabled);
    RUN_TEST(test_reconnect_cancels_running_auto_power_timer);
    RUN_TEST(test_auto_power_off_shuts_down_after_timeout);
    RUN_TEST(test_process_updates_battery_every_call);
    RUN_TEST(test_critical_battery_still_wins_while_auto_power_timer_is_running);
    return UNITY_END();
}

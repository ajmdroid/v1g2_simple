/**
 * PowerModule Unit Tests
 * 
 * Tests safety-critical shutdown logic:
 * - Critical battery warning and auto-shutdown
 * - Auto power-off timer after V1 disconnect
 * - Timer cancellation on reconnect
 * - V1 data arming mechanism
 */

#include <unity.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <cstdint>

// ============================================================================
// MOCK DEFINITIONS
// ============================================================================

// Mock millis() for time-based testing
static unsigned long mock_millis_value = 0;
static unsigned long millis() { return mock_millis_value; }
static void advance_millis(unsigned long ms) { mock_millis_value += ms; }
static void reset_millis() { mock_millis_value = 0; }

// Mock Serial for logging (no-op)
struct MockSerial {
    void println(const char*) {}
    void printf(const char*, ...) {}
};
static MockSerial Serial;

// Mock BatteryManager
class MockBatteryManager {
public:
    bool onBattery = true;
    bool hasBatteryFlag = true;
    bool criticalFlag = false;
    int voltageMillivolts = 3800;
    int percentage = 60;
    bool powerOffCalled = false;
    int updateCallCount = 0;
    int processPowerButtonCallCount = 0;
    
    void update() { updateCallCount++; }
    void processPowerButton() { processPowerButtonCallCount++; }
    bool isOnBattery() const { return onBattery; }
    bool hasBattery() const { return hasBatteryFlag; }
    bool isCritical() const { return criticalFlag; }
    int getVoltageMillivolts() const { return voltageMillivolts; }
    int getPercentage() const { return percentage; }
    void powerOff() { powerOffCalled = true; }
    
    void reset() {
        onBattery = true;
        hasBatteryFlag = true;
        criticalFlag = false;
        voltageMillivolts = 3800;
        percentage = 60;
        powerOffCalled = false;
        updateCallCount = 0;
        processPowerButtonCallCount = 0;
    }
};

// Mock V1Display
class MockDisplay {
public:
    bool lowBatteryShown = false;
    
    void showLowBattery() { lowBatteryShown = true; }
    
    void reset() { lowBatteryShown = false; }
};

// Mock V1Settings
struct MockSettings {
    uint8_t autoPowerOffMinutes = 10;  // 10 minute default
};

// Mock SettingsManager
class MockSettingsManager {
public:
    MockSettings settings;
    
    const MockSettings& get() const { return settings; }
    
    void reset() {
        settings.autoPowerOffMinutes = 10;
    }
};

// Mock DebugLogger
class MockDebugLogger {};

// ============================================================================
// POWER MODULE IMPLEMENTATION (extracted for testing)
// ============================================================================

class TestPowerModule {
public:
    void begin(MockBatteryManager* batteryMgr,
               MockDisplay* disp,
               MockSettingsManager* settingsMgr,
               MockDebugLogger* dbgLogger) {
        battery = batteryMgr;
        display = disp;
        settings = settingsMgr;
        debugLogger = dbgLogger;
    }

    void onV1DataReceived() {
        if (!autoPowerOffArmed) {
            autoPowerOffArmed = true;
        }
    }

    void onV1ConnectionChange(bool connected) {
        if (!battery || !settings) return;

        if (connected) {
            if (autoPowerOffTimerStart != 0) {
                autoPowerOffTimerStart = 0;  // Cancel timer
            }
            return;
        }

        // On disconnect, start auto power-off timer if armed and configured
        const MockSettings& s = settings->get();
        if (autoPowerOffArmed && s.autoPowerOffMinutes > 0) {
            autoPowerOffTimerStart = millis();
        }
    }

    void process(unsigned long nowMs) {
        if (!battery || !display || !settings) return;

        battery->update();
        battery->processPowerButton();

        // Critical battery handling
        if (battery->isOnBattery() && battery->hasBattery()) {
            if (battery->isCritical()) {
                if (!lowBatteryWarningShown) {
                    display->showLowBattery();
                    lowBatteryWarningShown = true;
                    criticalBatteryTime = nowMs;
                } else if (nowMs - criticalBatteryTime > 5000) {
                    battery->powerOff();
                }
            } else {
                lowBatteryWarningShown = false;
            }
        }

        // Auto power-off timer check
        if (autoPowerOffTimerStart != 0) {
            const MockSettings& s = settings->get();
            unsigned long elapsedMs = nowMs - autoPowerOffTimerStart;
            unsigned long timeoutMs = (unsigned long)s.autoPowerOffMinutes * 60UL * 1000UL;
            if (elapsedMs >= timeoutMs) {
                autoPowerOffTimerStart = 0;
                battery->powerOff();
            }
        }
    }

    // Test accessors
    bool isAutoPowerOffArmed() const { return autoPowerOffArmed; }
    bool isAutoPowerOffTimerRunning() const { return autoPowerOffTimerStart != 0; }
    unsigned long getAutoPowerOffTimerStart() const { return autoPowerOffTimerStart; }
    bool isLowBatteryWarningShown() const { return lowBatteryWarningShown; }
    
    void reset() {
        lowBatteryWarningShown = false;
        criticalBatteryTime = 0;
        autoPowerOffTimerStart = 0;
        autoPowerOffArmed = false;
    }

private:
    MockBatteryManager* battery = nullptr;
    MockDisplay* display = nullptr;
    MockSettingsManager* settings = nullptr;
    MockDebugLogger* debugLogger = nullptr;

    bool lowBatteryWarningShown = false;
    unsigned long criticalBatteryTime = 0;
    unsigned long autoPowerOffTimerStart = 0;
    bool autoPowerOffArmed = false;
};

// ============================================================================
// TEST FIXTURES
// ============================================================================

static MockBatteryManager batteryMgr;
static MockDisplay displayMock;
static MockSettingsManager settingsMgr;
static MockDebugLogger debugLogger;
static TestPowerModule powerModule;

void setUp(void) {
    reset_millis();
    batteryMgr.reset();
    displayMock.reset();
    settingsMgr.reset();
    powerModule.reset();
    powerModule.begin(&batteryMgr, &displayMock, &settingsMgr, &debugLogger);
}

void tearDown(void) {}

// ============================================================================
// CRITICAL BATTERY TESTS
// ============================================================================

void test_critical_battery_shows_warning(void) {
    batteryMgr.criticalFlag = true;
    
    powerModule.process(1000);
    
    TEST_ASSERT_TRUE(displayMock.lowBatteryShown);
    TEST_ASSERT_TRUE(powerModule.isLowBatteryWarningShown());
    TEST_ASSERT_FALSE(batteryMgr.powerOffCalled);  // Not yet - need 5s
}

void test_critical_battery_shutdown_after_5_seconds(void) {
    batteryMgr.criticalFlag = true;
    
    // First process shows warning
    powerModule.process(1000);
    TEST_ASSERT_TRUE(displayMock.lowBatteryShown);
    TEST_ASSERT_FALSE(batteryMgr.powerOffCalled);
    
    // Process after 5 seconds triggers shutdown
    powerModule.process(6001);
    TEST_ASSERT_TRUE(batteryMgr.powerOffCalled);
}

void test_critical_battery_no_shutdown_if_recovers(void) {
    batteryMgr.criticalFlag = true;
    
    // Show warning
    powerModule.process(1000);
    TEST_ASSERT_TRUE(displayMock.lowBatteryShown);
    
    // Battery recovers before 5s
    batteryMgr.criticalFlag = false;
    powerModule.process(3000);
    
    TEST_ASSERT_FALSE(powerModule.isLowBatteryWarningShown());
    
    // Even after 5s total, no shutdown since recovered
    powerModule.process(7000);
    TEST_ASSERT_FALSE(batteryMgr.powerOffCalled);
}

void test_no_critical_handling_on_usb_power(void) {
    batteryMgr.onBattery = false;  // On USB power
    batteryMgr.criticalFlag = true;
    
    powerModule.process(1000);
    
    TEST_ASSERT_FALSE(displayMock.lowBatteryShown);
    
    powerModule.process(10000);
    TEST_ASSERT_FALSE(batteryMgr.powerOffCalled);
}

void test_no_critical_handling_without_battery(void) {
    batteryMgr.hasBatteryFlag = false;
    batteryMgr.criticalFlag = true;
    
    powerModule.process(1000);
    
    TEST_ASSERT_FALSE(displayMock.lowBatteryShown);
}

// ============================================================================
// AUTO POWER-OFF TESTS
// ============================================================================

void test_v1_data_arms_auto_power_off(void) {
    TEST_ASSERT_FALSE(powerModule.isAutoPowerOffArmed());
    
    powerModule.onV1DataReceived();
    
    TEST_ASSERT_TRUE(powerModule.isAutoPowerOffArmed());
}

void test_v1_disconnect_starts_timer_when_armed(void) {
    powerModule.onV1DataReceived();  // Arm
    TEST_ASSERT_FALSE(powerModule.isAutoPowerOffTimerRunning());
    
    advance_millis(5000);
    powerModule.onV1ConnectionChange(false);  // Disconnect
    
    TEST_ASSERT_TRUE(powerModule.isAutoPowerOffTimerRunning());
    TEST_ASSERT_EQUAL(5000, powerModule.getAutoPowerOffTimerStart());
}

void test_v1_disconnect_no_timer_when_not_armed(void) {
    // Don't arm - no V1 data received
    powerModule.onV1ConnectionChange(false);
    
    TEST_ASSERT_FALSE(powerModule.isAutoPowerOffTimerRunning());
}

void test_v1_disconnect_no_timer_when_disabled(void) {
    settingsMgr.settings.autoPowerOffMinutes = 0;  // Disabled
    powerModule.onV1DataReceived();  // Arm
    
    powerModule.onV1ConnectionChange(false);
    
    TEST_ASSERT_FALSE(powerModule.isAutoPowerOffTimerRunning());
}

void test_v1_reconnect_cancels_timer(void) {
    powerModule.onV1DataReceived();
    advance_millis(1000);
    powerModule.onV1ConnectionChange(false);  // Start timer
    TEST_ASSERT_TRUE(powerModule.isAutoPowerOffTimerRunning());
    
    powerModule.onV1ConnectionChange(true);  // Reconnect
    
    TEST_ASSERT_FALSE(powerModule.isAutoPowerOffTimerRunning());
}

void test_auto_power_off_triggers_after_timeout(void) {
    settingsMgr.settings.autoPowerOffMinutes = 1;  // 1 minute
    powerModule.onV1DataReceived();
    
    advance_millis(1000);
    powerModule.onV1ConnectionChange(false);
    
    // Process before timeout - no shutdown
    advance_millis(30000);  // 30 seconds
    powerModule.process(millis());
    TEST_ASSERT_FALSE(batteryMgr.powerOffCalled);
    
    // Process after timeout - shutdown
    advance_millis(30001);  // Total > 60 seconds
    powerModule.process(millis());
    TEST_ASSERT_TRUE(batteryMgr.powerOffCalled);
}

void test_auto_power_off_10_minute_default(void) {
    settingsMgr.settings.autoPowerOffMinutes = 10;
    powerModule.onV1DataReceived();
    
    advance_millis(1000);
    powerModule.onV1ConnectionChange(false);
    unsigned long startTime = millis();
    
    // Process at 9 minutes 59 seconds - no shutdown
    advance_millis(599000);
    powerModule.process(millis());
    TEST_ASSERT_FALSE(batteryMgr.powerOffCalled);
    
    // Process at 10 minutes - shutdown
    advance_millis(2000);  // Now at 10 min 1 sec
    powerModule.process(millis());
    TEST_ASSERT_TRUE(batteryMgr.powerOffCalled);
}

// ============================================================================
// BATTERY UPDATE TESTS
// ============================================================================

void test_process_updates_battery(void) {
    TEST_ASSERT_EQUAL(0, batteryMgr.updateCallCount);
    
    powerModule.process(1000);
    
    TEST_ASSERT_EQUAL(1, batteryMgr.updateCallCount);
    TEST_ASSERT_EQUAL(1, batteryMgr.processPowerButtonCallCount);
}

void test_multiple_processes_update_battery_each_time(void) {
    powerModule.process(1000);
    powerModule.process(2000);
    powerModule.process(3000);
    
    TEST_ASSERT_EQUAL(3, batteryMgr.updateCallCount);
    TEST_ASSERT_EQUAL(3, batteryMgr.processPowerButtonCallCount);
}

// ============================================================================
// EDGE CASES
// ============================================================================

void test_rapid_connect_disconnect_cycles(void) {
    powerModule.onV1DataReceived();
    
    // Rapid cycles shouldn't cause issues
    for (int i = 0; i < 10; i++) {
        advance_millis(100);
        powerModule.onV1ConnectionChange(false);
        TEST_ASSERT_TRUE(powerModule.isAutoPowerOffTimerRunning());
        
        advance_millis(100);
        powerModule.onV1ConnectionChange(true);
        TEST_ASSERT_FALSE(powerModule.isAutoPowerOffTimerRunning());
    }
    
    TEST_ASSERT_FALSE(batteryMgr.powerOffCalled);
}

void test_critical_battery_during_auto_power_off_timer(void) {
    // Start auto power-off timer
    powerModule.onV1DataReceived();
    advance_millis(1000);
    powerModule.onV1ConnectionChange(false);
    
    // Critical battery should take priority
    batteryMgr.criticalFlag = true;
    advance_millis(1000);
    powerModule.process(millis());
    
    TEST_ASSERT_TRUE(displayMock.lowBatteryShown);
    TEST_ASSERT_FALSE(batteryMgr.powerOffCalled);  // Not yet
    
    // Critical battery shutdown happens first (5s vs 10min)
    advance_millis(5001);
    powerModule.process(millis());
    
    TEST_ASSERT_TRUE(batteryMgr.powerOffCalled);
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void run_tests() {
    UNITY_BEGIN();
    
    // Critical battery tests
    RUN_TEST(test_critical_battery_shows_warning);
    RUN_TEST(test_critical_battery_shutdown_after_5_seconds);
    RUN_TEST(test_critical_battery_no_shutdown_if_recovers);
    RUN_TEST(test_no_critical_handling_on_usb_power);
    RUN_TEST(test_no_critical_handling_without_battery);
    
    // Auto power-off tests
    RUN_TEST(test_v1_data_arms_auto_power_off);
    RUN_TEST(test_v1_disconnect_starts_timer_when_armed);
    RUN_TEST(test_v1_disconnect_no_timer_when_not_armed);
    RUN_TEST(test_v1_disconnect_no_timer_when_disabled);
    RUN_TEST(test_v1_reconnect_cancels_timer);
    RUN_TEST(test_auto_power_off_triggers_after_timeout);
    RUN_TEST(test_auto_power_off_10_minute_default);
    
    // Battery update tests
    RUN_TEST(test_process_updates_battery);
    RUN_TEST(test_multiple_processes_update_battery_each_time);
    
    // Edge cases
    RUN_TEST(test_rapid_connect_disconnect_cycles);
    RUN_TEST(test_critical_battery_during_auto_power_off_timer);
    
    UNITY_END();
}

#ifdef ARDUINO
void setup() {
    delay(2000);  // Wait for serial monitor
    run_tests();
}
void loop() {}
#else
int main(int argc, char** argv) {
    run_tests();
    return 0;
}
#endif

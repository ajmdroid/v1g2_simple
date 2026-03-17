/**
 * Mock BatteryManager for native unit tests.
 * Provides test helpers to control battery state.
 */
#ifndef BATTERY_MANAGER_H
#define BATTERY_MANAGER_H

#include <cstdint>

#ifndef BOOT_BUTTON_GPIO
#define BOOT_BUTTON_GPIO 0
#endif

class BatteryManager {
public:
    bool isOnBattery() const { return onBattery_; }
    bool hasBattery() const { return hasBattery_; }
    int getBatteryPercent() const { return percent_; }
    uint8_t getPercentage() const { return static_cast<uint8_t>(percent_); }
    uint16_t getVoltageMillivolts() const { return static_cast<uint16_t>(voltage_ * 1000.0f); }
    float getBatteryVoltage() const { return voltage_; }
    bool isCharging() const { return charging_; }
    bool isLow() const { return percent_ < 20; }
    bool isCritical() const { return criticalOverrideEnabled_ ? criticalOverrideValue_ : percent_ < 5; }
    void update() { ++updateCalls; }
    bool processPowerButton() {
        ++processPowerButtonCalls;
        return processPowerButtonResult;
    }
    void powerOff() {
        ++powerOffCalls;
        powerOffCalled = true;
    }

    // Test helpers
    void setOnBattery(bool b) { onBattery_ = b; }
    void setHasBattery(bool b) { hasBattery_ = b; }
    void setBatteryPercent(int p) {
        percent_ = p;
        criticalOverrideEnabled_ = false;
    }
    void setVoltage(float v) { voltage_ = v; }
    void setCharging(bool c) { charging_ = c; }
    void setCritical(bool critical) {
        criticalOverrideEnabled_ = true;
        criticalOverrideValue_ = critical;
    }
    void reset() {
        onBattery_ = false;
        hasBattery_ = false;
        percent_ = 100;
        voltage_ = 4.2f;
        charging_ = false;
        criticalOverrideEnabled_ = false;
        criticalOverrideValue_ = false;
        updateCalls = 0;
        processPowerButtonCalls = 0;
        processPowerButtonResult = false;
        powerOffCalls = 0;
        powerOffCalled = false;
    }

    int updateCalls = 0;
    int processPowerButtonCalls = 0;
    bool processPowerButtonResult = false;
    int powerOffCalls = 0;
    bool powerOffCalled = false;

private:
    bool onBattery_ = false;
    bool hasBattery_ = false;
    int percent_ = 100;
    float voltage_ = 4.2f;
    bool charging_ = false;
    bool criticalOverrideEnabled_ = false;
    bool criticalOverrideValue_ = false;
};

inline BatteryManager batteryManager;

#endif  // BATTERY_MANAGER_H

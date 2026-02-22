/**
 * Mock BatteryManager for native unit tests.
 * Provides test helpers to control battery state.
 */
#pragma once

#include <cstdint>

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
    bool isCritical() const { return percent_ < 5; }
    void update() {}
    bool processPowerButton() { return false; }

    // Test helpers
    void setOnBattery(bool b) { onBattery_ = b; }
    void setHasBattery(bool b) { hasBattery_ = b; }
    void setBatteryPercent(int p) { percent_ = p; }
    void setVoltage(float v) { voltage_ = v; }
    void setCharging(bool c) { charging_ = c; }

private:
    bool onBattery_ = false;
    bool hasBattery_ = false;
    int percent_ = 100;
    float voltage_ = 4.2f;
    bool charging_ = false;
};

inline BatteryManager batteryManager;

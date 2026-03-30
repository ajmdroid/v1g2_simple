/**
 * Touch Handler for Waveshare ESP32-S3-Touch-LCD-3.49
 *
 * Hardware: AXS15231B display controller with integrated touch
 * Protocol: I2C @ 0x3B on SDA=17 / SCL=18
 *
 * Features:
 * - Single-touch support (hardware limitation)
 * - 200ms debounce to prevent rapid repeat taps
 * - Optional hardware reset support via RST pin
 * - Returns coordinates in display space
 *
 * Usage:
 *   TouchHandler touch;
 *   touch.begin(17, 18, 0x3B, -1);  // SDA, SCL, addr, RST (unused)
 *   int16_t x, y;
 *   if (touch.getTouchPoint(x, y)) {
 *     // Handle touch at (x, y)
 *   }
 */

#pragma once
#ifndef TOUCH_HANDLER_H
#define TOUCH_HANDLER_H

#include <Arduino.h>
#include <Wire.h>

// AXS15231B Touch Controller I2C address and registers
// (integrated into the display controller on Waveshare ESP32-S3-Touch-LCD-3.49)
#define AXS_TOUCH_ADDR      0x3B
#define AXS_REG_STATUS      0x01
#define AXS_REG_XPOS_HIGH   0x03
#define AXS_REG_XPOS_LOW    0x04
#define AXS_REG_YPOS_HIGH   0x05
#define AXS_REG_YPOS_LOW    0x06
#define AXS_REG_CHIP_ID     0xA3

class TouchHandler {
public:
    TouchHandler();

    // Initialize touch controller with I2C
    bool begin(int sda = 17, int scl = 18, uint8_t addr = AXS_TOUCH_ADDR, int rst = -1);

    // Check if screen is touched
    bool isTouched();

    // Get touch coordinates (returns true if valid touch detected)
    bool getTouchPoint(int16_t& x, int16_t& y);

    // Reset the touch controller
    void reset();

private:
    uint8_t i2cAddr;
    int rstPin;
    bool touchActive;
    unsigned long lastTouchTime;
    unsigned long lastReleaseTime;      // When finger was last released
    unsigned long touchDebounceMs;
    unsigned long releaseDebounceMs;    // Time finger must be lifted before new tap

    // I2C stall tracking
    uint32_t i2cStallCount = 0;          // Transactions that returned error
    uint32_t i2cMaxUs = 0;               // Longest I2C transaction observed

public:
    uint32_t getI2cStallCount() const { return i2cStallCount; }
    uint32_t getI2cMaxUs() const { return i2cMaxUs; }
    uint32_t getI2cRecoveryCount() const { return i2cRecoveryCount; }
    void resetI2cStats() {
        i2cStallCount = 0;
        i2cMaxUs = 0;
        i2cRecoveryCount = 0;
        consecutiveI2cFailures = 0;
    }

private:
    static constexpr uint8_t I2C_RECOVERY_THRESHOLD = 3;
    static constexpr unsigned long I2C_RECOVERY_COOLDOWN_MS = 250;
    static constexpr unsigned long I2C_RECOVERY_BACKOFF_MS = 50;
    static constexpr uint8_t I2C_RECOVERY_CLOCK_PULSES = 9;
    static constexpr unsigned int I2C_RECOVERY_PULSE_DELAY_US = 5;
    static constexpr uint32_t I2C_CLOCK_HZ = 400000;
    static constexpr uint16_t I2C_TIMEOUT_MS = 5;

    int sdaPin = 17;
    int sclPin = 18;
    // I2C communication
    void configureWireBus();
    void noteNoTouch(unsigned long now);
    void recordI2cFailure(unsigned long now, uint32_t elapsedUs);
    void recordI2cSuccess();
    void maybeRecoverI2cBus(unsigned long now);
    void recoverI2cBus(unsigned long now);
    bool isI2cPollBackoffActive(unsigned long now) const;
    uint8_t readRegister(uint8_t reg);

    unsigned long lastRecoveryMs = 0;
    unsigned long nextI2cPollAllowedMs = 0;
    uint8_t consecutiveI2cFailures = 0;
    uint32_t i2cRecoveryCount = 0;
};

#endif // TOUCH_HANDLER_H

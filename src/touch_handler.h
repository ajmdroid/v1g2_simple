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
    unsigned long touchDebounceMs;
    
    // I2C communication
    uint8_t readRegister(uint8_t reg);
    void readRegisters(uint8_t reg, uint8_t* buf, size_t len);
};

#endif // TOUCH_HANDLER_H

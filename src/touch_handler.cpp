/**
 * Touch Handler for Waveshare ESP32-S3-Touch-LCD-3.49
 * AXS15231B integrated touch controller
 * Based on Waveshare official example code
 */

#include "touch_handler.h"

// AXS15231B touch read command sequence
static const uint8_t AXS_TOUCH_READ_CMD[] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};

TouchHandler::TouchHandler() 
    : i2cAddr(AXS_TOUCH_ADDR)
    , rstPin(-1)
    , touchActive(false)
    , lastTouchTime(0)
    , touchDebounceMs(200)  // 200ms debounce for touch detection
{
}

bool TouchHandler::begin(int sda, int scl, uint8_t addr, int rst) {
    i2cAddr = addr;
    rstPin = rst;
    
    Serial.printf("[Touch] Initializing AXS15231B touch on I2C SDA=%d SCL=%d addr=0x%02X\n", sda, scl, addr);
    
    // Initialize I2C with specified pins
    Wire.begin(sda, scl);
    Wire.setClock(400000);  // 400kHz I2C speed
    
    delay(100);  // Give touch controller time to initialize
    
    // Reset the touch controller if reset pin is available
    if (rstPin >= 0) {
        reset();
    }
    
    // Try to communicate with touch controller
    Serial.println("[Touch] Scanning for device...");
    Wire.beginTransmission(i2cAddr);
    uint8_t error = Wire.endTransmission();
    
    if (error == 0) {
        Serial.printf("[Touch] Device found at 0x%02X\n", i2cAddr);
        
        // Try to read status register
        uint8_t status = readRegister(AXS_REG_STATUS);
        Serial.printf("[Touch] Status register: 0x%02X\n", status);
        
        Serial.printf("[Touch] Controller initialized successfully\n");
        return true;
    } else {
        Serial.printf("[Touch] ERROR: Device not found at 0x%02X (error=%d)\n", i2cAddr, error);
        return false;
    }
}

void TouchHandler::reset() {
    if (rstPin >= 0) {
        pinMode(rstPin, OUTPUT);
        Serial.printf("[Touch] Reset: Setting GPIO%d LOW\n", rstPin);
        digitalWrite(rstPin, LOW);
        delay(30);
        Serial.printf("[Touch] Reset: Setting GPIO%d HIGH\n", rstPin);
        digitalWrite(rstPin, HIGH);
        delay(50);
        Serial.println("[Touch] Reset complete");
    } else {
        Serial.println("[Touch] Reset pin not configured");
    }
}

bool TouchHandler::isTouched() {
    int16_t x, y;
    return getTouchPoint(x, y);
}

bool TouchHandler::getTouchPoint(int16_t& x, int16_t& y) {
    // Check for debounce (handles millis() rollover at 49 days)
    unsigned long now = millis();
    if ((long)(now - lastTouchTime) < (long)touchDebounceMs) {
        return false;  // Still in debounce period
    }
    
    // AXS15231B requires special command sequence to read touch data
    // Send command: {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0}
    // Then read 32 bytes of response
    
    Wire.beginTransmission(i2cAddr);
    Wire.write(AXS_TOUCH_READ_CMD, sizeof(AXS_TOUCH_READ_CMD));
    uint8_t err = Wire.endTransmission(false);  // Keep connection open for read
    
    if (err != 0) {
        return false;
    }
    
    // Read 32 bytes of touch data
    uint8_t buff[32] = {0};
    Wire.requestFrom(i2cAddr, (uint8_t)32);
    for (int i = 0; i < 32 && Wire.available(); i++) {
        buff[i] = Wire.read();
    }
    
    // Parse touch data from AXS15231B response
    // buff[0] = gesture (ignored)
    // buff[1] = number of touch points (1-4 = valid touch)
    // buff[2] = X high nibble (bits 3-0)
    // buff[3] = X low byte
    // buff[4] = Y high nibble (bits 3-0)  
    // buff[5] = Y low byte
    
    uint8_t numPoints = buff[1];
    
    if (numPoints == 0 || numPoints > 4) {
        touchActive = false;
        return false;
    }
    
    // Extract coordinates
    x = ((buff[2] & 0x0F) << 8) | buff[3];
    y = ((buff[4] & 0x0F) << 8) | buff[5];
    
    // Detect new touch (rising edge)
    if (!touchActive) {
        touchActive = true;
        lastTouchTime = now;
        Serial.printf("[Touch] *** TAP DETECTED at (%d, %d) ***\n", x, y);
        return true;  // New touch event
    }
    
    return false;  // Touch held, not a new tap
}

uint8_t TouchHandler::readRegister(uint8_t reg) {
    Wire.beginTransmission(i2cAddr);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(false);  // Send restart
    
    if (err != 0) {
        Serial.printf("[Touch] I2C error writing reg 0x%02X: %d\n", reg, err);
        return 0;
    }
    
    Wire.requestFrom(i2cAddr, (uint8_t)1);
    if (Wire.available()) {
        uint8_t val = Wire.read();
        return val;
    }
    return 0;
}

void TouchHandler::readRegisters(uint8_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(i2cAddr);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(false);  // Send restart
    
    if (err != 0) {
        Serial.printf("[Touch] I2C error writing start reg 0x%02X: %d\n", reg, err);
        memset(buf, 0, len);
        return;
    }
    
    Wire.requestFrom(i2cAddr, (uint8_t)len);
    size_t bytesRead = 0;
    for (size_t i = 0; i < len && Wire.available(); i++) {
        buf[i] = Wire.read();
        bytesRead++;
    }
    
    if (bytesRead != len) {
        Serial.printf("[Touch] Incomplete read: got %d/%d bytes from reg 0x%02X\n", bytesRead, len, reg);
    }
}

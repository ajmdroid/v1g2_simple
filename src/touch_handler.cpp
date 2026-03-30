/**
 * Touch Handler for Waveshare ESP32-S3-Touch-LCD-3.49
 * AXS15231B integrated touch controller
 * Based on Waveshare official example code
 */

#include "touch_handler.h"

#define TOUCH_LOGF(...) do { } while(0)

// AXS15231B touch read command sequence
static const uint8_t AXS_TOUCH_READ_CMD[] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};

TouchHandler::TouchHandler()
    : i2cAddr(AXS_TOUCH_ADDR)
    , rstPin(-1)
    , touchActive(false)
    , lastTouchTime(0)
    , lastReleaseTime(0)
    , touchDebounceMs(200)  // 200ms debounce for touch detection
    , releaseDebounceMs(100) // 100ms of no-touch required before new tap can register
{
}

bool TouchHandler::begin(int sda, int scl, uint8_t addr, int rst) {
    sdaPin = sda;
    sclPin = scl;
    i2cAddr = addr;
    rstPin = rst;
    nextI2cPollAllowedMs = 0;
    lastRecoveryMs = 0;
    consecutiveI2cFailures = 0;

    TOUCH_LOGF("[Touch] Initializing AXS15231B touch on I2C SDA=%d SCL=%d addr=0x%02X\n", sda, scl, addr);

    // Initialize I2C with specified pins
    configureWireBus();

    delay(30);   // Conservative I2C/touch controller settle

    // Reset the touch controller if reset pin is available
    if (rstPin >= 0) {
        reset();
    }

    // Try to communicate with touch controller
    Wire.beginTransmission(i2cAddr);
    uint8_t error = Wire.endTransmission();

    if (error == 0) {
        TOUCH_LOGF("[Touch] Device found at 0x%02X\n", i2cAddr);

        // Try to read status register
        uint8_t status = readRegister(AXS_REG_STATUS);
        TOUCH_LOGF("[Touch] Status register: 0x%02X\n", status);

        return true;
    } else {
        Serial.printf("[Touch] ERROR: Device not found at 0x%02X (error=%d)\n", i2cAddr, error);
        return false;
    }
}

void TouchHandler::reset() {
    if (rstPin >= 0) {
        pinMode(rstPin, OUTPUT);
        TOUCH_LOGF("[Touch] Reset: Setting GPIO%d LOW\n", rstPin);
        digitalWrite(rstPin, LOW);
        delay(30);
        TOUCH_LOGF("[Touch] Reset: Setting GPIO%d HIGH\n", rstPin);
        digitalWrite(rstPin, HIGH);
        delay(50);
        TOUCH_LOGF("[Touch] Reset complete\n");
    }
}

bool TouchHandler::isTouched() {
    int16_t x, y;
    return getTouchPoint(x, y);
}

void TouchHandler::configureWireBus() {
    Wire.begin(sdaPin, sclPin);
    Wire.setClock(I2C_CLOCK_HZ);
    Wire.setTimeOut(I2C_TIMEOUT_MS);
}

void TouchHandler::noteNoTouch(unsigned long now) {
    if (touchActive) {
        lastReleaseTime = now;
        touchActive = false;
    }
}

void TouchHandler::recordI2cFailure(unsigned long now, uint32_t elapsedUs) {
    if (elapsedUs > i2cMaxUs) {
        i2cMaxUs = elapsedUs;
    }
    ++i2cStallCount;
    if (consecutiveI2cFailures < UINT8_MAX) {
        ++consecutiveI2cFailures;
    }
    noteNoTouch(now);
    maybeRecoverI2cBus(now);
}

void TouchHandler::recordI2cSuccess() {
    consecutiveI2cFailures = 0;
}

bool TouchHandler::isI2cPollBackoffActive(unsigned long now) const {
    return static_cast<long>(now - nextI2cPollAllowedMs) < 0;
}

void TouchHandler::maybeRecoverI2cBus(unsigned long now) {
    if (consecutiveI2cFailures < I2C_RECOVERY_THRESHOLD) {
        return;
    }

    if (i2cRecoveryCount != 0 &&
        (now - lastRecoveryMs) < I2C_RECOVERY_COOLDOWN_MS) {
        return;
    }

    recoverI2cBus(now);
}

void TouchHandler::recoverI2cBus(unsigned long now) {
    const uint8_t failuresBeforeRecovery = consecutiveI2cFailures;
    ++i2cRecoveryCount;
    lastRecoveryMs = now;
    nextI2cPollAllowedMs = now + I2C_RECOVERY_BACKOFF_MS;
    consecutiveI2cFailures = 0;

    Wire.end();

    pinMode(sdaPin, OUTPUT_OPEN_DRAIN);
    pinMode(sclPin, OUTPUT_OPEN_DRAIN);
    digitalWrite(sdaPin, HIGH);
    digitalWrite(sclPin, HIGH);
    delayMicroseconds(I2C_RECOVERY_PULSE_DELAY_US);

    bool sdaReleased = (digitalRead(sdaPin) != LOW);
    if (!sdaReleased) {
        for (uint8_t pulse = 0; pulse < I2C_RECOVERY_CLOCK_PULSES; ++pulse) {
            digitalWrite(sclPin, LOW);
            delayMicroseconds(I2C_RECOVERY_PULSE_DELAY_US);
            digitalWrite(sclPin, HIGH);
            delayMicroseconds(I2C_RECOVERY_PULSE_DELAY_US);
            if (digitalRead(sdaPin) != LOW) {
                sdaReleased = true;
                break;
            }
        }
    }

    digitalWrite(sclPin, HIGH);
    delayMicroseconds(I2C_RECOVERY_PULSE_DELAY_US);
    digitalWrite(sdaPin, LOW);
    delayMicroseconds(I2C_RECOVERY_PULSE_DELAY_US);
    digitalWrite(sdaPin, HIGH);
    delayMicroseconds(I2C_RECOVERY_PULSE_DELAY_US);

    pinMode(sdaPin, INPUT_PULLUP);
    pinMode(sclPin, INPUT_PULLUP);

    configureWireBus();

    Serial.printf("[Touch] I2C recovery #%lu after %u consecutive failures (%s)\n",
                  static_cast<unsigned long>(i2cRecoveryCount),
                  static_cast<unsigned>(failuresBeforeRecovery),
                  sdaReleased ? "sda_released" : "sda_still_low");
}

bool TouchHandler::getTouchPoint(int16_t& x, int16_t& y) {
    unsigned long now = millis();
    if (isI2cPollBackoffActive(now)) {
        noteNoTouch(now);
        return false;
    }

    // AXS15231B requires special command sequence to read touch data
    // Send command: {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0}
    // Then read 32 bytes of response

    Wire.beginTransmission(i2cAddr);
    Wire.write(AXS_TOUCH_READ_CMD, sizeof(AXS_TOUCH_READ_CMD));
    uint32_t i2cStart = micros();
    uint8_t err = Wire.endTransmission(false);  // Keep connection open for read

    if (err != 0) {
        recordI2cFailure(now, micros() - i2cStart);
        return false;
    }

    // Read 32 bytes of touch data
    uint8_t buff[32] = {0};
    const size_t bytesRead = Wire.requestFrom(i2cAddr, static_cast<uint8_t>(32));
    uint32_t i2cElapsed = micros() - i2cStart;
    if (bytesRead != 32) {
        while (Wire.available()) {
            (void)Wire.read();
        }
        recordI2cFailure(now, i2cElapsed);
        return false;
    }

    if (i2cElapsed > i2cMaxUs) i2cMaxUs = i2cElapsed;
    for (int i = 0; i < 32; i++) {
        if (!Wire.available()) {
            recordI2cFailure(now, micros() - i2cStart);
            return false;
        }
        buff[i] = Wire.read();
    }
    recordI2cSuccess();

    // Parse touch data from AXS15231B response
    // buff[0] = gesture (ignored)
    // buff[1] = number of touch points (1-4 = valid touch)
    // buff[2] = X high nibble (bits 3-0)
    // buff[3] = X low byte
    // buff[4] = Y high nibble (bits 3-0)
    // buff[5] = Y low byte

    uint8_t numPoints = buff[1];

    if (numPoints == 0 || numPoints > 4) {
        // No touch - track when finger was released
        noteNoTouch(now);
        return false;
    }

    // Extract coordinates
    x = ((buff[2] & 0x0F) << 8) | buff[3];
    y = ((buff[4] & 0x0F) << 8) | buff[5];

    // Check if we're still within debounce period from last tap
    if ((long)(now - lastTouchTime) < (long)touchDebounceMs) {
        touchActive = true;  // Keep tracking that finger is down
        return false;  // Still in debounce period
    }

    // Detect new touch (rising edge) - require finger to have been lifted
    // for at least releaseDebounceMs to prevent false taps from noisy readings
    if (!touchActive) {
        // Check if finger was released long enough for this to be a real new tap
        if ((long)(now - lastReleaseTime) >= (long)releaseDebounceMs) {
            touchActive = true;
            lastTouchTime = now;
            TOUCH_LOGF("[Touch] TAP at (%d, %d)\n", x, y);
            return true;  // New touch event
        }
    }

    touchActive = true;  // Finger is down
    return false;  // Touch held, not a new tap
}

uint8_t TouchHandler::readRegister(uint8_t reg) {
    Wire.beginTransmission(i2cAddr);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(false);  // Send restart

    if (err != 0) {
        TOUCH_LOGF("[Touch] I2C error writing reg 0x%02X: %d\n", reg, err);
        return 0;
    }

    Wire.requestFrom(i2cAddr, (uint8_t)1);
    if (Wire.available()) {
        uint8_t val = Wire.read();
        return val;
    }
    return 0;
}

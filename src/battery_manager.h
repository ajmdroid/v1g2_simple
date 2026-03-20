/**
 * Battery Manager for Waveshare ESP32-S3-Touch-LCD-3.49
 * 
 * Handles:
 * - Battery voltage monitoring via ADC
 * - Power control via TCA9554 I/O expander
 * - Power button handling for battery power on/off
 */

#ifndef BATTERY_MANAGER_H
#define BATTERY_MANAGER_H

#include <Arduino.h>
#include "../include/battery_math.h"
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Hardware Pins
#define BATTERY_ADC_CHANNEL     ADC1_CHANNEL_3  // GPIO4
#define BATTERY_ADC_GPIO        4
#define BOOT_BUTTON_GPIO        0               // BOOT button for brightness adjustment
#define PWR_BUTTON_GPIO         16              // Also battery presence detection
#define TCA9554_SDA_GPIO        47
#define TCA9554_SCL_GPIO        48
#define TCA9554_I2C_ADDR        0x20            // ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000
#define TCA9554_PWR_LATCH_PIN   6               // Pin 6 controls battery power latch

// TCA9554 Registers
#define TCA9554_OUTPUT_PORT     0x01
#define TCA9554_CONFIG_PORT     0x03

// Battery voltage thresholds (mV)
#define BATTERY_FULL_MV         battery_math::kFullMv
#define BATTERY_EMPTY_MV        battery_math::kEmptyMv
#define BATTERY_WARNING_MV      battery_math::kWarningMv
#define BATTERY_CRITICAL_MV     battery_math::kCriticalMv

class BatteryManager {
public:
    BatteryManager();
    
    // Initialize the battery manager (call in setup)
    bool begin();
    
    // Check if running on battery power
    bool isOnBattery() const;
    
    // Check if a battery is present (detects battery even when on USB power)
    bool hasBattery() const;
    
    // Update cached battery readings (call in loop, voltage updates every 30s)
    void update();
    
    // Get cached battery voltage in millivolts (updated every 30s)
    uint16_t getVoltageMillivolts() const;
    
    // Get cached battery percentage (0-100, updated every 30s)
    uint8_t getPercentage() const;
    
    // Check if battery is low (uses cached values)
    bool isLow() const;
    
    // Check if battery is critically low (should shutdown soon)
    bool isCritical() const;
    
    // Keep system powered on (call early in setup when on battery)
    bool latchPowerOn();
    
    // Execute the final hardware-only power-off tail after shutdown prep completes
    bool powerOff();
    
    // Check if power button is pressed
    bool isPowerButtonPressed();
    
    // Process power button (call in loop) - returns true if should power off
    bool processPowerButton();
    
private:
    bool initialized;
    bool onBattery;
    uint16_t lastVoltage;
    unsigned long lastButtonPress;
    unsigned long buttonPressStart;
    bool buttonWasPressed;
    
    // Cached battery state (updated every 30s)
    uint16_t cachedVoltage;
    uint8_t cachedPercent;
    unsigned long lastUpdateMs;
    
    // Debug simulation
    uint16_t simulatedVoltage;
    
    bool initADC();
    bool initTCA9554();
    bool setTCA9554Pin(uint8_t pin, bool high);
    bool setTCA9554PinWithBudget(uint8_t pin, bool high, TickType_t timeoutTicks, int maxRetries);
    uint16_t readADCMillivolts();
};

extern BatteryManager batteryManager;

// Shared I2C bus for TCA9554 (also used by ES8311 codec)
extern TwoWire tca9554Wire;
extern SemaphoreHandle_t tca9554WireMutex;

#endif // BATTERY_MANAGER_H

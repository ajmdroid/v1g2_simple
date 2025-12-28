/**
 * Battery Manager for Waveshare ESP32-S3-Touch-LCD-3.49
 */

#include "battery_manager.h"
#include <Wire.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <driver/gpio.h>

// Only compile for Waveshare 3.49 board
#if defined(DISPLAY_WAVESHARE_349)

BatteryManager batteryManager;

// ADC handles
static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;

// I2C for TCA9554 (separate from touch I2C)
static TwoWire tca9554Wire(1);  // Use I2C port 1

BatteryManager::BatteryManager() 
    : initialized(false)
    , onBattery(false)
    , lastVoltage(0)
    , lastButtonPress(0)
    , buttonPressStart(0)
    , buttonWasPressed(false)
{
}

bool BatteryManager::begin() {
    Serial.println("[Battery] Initializing battery manager...");
    
    // Check if we're on battery power (GPIO16 is HIGH when on battery)
    pinMode(PWR_BUTTON_GPIO, INPUT_PULLUP);
    onBattery = digitalRead(PWR_BUTTON_GPIO) == HIGH;
    Serial.printf("[Battery] Running on %s power\n", onBattery ? "BATTERY" : "USB");
    
    // Initialize ADC for battery voltage reading
    if (!initADC()) {
        Serial.println("[Battery] WARNING: ADC init failed, voltage monitoring disabled");
    }
    
    // Initialize TCA9554 for power control
    if (!initTCA9554()) {
        Serial.println("[Battery] WARNING: TCA9554 init failed, power control disabled");
    } else if (onBattery) {
        // If on battery, latch power on immediately
        latchPowerOn();
    }
    
    initialized = true;
    Serial.println("[Battery] Battery manager initialized");
    return true;
}

bool BatteryManager::initADC() {
    // Create calibration handle
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle);
    if (ret != ESP_OK) {
        Serial.printf("[Battery] ADC calibration init failed: %d\n", ret);
        return false;
    }
    
    // Create oneshot unit
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    
    ret = adc_oneshot_new_unit(&init_config, &adc1_handle);
    if (ret != ESP_OK) {
        Serial.printf("[Battery] ADC unit init failed: %d\n", ret);
        return false;
    }
    
    // Configure channel
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    
    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &chan_config);
    if (ret != ESP_OK) {
        Serial.printf("[Battery] ADC channel config failed: %d\n", ret);
        return false;
    }
    
    Serial.println("[Battery] ADC initialized for battery monitoring");
    return true;
}

bool BatteryManager::initTCA9554() {
    // Initialize I2C for TCA9554 on port 1 (separate from touch)
    tca9554Wire.begin(TCA9554_SDA_GPIO, TCA9554_SCL_GPIO, 100000);
    
    // Check if TCA9554 responds
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    uint8_t error = tca9554Wire.endTransmission();
    
    if (error != 0) {
        Serial.printf("[Battery] TCA9554 not found at 0x%02X (error: %d)\n", TCA9554_I2C_ADDR, error);
        return false;
    }
    
    // Configure pin 6 as output (set bit 6 to 0 in config register)
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_CONFIG_PORT);
    tca9554Wire.write(0xBF);  // All inputs except pin 6 (bit 6 = 0 = output)
    error = tca9554Wire.endTransmission();
    
    if (error != 0) {
        Serial.printf("[Battery] TCA9554 config failed: %d\n", error);
        return false;
    }
    
    Serial.println("[Battery] TCA9554 initialized for power control");
    return true;
}

bool BatteryManager::setTCA9554Pin(uint8_t pin, bool high) {
    // Read current output state
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_OUTPUT_PORT);
    tca9554Wire.endTransmission(false);
    tca9554Wire.requestFrom(TCA9554_I2C_ADDR, (uint8_t)1);
    
    if (tca9554Wire.available() < 1) {
        Serial.println("[Battery] Failed to read TCA9554 output port");
        return false;
    }
    
    uint8_t current = tca9554Wire.read();
    
    // Modify the bit
    if (high) {
        current |= (1 << pin);
    } else {
        current &= ~(1 << pin);
    }
    
    // Write back
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_OUTPUT_PORT);
    tca9554Wire.write(current);
    uint8_t error = tca9554Wire.endTransmission();
    
    if (error != 0) {
        Serial.printf("[Battery] Failed to set TCA9554 pin %d: %d\n", pin, error);
        return false;
    }
    
    return true;
}

uint16_t BatteryManager::readADCMillivolts() {
    if (!adc1_handle) return 0;
    
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &raw);
    if (ret != ESP_OK) {
        return lastVoltage;  // Return last known value on error
    }
    
    int voltage_mv = 0;
    if (adc_cali_handle) {
        adc_cali_raw_to_voltage(adc_cali_handle, raw, &voltage_mv);
    } else {
        // Fallback calculation without calibration
        voltage_mv = (raw * 3300) / 4096;
    }
    
    // Apply voltage divider factor (3:1 ratio on the board)
    lastVoltage = voltage_mv * 3;
    return lastVoltage;
}

bool BatteryManager::isOnBattery() const {
    return onBattery;
}

uint16_t BatteryManager::getVoltageMillivolts() {
    return readADCMillivolts();
}

uint8_t BatteryManager::getPercentage() {
    uint16_t voltage = getVoltageMillivolts();
    
    if (voltage >= BATTERY_FULL_MV) return 100;
    if (voltage <= BATTERY_EMPTY_MV) return 0;
    
    // Linear interpolation
    return (uint8_t)((voltage - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}

bool BatteryManager::isLow() {
    return getVoltageMillivolts() < BATTERY_WARNING_MV;
}

bool BatteryManager::latchPowerOn() {
    Serial.println("[Battery] Latching power ON");
    return setTCA9554Pin(TCA9554_PWR_LATCH_PIN, true);
}

bool BatteryManager::powerOff() {
    if (!onBattery) {
        Serial.println("[Battery] Cannot power off - not on battery");
        return false;
    }
    
    Serial.println("[Battery] Powering OFF...");
    delay(100);  // Let serial flush
    return setTCA9554Pin(TCA9554_PWR_LATCH_PIN, false);
}

bool BatteryManager::isPowerButtonPressed() {
    // PWR button is on GPIO16, active LOW
    return digitalRead(PWR_BUTTON_GPIO) == LOW;
}

bool BatteryManager::processPowerButton() {
    if (!onBattery) return false;  // Only process when on battery
    
    bool pressed = isPowerButtonPressed();
    unsigned long now = millis();
    
    if (pressed && !buttonWasPressed) {
        // Button just pressed
        buttonPressStart = now;
        buttonWasPressed = true;
    } else if (pressed && buttonWasPressed) {
        // Button held - check for long press (2 seconds)
        if (now - buttonPressStart >= 2000) {
            Serial.println("[Battery] Long press detected - powering off");
            powerOff();
            return true;
        }
    } else if (!pressed && buttonWasPressed) {
        // Button released
        buttonWasPressed = false;
    }
    
    return false;
}

String BatteryManager::getStatusString() {
    if (!onBattery) {
        return "USB";
    }
    
    uint8_t pct = getPercentage();
    uint16_t mv = lastVoltage;
    
    char buf[32];
    snprintf(buf, sizeof(buf), "BAT %d%% (%d.%02dV)", pct, mv / 1000, (mv % 1000) / 10);
    return String(buf);
}

#else
// Stub implementation for non-Waveshare boards
BatteryManager batteryManager;

BatteryManager::BatteryManager() : initialized(false), onBattery(false), lastVoltage(0) {}
bool BatteryManager::begin() { return false; }
bool BatteryManager::isOnBattery() const { return false; }
uint16_t BatteryManager::getVoltageMillivolts() { return 0; }
uint8_t BatteryManager::getPercentage() { return 0; }
bool BatteryManager::isLow() { return false; }
bool BatteryManager::latchPowerOn() { return false; }
bool BatteryManager::powerOff() { return false; }
bool BatteryManager::isPowerButtonPressed() { return false; }
bool BatteryManager::processPowerButton() { return false; }
String BatteryManager::getStatusString() { return "N/A"; }
bool BatteryManager::initADC() { return false; }
bool BatteryManager::initTCA9554() { return false; }
bool BatteryManager::setTCA9554Pin(uint8_t pin, bool high) { return false; }
uint16_t BatteryManager::readADCMillivolts() { return 0; }

#endif // DISPLAY_WAVESHARE_349

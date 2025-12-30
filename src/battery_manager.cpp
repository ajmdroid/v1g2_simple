/**
 * Battery Manager for Waveshare ESP32-S3-Touch-LCD-3.49
 */

#include "battery_manager.h"
#include "display.h"
#include "settings.h"
#include <Wire.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <driver/gpio.h>

// Only compile for Waveshare 3.49 board
#if defined(DISPLAY_WAVESHARE_349)

// External references for graceful shutdown
extern V1Display display;
extern SettingsManager settingsManager;

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
    , cachedVoltage(0)
    , cachedPercent(0)
    , lastUpdateMs(0)
    , simulatedVoltage(0)
{
}

bool BatteryManager::begin() {
    Serial.println("[Battery] Initializing battery manager...");
    
    // CRITICAL: Initialize TCA9554 and latch power FIRST, before anything else
    // This MUST happen immediately on ANY boot to handle button-press boot scenarios
    // During button boot, GPIO16 is LOW (button pressed) but we still need the latch
    Serial.println("[Battery] Initializing power latch (required for battery operation)...");
    if (!initTCA9554()) {
        Serial.println("[Battery] WARNING: TCA9554 init failed - power latch unavailable");
    } else {
        if (latchPowerOn()) {
            Serial.println("[Battery] Power latch engaged - device will stay on after button release");
        } else {
            Serial.println("[Battery] WARNING: Power latch verification failed!");
        }
    }
    
    // Now determine if we're on battery power with debouncing
    // GPIO16 is HIGH when on battery, LOW when on USB (or button pressed)
    // Use INPUT (no pullup) to avoid biasing the reading if pin is driven externally
    pinMode(PWR_BUTTON_GPIO, INPUT);
    
    // Sample GPIO16 multiple times to debounce
    const int samples = 10;
    int highCount = 0;
    
    Serial.println("[Battery] Sampling power source detection...");
    for (int i = 0; i < samples; i++) {
        if (digitalRead(PWR_BUTTON_GPIO) == HIGH) {
            highCount++;
        }
        delay(5);  // 5ms between samples = 50ms total
    }
    
    // Majority vote
    onBattery = (highCount > samples / 2);
    
    Serial.printf("[Battery] Power detection: GPIO16 samples=%d/%d (HIGH), decision=%s\n",
                  highCount, samples, onBattery ? "BATTERY" : "USB");
    
    // Initialize ADC for battery voltage reading
    if (!initADC()) {
        Serial.println("[Battery] WARNING: ADC init failed, voltage monitoring disabled");
    }
    
    // Read initial voltage for diagnostics
    uint16_t initialVoltage = 0;
    if (adc1_handle) {
        initialVoltage = readADCMillivolts();
        Serial.printf("[Battery] Initial voltage reading: %dmV\n", initialVoltage);
        
        // Sanity check: if we think we're on USB but voltage looks like battery
        if (!onBattery && initialVoltage > BATTERY_EMPTY_MV && initialVoltage < BATTERY_FULL_MV + 500) {
            Serial.printf("[Battery] WARNING: USB mode but battery voltage detected (%dmV)\n", initialVoltage);
        }
        // Sanity check: if we think we're on battery but voltage is too low or zero
        if (onBattery && initialVoltage < BATTERY_EMPTY_MV) {
            Serial.printf("[Battery] WARNING: Battery mode but voltage too low (%dmV)\n", initialVoltage);
        }
    }
    
    // TCA9554 already initialized if on battery (done early)
    // Just log the final status
    if (onBattery && !initialized) {
        Serial.println("[Battery] Power latch already set (early init)");
    }
    
    initialized = true;
    
    // Do initial cache update to populate voltage reading
    update();
    Serial.printf("[Battery] Cached state: %dmV, %d%%, hasBattery=%d\n", 
                  cachedVoltage, cachedPercent, hasBattery());
    
    Serial.printf("[Battery] Battery manager initialized - Mode: %s\n", 
                  onBattery ? "BATTERY" : "USB");
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
    
    // Brief delay for I2C bus to stabilize
    delay(10);
    
    // Check if TCA9554 responds with retries
    uint8_t error = 1;
    for (int retry = 0; retry < 5; retry++) {
        tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
        error = tca9554Wire.endTransmission();
        if (error == 0) break;
        Serial.printf("[Battery] TCA9554 probe attempt %d failed\n", retry + 1);
        delay(5);
    }
    
    if (error != 0) {
        Serial.printf("[Battery] TCA9554 not found at 0x%02X after retries\n", TCA9554_I2C_ADDR);
        return false;
    }
    
    // CRITICAL: Set output HIGH FIRST before configuring as output
    // Preserve other output states by read-modify-write
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_OUTPUT_PORT);
    tca9554Wire.endTransmission(false);
    tca9554Wire.requestFrom(TCA9554_I2C_ADDR, (uint8_t)1);
    uint8_t current = 0;
    if (tca9554Wire.available() >= 1) {
        current = tca9554Wire.read();
    }
    current |= (1 << TCA9554_PWR_LATCH_PIN);  // Ensure latch pin HIGH
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_OUTPUT_PORT);
    tca9554Wire.write(current);
    error = tca9554Wire.endTransmission();
    
    if (error != 0) {
        Serial.printf("[Battery] TCA9554 output set failed: %d\n", error);
        return false;
    }
    
    // Now configure pin 6 as output (output is already HIGH)
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_CONFIG_PORT);
    tca9554Wire.write(0xBF);  // All inputs except pin 6 (bit 6 = 0 = output)
    error = tca9554Wire.endTransmission();
    
    if (error != 0) {
        Serial.printf("[Battery] TCA9554 config failed: %d\n", error);
        return false;
    }
    
    Serial.println("[Battery] TCA9554 initialized - power latch engaged");
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

bool BatteryManager::hasBattery() const {
    // Debug simulation mode - if simulatedVoltage is set, use it
    if (simulatedVoltage > 0) {
        return true;
    }
    
    // Must be initialized with working ADC to detect battery
    if (!initialized || !adc1_handle) {
        return false;
    }
    
    // Only show battery icon when actually running on battery power
    // When on USB, we don't show the battery icon even if physically present
    if (!onBattery) {
        return false;
    }
    
    // Verify with actual voltage - if below minimum, no real battery
    // This catches cases where GPIO16 floats HIGH but no battery is connected
    if (cachedVoltage < BATTERY_EMPTY_MV) {
        return false;
    }
    
    return true;
}

void BatteryManager::simulateBattery(uint16_t voltageMV) {
    simulatedVoltage = voltageMV;
    if (voltageMV > 0) {
        // Update cached values to match simulation
        cachedVoltage = voltageMV;
        cachedPercent = (uint8_t)((voltageMV - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
        if (cachedPercent > 100) cachedPercent = 100;
        Serial.printf("[Battery] SIMULATION: %dmV (%d%%)\n", voltageMV, cachedPercent);
    } else {
        Serial.println("[Battery] Simulation disabled");
    }
}

void BatteryManager::update() {
    if (!initialized) {
        return;
    }
    
    // Skip normal updates if in simulation mode
    if (simulatedVoltage > 0) {
        return;
    }
    
    unsigned long now = millis();

    // Refresh power source detection periodically to handle USB/battery swaps
    // Skip while the power button is held (GPIO16 LOW) to avoid misclassifying as USB
    static unsigned long lastPowerCheckMs = 0;
    if (now - lastPowerCheckMs >= 1000 && !isPowerButtonPressed()) {
        const int samples = 5;
        int highCount = 0;
        for (int i = 0; i < samples; i++) {
            if (digitalRead(PWR_BUTTON_GPIO) == HIGH) {
                highCount++;
            }
        }
        bool detectedBattery = (highCount > samples / 2);
        if (detectedBattery != onBattery) {
            onBattery = detectedBattery;
            Serial.printf("[Battery] Power source changed: %s\n", onBattery ? "BATTERY" : "USB");
        }
        lastPowerCheckMs = now;
    }
    
    // Update cached values at 1Hz
    if (now - lastUpdateMs >= 1000) {
        uint16_t voltage = readADCMillivolts();
        cachedVoltage = voltage;
        
        // Calculate percentage
        if (voltage >= BATTERY_FULL_MV) {
            cachedPercent = 100;
        } else if (voltage <= BATTERY_EMPTY_MV) {
            cachedPercent = 0;
        } else {
            // Linear interpolation
            cachedPercent = (uint8_t)((voltage - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
        }
        
        lastUpdateMs = now;
    }
}

uint16_t BatteryManager::getVoltageMillivolts() const {
    return cachedVoltage;
}

uint8_t BatteryManager::getPercentage() const {
    return cachedPercent;
}

bool BatteryManager::isLow() const {
    return cachedVoltage < BATTERY_WARNING_MV && cachedVoltage > 0;
}

bool BatteryManager::isCritical() const {
    return cachedVoltage < BATTERY_CRITICAL_MV && cachedVoltage > 0;
}

bool BatteryManager::latchPowerOn() {
    // Verify the latch is HIGH (should already be set by initTCA9554)
    Serial.println("[Battery] Verifying power latch is ON");
    
    // Read current output state
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_OUTPUT_PORT);
    tca9554Wire.endTransmission(false);
    tca9554Wire.requestFrom(TCA9554_I2C_ADDR, (uint8_t)1);
    
    if (tca9554Wire.available() < 1) {
        Serial.println("[Battery] Failed to read power latch state");
        return false;
    }
    
    uint8_t current = tca9554Wire.read();
    bool latchHigh = (current & (1 << TCA9554_PWR_LATCH_PIN)) != 0;
    
    Serial.printf("[Battery] Power latch pin 6 is %s (0x%02X)\n", 
                  latchHigh ? "HIGH" : "LOW", current);
    
    if (!latchHigh) {
        Serial.println("[Battery] WARNING: Latch is LOW - forcing HIGH!");
        return setTCA9554Pin(TCA9554_PWR_LATCH_PIN, true);
    }
    
    return true;
}

bool BatteryManager::powerOff() {
    if (!onBattery) {
        Serial.println("[Battery] Cannot power off - not on battery");
        return false;
    }
    
    Serial.println("[Battery] Initiating graceful shutdown...");
    
    // Step 1: Save settings to ensure state is preserved
    Serial.println("[Battery] Saving settings...");
    settingsManager.save();
    
    // Step 2: Show shutdown screen
    Serial.println("[Battery] Showing shutdown screen...");
    display.showShutdown();
    
    // Step 3: Brief delay for user feedback
    delay(1000);
    
    // Step 4: Fade backlight (optional smooth transition)
    Serial.println("[Battery] Fading backlight...");
    for (int i = 0; i <= 255; i += 5) {
        analogWrite(LCD_BL, i);  // Inverted: 255 = off
        delay(10);
    }
    
    // Step 5: Cut power
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

BatteryManager::BatteryManager() : initialized(false), onBattery(false), lastVoltage(0), cachedVoltage(0), cachedPercent(0), lastUpdateMs(0) {}
bool BatteryManager::begin() { return false; }
bool BatteryManager::isOnBattery() const { return false; }
bool BatteryManager::hasBattery() const { return false; }
void BatteryManager::update() {}
uint16_t BatteryManager::getVoltageMillivolts() const { return 0; }
uint8_t BatteryManager::getPercentage() const { return 0; }
bool BatteryManager::isLow() const { return false; }
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

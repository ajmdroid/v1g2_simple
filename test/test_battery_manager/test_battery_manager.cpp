/**
 * Battery Manager Unit Tests
 * 
 * Tests voltage thresholds, percentage calculations, and state detection.
 * These tests catch bugs where:
 * - Percentage calculations overflow or produce wrong values
 * - Threshold checks use wrong comparison operators
 * - Edge cases at boundaries aren't handled
 */

#include <unity.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <cstdint>

// ============================================================================
// CONSTANTS EXTRACTED FROM battery_manager.h
// ============================================================================

static constexpr uint16_t BATTERY_FULL_MV = 4095;
static constexpr uint16_t BATTERY_EMPTY_MV = 3200;
static constexpr uint16_t BATTERY_WARNING_MV = 3400;
static constexpr uint16_t BATTERY_CRITICAL_MV = 3250;

// ============================================================================
// PURE FUNCTIONS EXTRACTED FOR TESTING
// ============================================================================

/**
 * Calculate battery percentage from voltage (from battery_manager.cpp)
 * Linear interpolation between EMPTY and FULL
 */
uint8_t voltageToPercent(uint16_t voltageMV) {
    if (voltageMV >= BATTERY_FULL_MV) {
        return 100;
    } else if (voltageMV <= BATTERY_EMPTY_MV) {
        return 0;
    } else {
        // Linear interpolation
        return (uint8_t)((voltageMV - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
    }
}

/**
 * Check if battery is low (from battery_manager.cpp)
 * Low = below warning threshold but above 0 (to ignore "no battery" readings)
 */
bool isLow(uint16_t voltageMV) {
    return voltageMV < BATTERY_WARNING_MV && voltageMV > 0;
}

/**
 * Check if battery is critical (from battery_manager.cpp)
 * Critical = below critical threshold but above 0
 */
bool isCritical(uint16_t voltageMV) {
    return voltageMV < BATTERY_CRITICAL_MV && voltageMV > 0;
}

/**
 * Get battery status string (simplified from battery_manager.cpp)
 */
const char* getBatteryStatus(uint16_t voltageMV) {
    if (voltageMV == 0) return "NO_BATTERY";
    if (voltageMV < BATTERY_CRITICAL_MV) return "CRITICAL";
    if (voltageMV < BATTERY_WARNING_MV) return "LOW";
    if (voltageMV >= BATTERY_FULL_MV) return "FULL";
    return "OK";
}

// ============================================================================
// VOLTAGE TO PERCENTAGE TESTS
// ============================================================================

void test_percent_at_full_voltage() {
    TEST_ASSERT_EQUAL_UINT8(100, voltageToPercent(BATTERY_FULL_MV));
}

void test_percent_above_full_voltage() {
    // Above full should still be 100%
    TEST_ASSERT_EQUAL_UINT8(100, voltageToPercent(4200));
    TEST_ASSERT_EQUAL_UINT8(100, voltageToPercent(5000));
}

void test_percent_at_empty_voltage() {
    TEST_ASSERT_EQUAL_UINT8(0, voltageToPercent(BATTERY_EMPTY_MV));
}

void test_percent_below_empty_voltage() {
    // Below empty should still be 0%
    TEST_ASSERT_EQUAL_UINT8(0, voltageToPercent(3000));
    TEST_ASSERT_EQUAL_UINT8(0, voltageToPercent(0));
}

void test_percent_at_midpoint() {
    // Midpoint: 3200 + (4095-3200)/2 = 3200 + 447.5 ≈ 3648
    // Expected: 50%
    uint16_t midpoint = BATTERY_EMPTY_MV + (BATTERY_FULL_MV - BATTERY_EMPTY_MV) / 2;
    uint8_t pct = voltageToPercent(midpoint);
    TEST_ASSERT_UINT8_WITHIN(1, 50, pct);  // Allow ±1 for rounding
}

void test_percent_at_warning_threshold() {
    // 3400mV: (3400 - 3200) * 100 / (4095 - 3200) = 200 * 100 / 895 ≈ 22%
    uint8_t pct = voltageToPercent(BATTERY_WARNING_MV);
    TEST_ASSERT_UINT8_WITHIN(1, 22, pct);
}

void test_percent_at_critical_threshold() {
    // 3250mV: (3250 - 3200) * 100 / (4095 - 3200) = 50 * 100 / 895 ≈ 5%
    uint8_t pct = voltageToPercent(BATTERY_CRITICAL_MV);
    TEST_ASSERT_UINT8_WITHIN(1, 5, pct);
}

void test_percent_at_75_percent() {
    // 75% = 3200 + 0.75 * 895 = 3200 + 671 = 3871mV
    uint16_t voltage = BATTERY_EMPTY_MV + (BATTERY_FULL_MV - BATTERY_EMPTY_MV) * 3 / 4;
    uint8_t pct = voltageToPercent(voltage);
    TEST_ASSERT_UINT8_WITHIN(1, 75, pct);
}

void test_percent_at_25_percent() {
    // 25% = 3200 + 0.25 * 895 = 3200 + 224 = 3424mV
    uint16_t voltage = BATTERY_EMPTY_MV + (BATTERY_FULL_MV - BATTERY_EMPTY_MV) / 4;
    uint8_t pct = voltageToPercent(voltage);
    TEST_ASSERT_UINT8_WITHIN(1, 25, pct);
}

void test_percent_just_above_empty() {
    // 3201mV: (1 * 100) / 895 = 0%
    TEST_ASSERT_UINT8_WITHIN(1, 0, voltageToPercent(3201));
}

void test_percent_just_below_full() {
    // 4094mV: (894 * 100) / 895 ≈ 99%
    TEST_ASSERT_UINT8_WITHIN(1, 99, voltageToPercent(4094));
}

// ============================================================================
// IS LOW TESTS
// ============================================================================

void test_is_low_at_warning_minus_1() {
    TEST_ASSERT_TRUE(isLow(BATTERY_WARNING_MV - 1));  // 3399mV
}

void test_is_low_at_warning() {
    // At threshold is NOT low (below, not at-or-below)
    TEST_ASSERT_FALSE(isLow(BATTERY_WARNING_MV));  // 3400mV
}

void test_is_low_above_warning() {
    TEST_ASSERT_FALSE(isLow(BATTERY_WARNING_MV + 1));  // 3401mV
    TEST_ASSERT_FALSE(isLow(4000));
}

void test_is_low_at_critical() {
    TEST_ASSERT_TRUE(isLow(BATTERY_CRITICAL_MV));  // 3250mV - still below warning
}

void test_is_low_at_zero() {
    // Zero voltage means no battery - not "low"
    TEST_ASSERT_FALSE(isLow(0));
}

void test_is_low_at_1mv() {
    // Very low but non-zero - should be low
    TEST_ASSERT_TRUE(isLow(1));
}

// ============================================================================
// IS CRITICAL TESTS
// ============================================================================

void test_is_critical_at_threshold_minus_1() {
    TEST_ASSERT_TRUE(isCritical(BATTERY_CRITICAL_MV - 1));  // 3249mV
}

void test_is_critical_at_threshold() {
    // At threshold is NOT critical (below, not at-or-below)
    TEST_ASSERT_FALSE(isCritical(BATTERY_CRITICAL_MV));  // 3250mV
}

void test_is_critical_above_threshold() {
    TEST_ASSERT_FALSE(isCritical(BATTERY_CRITICAL_MV + 1));  // 3251mV
    TEST_ASSERT_FALSE(isCritical(3400));
    TEST_ASSERT_FALSE(isCritical(4000));
}

void test_is_critical_near_empty() {
    TEST_ASSERT_TRUE(isCritical(3201));  // Just above empty
}

void test_is_critical_at_zero() {
    // Zero voltage means no battery - not "critical"
    TEST_ASSERT_FALSE(isCritical(0));
}

void test_is_critical_at_1mv() {
    // Very low but non-zero - should be critical
    TEST_ASSERT_TRUE(isCritical(1));
}

// ============================================================================
// STATUS STRING TESTS
// ============================================================================

void test_status_no_battery() {
    TEST_ASSERT_EQUAL_STRING("NO_BATTERY", getBatteryStatus(0));
}

void test_status_critical() {
    TEST_ASSERT_EQUAL_STRING("CRITICAL", getBatteryStatus(3200));  // At empty
    TEST_ASSERT_EQUAL_STRING("CRITICAL", getBatteryStatus(3100));  // Below empty
}

void test_status_low() {
    TEST_ASSERT_EQUAL_STRING("LOW", getBatteryStatus(3300));  // Between critical and warning
    TEST_ASSERT_EQUAL_STRING("LOW", getBatteryStatus(3250));  // At critical threshold
}

void test_status_ok() {
    TEST_ASSERT_EQUAL_STRING("OK", getBatteryStatus(3500));  // Normal range
    TEST_ASSERT_EQUAL_STRING("OK", getBatteryStatus(3800));
    TEST_ASSERT_EQUAL_STRING("OK", getBatteryStatus(4000));
}

void test_status_full() {
    TEST_ASSERT_EQUAL_STRING("FULL", getBatteryStatus(4095));  // At full
    TEST_ASSERT_EQUAL_STRING("FULL", getBatteryStatus(4200));  // Above full
}

// ============================================================================
// THRESHOLD RELATIONSHIP TESTS
// ============================================================================

void test_thresholds_ordering() {
    // Verify thresholds are in correct order: EMPTY < CRITICAL < WARNING < FULL
    TEST_ASSERT_LESS_THAN(BATTERY_FULL_MV, BATTERY_WARNING_MV);      // 3400 < 4095
    TEST_ASSERT_LESS_THAN(BATTERY_WARNING_MV, BATTERY_CRITICAL_MV);  // 3250 < 3400
    TEST_ASSERT_LESS_THAN(BATTERY_CRITICAL_MV, BATTERY_EMPTY_MV);    // 3200 < 3250
}

void test_threshold_values() {
    // Document expected values
    TEST_ASSERT_EQUAL_UINT16(4095, BATTERY_FULL_MV);
    TEST_ASSERT_EQUAL_UINT16(3200, BATTERY_EMPTY_MV);
    TEST_ASSERT_EQUAL_UINT16(3400, BATTERY_WARNING_MV);
    TEST_ASSERT_EQUAL_UINT16(3250, BATTERY_CRITICAL_MV);
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

void test_percent_no_overflow_at_max_uint16() {
    // Should not overflow when calculating with large values
    uint8_t pct = voltageToPercent(65535);
    TEST_ASSERT_EQUAL_UINT8(100, pct);  // Should cap at 100%
}

void test_percent_monotonic_increase() {
    // Percentage should increase monotonically with voltage
    uint8_t prev = 0;
    for (uint16_t v = BATTERY_EMPTY_MV; v <= BATTERY_FULL_MV; v += 50) {
        uint8_t current = voltageToPercent(v);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(prev, current);
        prev = current;
    }
}

void test_low_not_critical_boundary() {
    // There should be a range that is low but not critical
    uint16_t testVoltage = (BATTERY_CRITICAL_MV + BATTERY_WARNING_MV) / 2;  // 3325mV
    TEST_ASSERT_TRUE(isLow(testVoltage));
    TEST_ASSERT_FALSE(isCritical(testVoltage));
}

void test_critical_is_also_low() {
    // If it's critical, it should also be low
    uint16_t criticalVoltage = BATTERY_CRITICAL_MV - 50;  // 3200mV
    TEST_ASSERT_TRUE(isCritical(criticalVoltage));
    TEST_ASSERT_TRUE(isLow(criticalVoltage));
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void setUp(void) {}
void tearDown(void) {}

void runAllTests() {
    // Voltage to percentage tests
    RUN_TEST(test_percent_at_full_voltage);
    RUN_TEST(test_percent_above_full_voltage);
    RUN_TEST(test_percent_at_empty_voltage);
    RUN_TEST(test_percent_below_empty_voltage);
    RUN_TEST(test_percent_at_midpoint);
    RUN_TEST(test_percent_at_warning_threshold);
    RUN_TEST(test_percent_at_critical_threshold);
    RUN_TEST(test_percent_at_75_percent);
    RUN_TEST(test_percent_at_25_percent);
    RUN_TEST(test_percent_just_above_empty);
    RUN_TEST(test_percent_just_below_full);
    
    // Is low tests
    RUN_TEST(test_is_low_at_warning_minus_1);
    RUN_TEST(test_is_low_at_warning);
    RUN_TEST(test_is_low_above_warning);
    RUN_TEST(test_is_low_at_critical);
    RUN_TEST(test_is_low_at_zero);
    RUN_TEST(test_is_low_at_1mv);
    
    // Is critical tests
    RUN_TEST(test_is_critical_at_threshold_minus_1);
    RUN_TEST(test_is_critical_at_threshold);
    RUN_TEST(test_is_critical_above_threshold);
    RUN_TEST(test_is_critical_near_empty);
    RUN_TEST(test_is_critical_at_zero);
    RUN_TEST(test_is_critical_at_1mv);
    
    // Status string tests
    RUN_TEST(test_status_no_battery);
    RUN_TEST(test_status_critical);
    RUN_TEST(test_status_low);
    RUN_TEST(test_status_ok);
    RUN_TEST(test_status_full);
    
    // Threshold relationship tests
    RUN_TEST(test_thresholds_ordering);
    RUN_TEST(test_threshold_values);
    
    // Edge case tests
    RUN_TEST(test_percent_no_overflow_at_max_uint16);
    RUN_TEST(test_percent_monotonic_increase);
    RUN_TEST(test_low_not_critical_boundary);
    RUN_TEST(test_critical_is_also_low);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main(int argc, char **argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif

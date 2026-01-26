/**
 * OBD Handler Unit Tests
 * 
 * Tests hex validation, response parsing, and backoff calculation.
 * These tests catch bugs where:
 * - Invalid hex strings crash the parser
 * - Speed/RPM parsing produces wrong values
 * - Exponential backoff doesn't cap correctly
 */

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <string>

// ============================================================================
// PURE FUNCTIONS EXTRACTED FOR TESTING
// ============================================================================

/**
 * OBD-II connection states (from obd_handler.h)
 */
enum class OBDState {
    OBD_DISABLED,       // OBD not enabled in settings
    IDLE,               // Waiting to start scan
    SCANNING,           // Scanning for ELM327 device
    CONNECTING,         // Connecting to found device
    INITIALIZING,       // Sending AT init commands
    READY,              // Connected and initialized
    POLLING,            // Actively polling for data
    DISCONNECTED,       // Was connected, now disconnected
    FAILED              // Detection timeout or init failed
};

/**
 * Convert OBDState to string for logging
 */
const char* obdStateToString(OBDState state) {
    switch (state) {
        case OBDState::OBD_DISABLED: return "OBD_DISABLED";
        case OBDState::IDLE: return "IDLE";
        case OBDState::SCANNING: return "SCANNING";
        case OBDState::CONNECTING: return "CONNECTING";
        case OBDState::INITIALIZING: return "INITIALIZING";
        case OBDState::READY: return "READY";
        case OBDState::POLLING: return "POLLING";
        case OBDState::DISCONNECTED: return "DISCONNECTED";
        case OBDState::FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

/**
 * Backoff constants (from obd_handler.h)
 */
static constexpr uint8_t MAX_CONNECTION_FAILURES = 5;
static constexpr uint32_t BASE_RETRY_DELAY_MS = 5000;
static constexpr uint32_t MAX_RETRY_DELAY_MS = 60000;

/**
 * Validate that a string contains only valid hex characters
 * (from obd_handler.cpp)
 */
bool isValidHexString(const std::string& str, size_t expectedLen = 0) {
    if (str.length() == 0) return false;
    if (expectedLen > 0 && str.length() != expectedLen) return false;
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        bool isHex = (c >= '0' && c <= '9') || 
                     (c >= 'A' && c <= 'F') || 
                     (c >= 'a' && c <= 'f');
        if (!isHex) return false;
    }
    return true;
}

/**
 * Parse speed response (from obd_handler.cpp)
 * Response format: "410DXX" where XX is speed in km/h (hex)
 */
bool parseSpeedResponse(const std::string& response, uint8_t& speedKph) {
    // Find "410D" or "410d"
    size_t idx = response.find("410D");
    if (idx == std::string::npos) {
        idx = response.find("410d");
    }
    if (idx == std::string::npos) {
        return false;
    }
    
    // Get the hex value after "410D"
    if (idx + 6 > response.length()) {
        return false;
    }
    
    std::string hexVal = response.substr(idx + 4, 2);
    
    // Validate hex characters before parsing
    if (!isValidHexString(hexVal, 2)) {
        return false;
    }
    
    speedKph = (uint8_t)strtoul(hexVal.c_str(), nullptr, 16);
    return true;
}

/**
 * Parse RPM response (from obd_handler.cpp)
 * Response format: "410CXXYY" where RPM = ((XX * 256) + YY) / 4
 */
bool parseRPMResponse(const std::string& response, uint16_t& rpm) {
    // Find "410C" or "410c"
    size_t idx = response.find("410C");
    if (idx == std::string::npos) {
        idx = response.find("410c");
    }
    if (idx == std::string::npos) {
        return false;
    }
    
    // Need 4 hex digits after "410C"
    if (idx + 8 > response.length()) {
        return false;
    }
    
    std::string hexA = response.substr(idx + 4, 2);
    std::string hexB = response.substr(idx + 6, 2);
    
    // Validate hex characters before parsing
    if (!isValidHexString(hexA, 2) || !isValidHexString(hexB, 2)) {
        return false;
    }
    
    uint8_t a = (uint8_t)strtoul(hexA.c_str(), nullptr, 16);
    uint8_t b = (uint8_t)strtoul(hexB.c_str(), nullptr, 16);
    
    rpm = ((uint16_t)a * 256 + b) / 4;
    return true;
}

/**
 * Parse voltage response (from obd_handler.cpp)
 * Response format: "12.5V" or similar floating point value
 */
bool parseVoltageResponse(const std::string& response, float& voltage) {
    voltage = strtof(response.c_str(), nullptr);
    return voltage > 0 && voltage < 20;  // Sanity check
}

/**
 * Calculate retry delay with exponential backoff
 * (from obd_handler.cpp)
 */
uint32_t calculateRetryDelay(uint8_t connectionFailures) {
    uint32_t retryDelay = BASE_RETRY_DELAY_MS * (1 << connectionFailures);
    if (retryDelay > MAX_RETRY_DELAY_MS) {
        retryDelay = MAX_RETRY_DELAY_MS;
    }
    return retryDelay;
}

/**
 * Check if max failures has been reached
 */
bool shouldGiveUp(uint8_t connectionFailures) {
    return connectionFailures >= MAX_CONNECTION_FAILURES;
}

// ============================================================================
// HEX VALIDATION TESTS
// ============================================================================

void test_hex_valid_uppercase() {
    TEST_ASSERT_TRUE(isValidHexString("ABCDEF"));
}

void test_hex_valid_lowercase() {
    TEST_ASSERT_TRUE(isValidHexString("abcdef"));
}

void test_hex_valid_mixed_case() {
    TEST_ASSERT_TRUE(isValidHexString("AbCdEf"));
}

void test_hex_valid_digits() {
    TEST_ASSERT_TRUE(isValidHexString("0123456789"));
}

void test_hex_invalid_g() {
    TEST_ASSERT_FALSE(isValidHexString("12G4"));
}

void test_hex_invalid_space() {
    TEST_ASSERT_FALSE(isValidHexString("12 34"));
}

void test_hex_empty_string() {
    TEST_ASSERT_FALSE(isValidHexString(""));
}

void test_hex_valid_with_expected_len() {
    TEST_ASSERT_TRUE(isValidHexString("AB", 2));
    TEST_ASSERT_TRUE(isValidHexString("ABCD", 4));
}

void test_hex_wrong_length() {
    TEST_ASSERT_FALSE(isValidHexString("ABC", 2));  // Too long
    TEST_ASSERT_FALSE(isValidHexString("A", 2));    // Too short
}

// ============================================================================
// SPEED PARSING TESTS
// ============================================================================

void test_speed_parse_zero() {
    uint8_t speed = 0xFF;
    TEST_ASSERT_TRUE(parseSpeedResponse("410D00", speed));
    TEST_ASSERT_EQUAL_UINT8(0, speed);
}

void test_speed_parse_60kph() {
    uint8_t speed = 0;
    TEST_ASSERT_TRUE(parseSpeedResponse("410D3C", speed));  // 0x3C = 60
    TEST_ASSERT_EQUAL_UINT8(60, speed);
}

void test_speed_parse_100kph() {
    uint8_t speed = 0;
    TEST_ASSERT_TRUE(parseSpeedResponse("410D64", speed));  // 0x64 = 100
    TEST_ASSERT_EQUAL_UINT8(100, speed);
}

void test_speed_parse_max_255() {
    uint8_t speed = 0;
    TEST_ASSERT_TRUE(parseSpeedResponse("410DFF", speed));  // 0xFF = 255
    TEST_ASSERT_EQUAL_UINT8(255, speed);
}

void test_speed_parse_lowercase() {
    uint8_t speed = 0;
    TEST_ASSERT_TRUE(parseSpeedResponse("410d3c", speed));
    TEST_ASSERT_EQUAL_UINT8(60, speed);
}

void test_speed_parse_with_prefix() {
    uint8_t speed = 0;
    TEST_ASSERT_TRUE(parseSpeedResponse(">410D50", speed));  // 0x50 = 80
    TEST_ASSERT_EQUAL_UINT8(80, speed);
}

void test_speed_parse_with_suffix() {
    uint8_t speed = 0;
    TEST_ASSERT_TRUE(parseSpeedResponse("410D32\r\n>", speed));  // 0x32 = 50
    TEST_ASSERT_EQUAL_UINT8(50, speed);
}

void test_speed_parse_missing_header() {
    uint8_t speed = 0;
    TEST_ASSERT_FALSE(parseSpeedResponse("3C", speed));
}

void test_speed_parse_incomplete() {
    uint8_t speed = 0;
    TEST_ASSERT_FALSE(parseSpeedResponse("410D", speed));  // Missing value
}

void test_speed_parse_invalid_hex() {
    uint8_t speed = 0;
    TEST_ASSERT_FALSE(parseSpeedResponse("410DGH", speed));  // Invalid hex
}

// ============================================================================
// RPM PARSING TESTS
// ============================================================================

void test_rpm_parse_idle() {
    uint16_t rpm = 0;
    // Idle RPM ~800: 800 * 4 = 3200 = 0x0C80
    TEST_ASSERT_TRUE(parseRPMResponse("410C0C80", rpm));
    TEST_ASSERT_EQUAL_UINT16(800, rpm);
}

void test_rpm_parse_zero() {
    uint16_t rpm = 0xFFFF;
    TEST_ASSERT_TRUE(parseRPMResponse("410C0000", rpm));
    TEST_ASSERT_EQUAL_UINT16(0, rpm);
}

void test_rpm_parse_3000() {
    uint16_t rpm = 0;
    // 3000 RPM: 3000 * 4 = 12000 = 0x2EE0
    TEST_ASSERT_TRUE(parseRPMResponse("410C2EE0", rpm));
    TEST_ASSERT_EQUAL_UINT16(3000, rpm);
}

void test_rpm_parse_max() {
    uint16_t rpm = 0;
    // Max: FFFF / 4 = 16383
    TEST_ASSERT_TRUE(parseRPMResponse("410CFFFF", rpm));
    TEST_ASSERT_EQUAL_UINT16(16383, rpm);
}

void test_rpm_parse_lowercase() {
    uint16_t rpm = 0;
    TEST_ASSERT_TRUE(parseRPMResponse("410c0c80", rpm));
    TEST_ASSERT_EQUAL_UINT16(800, rpm);
}

void test_rpm_parse_with_prefix() {
    uint16_t rpm = 0;
    TEST_ASSERT_TRUE(parseRPMResponse(">410C0C80", rpm));
    TEST_ASSERT_EQUAL_UINT16(800, rpm);
}

void test_rpm_parse_missing_header() {
    uint16_t rpm = 0;
    TEST_ASSERT_FALSE(parseRPMResponse("0C80", rpm));
}

void test_rpm_parse_incomplete() {
    uint16_t rpm = 0;
    TEST_ASSERT_FALSE(parseRPMResponse("410C0C", rpm));  // Only 2 hex digits
}

// ============================================================================
// VOLTAGE PARSING TESTS
// ============================================================================

void test_voltage_parse_normal() {
    float voltage = 0;
    TEST_ASSERT_TRUE(parseVoltageResponse("12.5V", voltage));
    TEST_ASSERT_FLOAT_WITHIN(0.1, 12.5, voltage);
}

void test_voltage_parse_low() {
    float voltage = 0;
    TEST_ASSERT_TRUE(parseVoltageResponse("11.8", voltage));
    TEST_ASSERT_FLOAT_WITHIN(0.1, 11.8, voltage);
}

void test_voltage_parse_high() {
    float voltage = 0;
    TEST_ASSERT_TRUE(parseVoltageResponse("14.2V", voltage));
    TEST_ASSERT_FLOAT_WITHIN(0.1, 14.2, voltage);
}

void test_voltage_parse_zero_fails() {
    float voltage = 999;
    TEST_ASSERT_FALSE(parseVoltageResponse("0.0V", voltage));
}

void test_voltage_parse_too_high_fails() {
    float voltage = 0;
    TEST_ASSERT_FALSE(parseVoltageResponse("25.0V", voltage));  // >20V fails sanity check
}

void test_voltage_parse_negative_fails() {
    float voltage = 0;
    TEST_ASSERT_FALSE(parseVoltageResponse("-5.0V", voltage));
}

// ============================================================================
// BACKOFF CALCULATION TESTS
// ============================================================================

void test_retry_delay_first_failure() {
    // 5000 * (1 << 0) = 5000ms
    TEST_ASSERT_EQUAL_UINT32(5000, calculateRetryDelay(0));
}

void test_retry_delay_second_failure() {
    // 5000 * (1 << 1) = 10000ms
    TEST_ASSERT_EQUAL_UINT32(10000, calculateRetryDelay(1));
}

void test_retry_delay_third_failure() {
    // 5000 * (1 << 2) = 20000ms
    TEST_ASSERT_EQUAL_UINT32(20000, calculateRetryDelay(2));
}

void test_retry_delay_fourth_failure() {
    // 5000 * (1 << 3) = 40000ms
    TEST_ASSERT_EQUAL_UINT32(40000, calculateRetryDelay(3));
}

void test_retry_delay_fifth_failure_capped() {
    // 5000 * (1 << 4) = 80000ms but capped to 60000ms
    TEST_ASSERT_EQUAL_UINT32(60000, calculateRetryDelay(4));
}

void test_retry_delay_many_failures_capped() {
    // Should stay capped at 60000ms
    TEST_ASSERT_EQUAL_UINT32(60000, calculateRetryDelay(5));
    TEST_ASSERT_EQUAL_UINT32(60000, calculateRetryDelay(10));
}

// ============================================================================
// GIVE UP CHECK TESTS
// ============================================================================

void test_give_up_at_zero() {
    TEST_ASSERT_FALSE(shouldGiveUp(0));
}

void test_give_up_at_four() {
    TEST_ASSERT_FALSE(shouldGiveUp(4));
}

void test_give_up_at_five() {
    TEST_ASSERT_TRUE(shouldGiveUp(5));
}

void test_give_up_beyond_five() {
    TEST_ASSERT_TRUE(shouldGiveUp(6));
    TEST_ASSERT_TRUE(shouldGiveUp(100));
}

// ============================================================================
// STATE ENUM TESTS
// ============================================================================

void test_obd_state_enum_values() {
    // Verify enum values for state machine
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(OBDState::OBD_DISABLED));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(OBDState::IDLE));
    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(OBDState::SCANNING));
    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(OBDState::CONNECTING));
    TEST_ASSERT_EQUAL_INT(4, static_cast<int>(OBDState::INITIALIZING));
    TEST_ASSERT_EQUAL_INT(5, static_cast<int>(OBDState::READY));
    TEST_ASSERT_EQUAL_INT(6, static_cast<int>(OBDState::POLLING));
    TEST_ASSERT_EQUAL_INT(7, static_cast<int>(OBDState::DISCONNECTED));
    TEST_ASSERT_EQUAL_INT(8, static_cast<int>(OBDState::FAILED));
}

void test_obd_state_strings() {
    TEST_ASSERT_EQUAL_STRING("OBD_DISABLED", obdStateToString(OBDState::OBD_DISABLED));
    TEST_ASSERT_EQUAL_STRING("IDLE", obdStateToString(OBDState::IDLE));
    TEST_ASSERT_EQUAL_STRING("SCANNING", obdStateToString(OBDState::SCANNING));
    TEST_ASSERT_EQUAL_STRING("CONNECTING", obdStateToString(OBDState::CONNECTING));
    TEST_ASSERT_EQUAL_STRING("INITIALIZING", obdStateToString(OBDState::INITIALIZING));
    TEST_ASSERT_EQUAL_STRING("READY", obdStateToString(OBDState::READY));
    TEST_ASSERT_EQUAL_STRING("POLLING", obdStateToString(OBDState::POLLING));
    TEST_ASSERT_EQUAL_STRING("DISCONNECTED", obdStateToString(OBDState::DISCONNECTED));
    TEST_ASSERT_EQUAL_STRING("FAILED", obdStateToString(OBDState::FAILED));
}

void test_obd_state_unknown() {
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", obdStateToString(static_cast<OBDState>(99)));
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void setUp(void) {}
void tearDown(void) {}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Hex validation tests
    RUN_TEST(test_hex_valid_uppercase);
    RUN_TEST(test_hex_valid_lowercase);
    RUN_TEST(test_hex_valid_mixed_case);
    RUN_TEST(test_hex_valid_digits);
    RUN_TEST(test_hex_invalid_g);
    RUN_TEST(test_hex_invalid_space);
    RUN_TEST(test_hex_empty_string);
    RUN_TEST(test_hex_valid_with_expected_len);
    RUN_TEST(test_hex_wrong_length);
    
    // Speed parsing tests
    RUN_TEST(test_speed_parse_zero);
    RUN_TEST(test_speed_parse_60kph);
    RUN_TEST(test_speed_parse_100kph);
    RUN_TEST(test_speed_parse_max_255);
    RUN_TEST(test_speed_parse_lowercase);
    RUN_TEST(test_speed_parse_with_prefix);
    RUN_TEST(test_speed_parse_with_suffix);
    RUN_TEST(test_speed_parse_missing_header);
    RUN_TEST(test_speed_parse_incomplete);
    RUN_TEST(test_speed_parse_invalid_hex);
    
    // RPM parsing tests
    RUN_TEST(test_rpm_parse_idle);
    RUN_TEST(test_rpm_parse_zero);
    RUN_TEST(test_rpm_parse_3000);
    RUN_TEST(test_rpm_parse_max);
    RUN_TEST(test_rpm_parse_lowercase);
    RUN_TEST(test_rpm_parse_with_prefix);
    RUN_TEST(test_rpm_parse_missing_header);
    RUN_TEST(test_rpm_parse_incomplete);
    
    // Voltage parsing tests
    RUN_TEST(test_voltage_parse_normal);
    RUN_TEST(test_voltage_parse_low);
    RUN_TEST(test_voltage_parse_high);
    RUN_TEST(test_voltage_parse_zero_fails);
    RUN_TEST(test_voltage_parse_too_high_fails);
    RUN_TEST(test_voltage_parse_negative_fails);
    
    // Backoff calculation tests
    RUN_TEST(test_retry_delay_first_failure);
    RUN_TEST(test_retry_delay_second_failure);
    RUN_TEST(test_retry_delay_third_failure);
    RUN_TEST(test_retry_delay_fourth_failure);
    RUN_TEST(test_retry_delay_fifth_failure_capped);
    RUN_TEST(test_retry_delay_many_failures_capped);
    
    // Give up check tests
    RUN_TEST(test_give_up_at_zero);
    RUN_TEST(test_give_up_at_four);
    RUN_TEST(test_give_up_at_five);
    RUN_TEST(test_give_up_beyond_five);
    
    // State enum tests
    RUN_TEST(test_obd_state_enum_values);
    RUN_TEST(test_obd_state_strings);
    RUN_TEST(test_obd_state_unknown);
    
    return UNITY_END();
}

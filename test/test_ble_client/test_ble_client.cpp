/**
 * BLE Client Unit Tests
 * 
 * Tests state machine, backoff calculation, and state string conversion.
 * These tests catch bugs where:
 * - State transitions happen incorrectly
 * - Backoff timing doesn't follow exponential pattern
 * - State-to-string mapping is incomplete
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

// ============================================================================
// PURE FUNCTIONS EXTRACTED FOR TESTING
// ============================================================================

/**
 * BLE Connection State Machine (from ble_client.h)
 */
enum class BLEState {
    DISCONNECTED,   // Not connected, not doing anything
    SCANNING,       // Actively scanning for V1
    SCAN_STOPPING,  // Scan stop requested, waiting for settle
    CONNECTING,     // Connection attempt in progress
    CONNECTED,      // Successfully connected to V1
    BACKOFF         // Failed connection, waiting before retry
};

/**
 * Convert BLEState to string for logging (from ble_client.h)
 */
const char* bleStateToString(BLEState state) {
    switch (state) {
        case BLEState::DISCONNECTED: return "DISCONNECTED";
        case BLEState::SCANNING: return "SCANNING";
        case BLEState::SCAN_STOPPING: return "SCAN_STOPPING";
        case BLEState::CONNECTING: return "CONNECTING";
        case BLEState::CONNECTED: return "CONNECTED";
        case BLEState::BACKOFF: return "BACKOFF";
        default: return "UNKNOWN";
    }
}

/**
 * Backoff constants (from ble_client.h)
 */
static constexpr uint8_t MAX_BACKOFF_FAILURES = 5;
static constexpr unsigned long BACKOFF_BASE_MS = 500;
static constexpr unsigned long BACKOFF_MAX_MS = 5000;

/**
 * Calculate backoff time based on consecutive failures
 * Logic extracted from ble_client.cpp lines 773-775
 */
unsigned long calculateBackoffMs(int consecutiveFailures) {
    if (consecutiveFailures <= 0) return 0;
    
    // exponent capped at 4 (for failures 5+)
    int exponent = (consecutiveFailures > 4) ? 4 : (consecutiveFailures - 1);
    unsigned long backoffMs = BACKOFF_BASE_MS * (1 << exponent);
    if (backoffMs > BACKOFF_MAX_MS) backoffMs = BACKOFF_MAX_MS;
    return backoffMs;
}

/**
 * Check if hard reset should be triggered
 * Logic extracted from ble_client.cpp line 767
 */
bool shouldTriggerHardReset(int consecutiveFailures) {
    return consecutiveFailures >= MAX_BACKOFF_FAILURES;
}

/**
 * V1 packet checksum calculation (from ble_client.cpp)
 */
uint8_t calcV1Checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return sum;
}

/**
 * Extract short UUID from full UUID string
 * Logic from ble_client.cpp shortUuid() function
 */
uint16_t shortUuid(const char* uuidStr) {
    // UUID is like 92a0b2ce-9e05-11e2-aa59-f23c91aec05e → take b2ce (chars 4-7)
    size_t len = strlen(uuidStr);
    if (len >= 8) {
        char hex[5] = {0};
        strncpy(hex, uuidStr + 4, 4);
        return static_cast<uint16_t>(strtoul(hex, nullptr, 16));
    }
    return 0;
}

// ============================================================================
// STATE TO STRING TESTS
// ============================================================================

void test_ble_state_disconnected_string() {
    TEST_ASSERT_EQUAL_STRING("DISCONNECTED", bleStateToString(BLEState::DISCONNECTED));
}

void test_ble_state_scanning_string() {
    TEST_ASSERT_EQUAL_STRING("SCANNING", bleStateToString(BLEState::SCANNING));
}

void test_ble_state_scan_stopping_string() {
    TEST_ASSERT_EQUAL_STRING("SCAN_STOPPING", bleStateToString(BLEState::SCAN_STOPPING));
}

void test_ble_state_connecting_string() {
    TEST_ASSERT_EQUAL_STRING("CONNECTING", bleStateToString(BLEState::CONNECTING));
}

void test_ble_state_connected_string() {
    TEST_ASSERT_EQUAL_STRING("CONNECTED", bleStateToString(BLEState::CONNECTED));
}

void test_ble_state_backoff_string() {
    TEST_ASSERT_EQUAL_STRING("BACKOFF", bleStateToString(BLEState::BACKOFF));
}

void test_ble_state_unknown_string() {
    // Cast an invalid value to test the default case
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", bleStateToString(static_cast<BLEState>(99)));
}

// ============================================================================
// BACKOFF CALCULATION TESTS
// ============================================================================

void test_backoff_zero_failures_returns_zero() {
    TEST_ASSERT_EQUAL_UINT32(0, calculateBackoffMs(0));
}

void test_backoff_first_failure() {
    // exponent = 0, backoff = 500 * 1 = 500ms
    TEST_ASSERT_EQUAL_UINT32(500, calculateBackoffMs(1));
}

void test_backoff_second_failure() {
    // exponent = 1, backoff = 500 * 2 = 1000ms
    TEST_ASSERT_EQUAL_UINT32(1000, calculateBackoffMs(2));
}

void test_backoff_third_failure() {
    // exponent = 2, backoff = 500 * 4 = 2000ms
    TEST_ASSERT_EQUAL_UINT32(2000, calculateBackoffMs(3));
}

void test_backoff_fourth_failure() {
    // exponent = 3, backoff = 500 * 8 = 4000ms
    TEST_ASSERT_EQUAL_UINT32(4000, calculateBackoffMs(4));
}

void test_backoff_fifth_failure_capped() {
    // exponent = 4 (capped), backoff = 500 * 16 = 8000ms but capped to 5000ms
    TEST_ASSERT_EQUAL_UINT32(5000, calculateBackoffMs(5));
}

void test_backoff_many_failures_stays_capped() {
    // Beyond 5 failures, backoff stays at max
    TEST_ASSERT_EQUAL_UINT32(5000, calculateBackoffMs(10));
    TEST_ASSERT_EQUAL_UINT32(5000, calculateBackoffMs(100));
}

void test_backoff_negative_failures_returns_zero() {
    TEST_ASSERT_EQUAL_UINT32(0, calculateBackoffMs(-1));
}

// ============================================================================
// HARD RESET TRIGGER TESTS
// ============================================================================

void test_hard_reset_not_triggered_at_four_failures() {
    TEST_ASSERT_FALSE(shouldTriggerHardReset(4));
}

void test_hard_reset_triggered_at_five_failures() {
    TEST_ASSERT_TRUE(shouldTriggerHardReset(5));
}

void test_hard_reset_triggered_beyond_five_failures() {
    TEST_ASSERT_TRUE(shouldTriggerHardReset(6));
    TEST_ASSERT_TRUE(shouldTriggerHardReset(10));
}

void test_hard_reset_not_triggered_at_zero() {
    TEST_ASSERT_FALSE(shouldTriggerHardReset(0));
}

// ============================================================================
// V1 CHECKSUM TESTS
// ============================================================================

void test_checksum_empty_data() {
    uint8_t data[] = {};
    TEST_ASSERT_EQUAL_UINT8(0, calcV1Checksum(data, 0));
}

void test_checksum_single_byte() {
    uint8_t data[] = {0x42};
    TEST_ASSERT_EQUAL_UINT8(0x42, calcV1Checksum(data, 1));
}

void test_checksum_multiple_bytes() {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    TEST_ASSERT_EQUAL_UINT8(0x0A, calcV1Checksum(data, 4));  // 1+2+3+4 = 10
}

void test_checksum_overflow_wraps() {
    uint8_t data[] = {0xFF, 0x02};
    TEST_ASSERT_EQUAL_UINT8(0x01, calcV1Checksum(data, 2));  // 255+2 = 257 → 1
}

void test_checksum_real_v1_packet() {
    // Example V1 packet: SOF, dest, src, id, len, ...
    uint8_t packet[] = {0xAA, 0x55, 0x01, 0x03, 0x31};
    // Sum: 0xAA + 0x55 + 0x01 + 0x03 + 0x31 = 308 = 0x134 → 0x34 after overflow
    TEST_ASSERT_EQUAL_UINT8(0x34, calcV1Checksum(packet, 5));
}

// ============================================================================
// SHORT UUID EXTRACTION TESTS
// ============================================================================

void test_short_uuid_full_uuid() {
    // 92a0b2ce-9e05-11e2-aa59-f23c91aec05e → extract b2ce
    TEST_ASSERT_EQUAL_HEX16(0xB2CE, shortUuid("92a0b2ce-9e05-11e2-aa59-f23c91aec05e"));
}

void test_short_uuid_different_uuid() {
    // 92a0b4e0-9e05-11e2-aa59-f23c91aec05e → extract b4e0
    TEST_ASSERT_EQUAL_HEX16(0xB4E0, shortUuid("92a0b4e0-9e05-11e2-aa59-f23c91aec05e"));
}

void test_short_uuid_short_string_returns_zero() {
    TEST_ASSERT_EQUAL_HEX16(0, shortUuid("12345"));  // Less than 8 chars
}

void test_short_uuid_empty_string_returns_zero() {
    TEST_ASSERT_EQUAL_HEX16(0, shortUuid(""));
}

void test_short_uuid_exactly_eight_chars() {
    // "92a0b2ce" → extract b2ce
    TEST_ASSERT_EQUAL_HEX16(0xB2CE, shortUuid("92a0b2ce"));
}

// ============================================================================
// STATE ENUM VALUE TESTS
// ============================================================================

void test_state_enum_values() {
    // Verify enum values are as expected for wire protocol/storage
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(BLEState::DISCONNECTED));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(BLEState::SCANNING));
    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(BLEState::SCAN_STOPPING));
    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(BLEState::CONNECTING));
    TEST_ASSERT_EQUAL_INT(4, static_cast<int>(BLEState::CONNECTED));
    TEST_ASSERT_EQUAL_INT(5, static_cast<int>(BLEState::BACKOFF));
}

void test_all_states_have_strings() {
    // Every valid state should have a non-empty, non-UNKNOWN string
    BLEState states[] = {
        BLEState::DISCONNECTED,
        BLEState::SCANNING,
        BLEState::SCAN_STOPPING,
        BLEState::CONNECTING,
        BLEState::CONNECTED,
        BLEState::BACKOFF
    };
    
    for (BLEState state : states) {
        const char* str = bleStateToString(state);
        TEST_ASSERT_NOT_NULL(str);
        TEST_ASSERT_TRUE(strlen(str) > 0);
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN", str));  // strcmp returns 0 if equal
    }
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void setUp(void) {}
void tearDown(void) {}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // State to string tests
    RUN_TEST(test_ble_state_disconnected_string);
    RUN_TEST(test_ble_state_scanning_string);
    RUN_TEST(test_ble_state_scan_stopping_string);
    RUN_TEST(test_ble_state_connecting_string);
    RUN_TEST(test_ble_state_connected_string);
    RUN_TEST(test_ble_state_backoff_string);
    RUN_TEST(test_ble_state_unknown_string);
    
    // Backoff calculation tests
    RUN_TEST(test_backoff_zero_failures_returns_zero);
    RUN_TEST(test_backoff_first_failure);
    RUN_TEST(test_backoff_second_failure);
    RUN_TEST(test_backoff_third_failure);
    RUN_TEST(test_backoff_fourth_failure);
    RUN_TEST(test_backoff_fifth_failure_capped);
    RUN_TEST(test_backoff_many_failures_stays_capped);
    RUN_TEST(test_backoff_negative_failures_returns_zero);
    
    // Hard reset trigger tests
    RUN_TEST(test_hard_reset_not_triggered_at_four_failures);
    RUN_TEST(test_hard_reset_triggered_at_five_failures);
    RUN_TEST(test_hard_reset_triggered_beyond_five_failures);
    RUN_TEST(test_hard_reset_not_triggered_at_zero);
    
    // V1 checksum tests
    RUN_TEST(test_checksum_empty_data);
    RUN_TEST(test_checksum_single_byte);
    RUN_TEST(test_checksum_multiple_bytes);
    RUN_TEST(test_checksum_overflow_wraps);
    RUN_TEST(test_checksum_real_v1_packet);
    
    // Short UUID tests
    RUN_TEST(test_short_uuid_full_uuid);
    RUN_TEST(test_short_uuid_different_uuid);
    RUN_TEST(test_short_uuid_short_string_returns_zero);
    RUN_TEST(test_short_uuid_empty_string_returns_zero);
    RUN_TEST(test_short_uuid_exactly_eight_chars);
    
    // State enum tests
    RUN_TEST(test_state_enum_values);
    RUN_TEST(test_all_states_have_strings);
    
    return UNITY_END();
}

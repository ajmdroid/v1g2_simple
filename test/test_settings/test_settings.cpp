/**
 * Settings Manager Unit Tests
 * 
 * Tests bounds validation, namespace toggling, and XOR obfuscation.
 * These tests catch bugs where:
 * - Out-of-bounds values crash the system
 * - WiFi mode derivation fails
 * - Password obfuscation doesn't round-trip
 */

#include <unity.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <cstdint>
#include <cstring>
#include <algorithm>

// ============================================================================
// PURE FUNCTIONS EXTRACTED FOR TESTING
// ============================================================================

/**
 * Clamp value to range [min, max]
 * Same pattern as std::clamp used in settings.cpp
 */
template<typename T>
T clampValue(T value, T minVal, T maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}

/**
 * WiFi mode derivation logic from settings.cpp line 316
 * Mode is derived from wifiClientEnabled, not stored directly
 */
enum WiFiMode {
    V1_WIFI_AP = 1,
    V1_WIFI_APSTA = 3
};

enum LockoutRuntimeMode {
    LOCKOUT_RUNTIME_OFF = 0,
    LOCKOUT_RUNTIME_SHADOW = 1,
    LOCKOUT_RUNTIME_ADVISORY = 2,
    LOCKOUT_RUNTIME_ENFORCE = 3
};

static constexpr uint16_t CAMERA_ALERT_RANGE_M_DEFAULT = 805;
static constexpr uint16_t CAMERA_ALERT_RANGE_M_MIN = 50;
static constexpr uint16_t CAMERA_ALERT_RANGE_M_MAX = 5000;

WiFiMode deriveWifiMode(bool wifiClientEnabled) {
    return wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;
}

/**
 * XOR obfuscation for WiFi passwords (from settings.cpp)
 * Simple obfuscation to avoid plain text in NVS
 */
static const uint8_t XOR_KEY[] = {0x3A, 0x7B, 0x1D, 0xF2, 0x9E, 0x4C, 0x8A, 0x65};
static const size_t XOR_KEY_LEN = sizeof(XOR_KEY);

void xorObfuscate(char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        data[i] ^= XOR_KEY[i % XOR_KEY_LEN];
    }
}

/**
 * Slot index clamping (0-2)
 */
uint8_t clampSlotIndex(uint8_t slot) {
    return clampValue<uint8_t>(slot, 0, 2);
}

LockoutRuntimeMode clampLockoutRuntimeMode(int raw) {
    return static_cast<LockoutRuntimeMode>(clampValue<int>(raw, 0, 3));
}

uint16_t clampGuardDropThreshold(int raw) {
    return static_cast<uint16_t>(clampValue<int>(raw, 0, 65535));
}

uint16_t clampCameraAlertRangeMValue(int raw) {
    return static_cast<uint16_t>(
        clampValue<int>(raw, CAMERA_ALERT_RANGE_M_MIN, CAMERA_ALERT_RANGE_M_MAX));
}

/**
 * Namespace toggle (A/B pattern from settings.cpp)
 */
const char* toggleNamespace(const char* current) {
    if (strcmp(current, "v1settingsA") == 0) {
        return "v1settingsB";
    }
    return "v1settingsA";
}

// ============================================================================
// TESTS: Bounds Clamping
// ============================================================================

void test_clamp_brightness_minimum_is_1() {
    // Brightness 0 would turn off display - clamp to 1
    TEST_ASSERT_EQUAL(1, clampValue<uint8_t>(0, 1, 255));
    TEST_ASSERT_EQUAL(1, clampValue<uint8_t>(1, 1, 255));
    TEST_ASSERT_EQUAL(128, clampValue<uint8_t>(128, 1, 255));
    TEST_ASSERT_EQUAL(255, clampValue<uint8_t>(255, 1, 255));
}

void test_clamp_alertVolumeFadeDelaySec_1_to_10() {
    TEST_ASSERT_EQUAL(1, clampValue<uint8_t>(0, 1, 10));
    TEST_ASSERT_EQUAL(1, clampValue<uint8_t>(1, 1, 10));
    TEST_ASSERT_EQUAL(5, clampValue<uint8_t>(5, 1, 10));
    TEST_ASSERT_EQUAL(10, clampValue<uint8_t>(10, 1, 10));
    TEST_ASSERT_EQUAL(10, clampValue<uint8_t>(99, 1, 10));
}

void test_clamp_voiceVolume_0_to_100() {
    TEST_ASSERT_EQUAL(0, clampValue<uint8_t>(0, 0, 100));
    TEST_ASSERT_EQUAL(50, clampValue<uint8_t>(50, 0, 100));
    TEST_ASSERT_EQUAL(100, clampValue<uint8_t>(100, 0, 100));
    TEST_ASSERT_EQUAL(100, clampValue<uint8_t>(200, 0, 100));
}

void test_clamp_volume_0_to_9() {
    TEST_ASSERT_EQUAL(0, clampValue<uint8_t>(0, 0, 9));
    TEST_ASSERT_EQUAL(5, clampValue<uint8_t>(5, 0, 9));
    TEST_ASSERT_EQUAL(9, clampValue<uint8_t>(9, 0, 9));
    TEST_ASSERT_EQUAL(9, clampValue<uint8_t>(15, 0, 9));
}

void test_clamp_lockout_runtime_mode_0_to_3() {
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_OFF, clampLockoutRuntimeMode(-10));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_OFF, clampLockoutRuntimeMode(0));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_SHADOW, clampLockoutRuntimeMode(1));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_ADVISORY, clampLockoutRuntimeMode(2));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_ENFORCE, clampLockoutRuntimeMode(3));
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_ENFORCE, clampLockoutRuntimeMode(99));
}

void test_clamp_guard_drop_threshold_0_to_65535() {
    TEST_ASSERT_EQUAL(0, clampGuardDropThreshold(-1));
    TEST_ASSERT_EQUAL(0, clampGuardDropThreshold(0));
    TEST_ASSERT_EQUAL(42, clampGuardDropThreshold(42));
    TEST_ASSERT_EQUAL(65535, clampGuardDropThreshold(65535));
    TEST_ASSERT_EQUAL(65535, clampGuardDropThreshold(70000));
}

void test_camera_alert_range_clamp_50_to_5000() {
    TEST_ASSERT_EQUAL(50, clampCameraAlertRangeMValue(-10));
    TEST_ASSERT_EQUAL(50, clampCameraAlertRangeMValue(0));
    TEST_ASSERT_EQUAL(50, clampCameraAlertRangeMValue(50));
    TEST_ASSERT_EQUAL(805, clampCameraAlertRangeMValue(805));
    TEST_ASSERT_EQUAL(5000, clampCameraAlertRangeMValue(5000));
    TEST_ASSERT_EQUAL(5000, clampCameraAlertRangeMValue(65000));
}

void test_camera_setting_defaults_match_contract() {
    TEST_ASSERT_EQUAL_UINT16(805, CAMERA_ALERT_RANGE_M_DEFAULT);
    TEST_ASSERT_EQUAL_UINT16(50, CAMERA_ALERT_RANGE_M_MIN);
    TEST_ASSERT_EQUAL_UINT16(5000, CAMERA_ALERT_RANGE_M_MAX);
}

// ============================================================================
// TESTS: Slot Index Clamping
// ============================================================================

void test_clampSlotIndex_valid_values() {
    TEST_ASSERT_EQUAL(0, clampSlotIndex(0));
    TEST_ASSERT_EQUAL(1, clampSlotIndex(1));
    TEST_ASSERT_EQUAL(2, clampSlotIndex(2));
}

void test_clampSlotIndex_overflow() {
    TEST_ASSERT_EQUAL(2, clampSlotIndex(3));
    TEST_ASSERT_EQUAL(2, clampSlotIndex(100));
    TEST_ASSERT_EQUAL(2, clampSlotIndex(255));
}

// ============================================================================
// TESTS: WiFi Mode Derivation
// ============================================================================

void test_wifiMode_derived_from_clientEnabled_true() {
    WiFiMode mode = deriveWifiMode(true);
    TEST_ASSERT_EQUAL(V1_WIFI_APSTA, mode);
}

void test_wifiMode_derived_from_clientEnabled_false() {
    WiFiMode mode = deriveWifiMode(false);
    TEST_ASSERT_EQUAL(V1_WIFI_AP, mode);
}

// ============================================================================
// TESTS: Namespace Toggle
// ============================================================================

void test_namespace_toggle_a_to_b() {
    const char* result = toggleNamespace("v1settingsA");
    TEST_ASSERT_EQUAL_STRING("v1settingsB", result);
}

void test_namespace_toggle_b_to_a() {
    const char* result = toggleNamespace("v1settingsB");
    TEST_ASSERT_EQUAL_STRING("v1settingsA", result);
}

void test_namespace_toggle_unknown_defaults_to_a() {
    // Unknown namespace defaults to A
    const char* result = toggleNamespace("unknown");
    TEST_ASSERT_EQUAL_STRING("v1settingsA", result);
}

// ============================================================================
// TESTS: XOR Obfuscation
// ============================================================================

void test_xor_obfuscate_roundtrip() {
    char password[32] = "MySecretPass123";
    char original[32];
    strcpy(original, password);
    
    // First XOR obfuscates
    xorObfuscate(password, strlen(password));
    TEST_ASSERT_TRUE(strcmp(password, original) != 0);  // Should be different
    
    // Second XOR de-obfuscates (XOR is self-inverse)
    xorObfuscate(password, strlen(password));
    TEST_ASSERT_EQUAL_STRING(original, password);
}

void test_xor_obfuscate_empty_string() {
    char password[32] = "";
    xorObfuscate(password, 0);
    TEST_ASSERT_EQUAL_STRING("", password);
}

void test_xor_obfuscate_single_char() {
    char password[32] = "A";
    char original[32];
    strcpy(original, password);
    
    xorObfuscate(password, 1);
    xorObfuscate(password, 1);
    TEST_ASSERT_EQUAL_STRING(original, password);
}

void test_xor_obfuscate_longer_than_key() {
    // Password longer than XOR_KEY (8 bytes)
    // NOTE: We must track length separately because XOR may create null bytes
    char password[32] = "LongPassword123";
    size_t len = strlen(password);
    char original[32];
    memcpy(original, password, len + 1);  // Include null terminator
    
    xorObfuscate(password, len);
    xorObfuscate(password, len);
    
    // Compare byte by byte since XOR may create/destroy null bytes
    TEST_ASSERT_EQUAL_MEMORY(original, password, len);
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void setUp(void) {}
void tearDown(void) {}

void runAllTests() {
    // Bounds clamping tests (4 tests)
    RUN_TEST(test_clamp_brightness_minimum_is_1);
    RUN_TEST(test_clamp_alertVolumeFadeDelaySec_1_to_10);
    RUN_TEST(test_clamp_voiceVolume_0_to_100);
    RUN_TEST(test_clamp_volume_0_to_9);
    RUN_TEST(test_clamp_lockout_runtime_mode_0_to_3);
    RUN_TEST(test_clamp_guard_drop_threshold_0_to_65535);
    RUN_TEST(test_camera_alert_range_clamp_50_to_5000);
    RUN_TEST(test_camera_setting_defaults_match_contract);
    
    // Slot index tests (2 tests)
    RUN_TEST(test_clampSlotIndex_valid_values);
    RUN_TEST(test_clampSlotIndex_overflow);
    
    // WiFi mode derivation tests (2 tests)
    RUN_TEST(test_wifiMode_derived_from_clientEnabled_true);
    RUN_TEST(test_wifiMode_derived_from_clientEnabled_false);
    
    // Namespace toggle tests (3 tests)
    RUN_TEST(test_namespace_toggle_a_to_b);
    RUN_TEST(test_namespace_toggle_b_to_a);
    RUN_TEST(test_namespace_toggle_unknown_defaults_to_a);
    
    // XOR obfuscation tests (4 tests)
    RUN_TEST(test_xor_obfuscate_roundtrip);
    RUN_TEST(test_xor_obfuscate_empty_string);
    RUN_TEST(test_xor_obfuscate_single_char);
    RUN_TEST(test_xor_obfuscate_longer_than_key);
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

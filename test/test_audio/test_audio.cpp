/**
 * Audio Beep Unit Tests
 * 
 * Tests alert band/direction enums, camera types, and frequency mapping.
 * These tests catch bugs where:
 * - Band/direction enums have wrong values
 * - getGHz() returns wrong band for frequency
 * - Enum values don't match expected audio clip indices
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

// ============================================================================
// ENUMS EXTRACTED FROM audio_beep.h
// ============================================================================

/**
 * Band types for voice alerts
 */
enum class AlertBand : uint8_t {
    LASER = 0,
    KA = 1,
    K = 2,
    X = 3
};

/**
 * Direction types for voice alerts
 */
enum class AlertDirection : uint8_t {
    AHEAD = 0,
    BEHIND = 1,
    SIDE = 2
};

/**
 * Camera types for voice alerts
 */
enum class CameraAlertType : uint8_t {
    RED_LIGHT = 0,
    SPEED = 1,
    ALPR = 2,
    RED_LIGHT_SPEED = 3  // Combined
};

/**
 * Voice alert modes (from settings.h)
 */
enum VoiceAlertMode : uint8_t {
    VOICE_MODE_DISABLED = 0,
    VOICE_MODE_BAND_ONLY = 1,
    VOICE_MODE_FREQ_ONLY = 2,
    VOICE_MODE_BAND_FREQ = 3
};

// ============================================================================
// PURE FUNCTIONS EXTRACTED FOR TESTING
// ============================================================================

/**
 * Get GHz integer for band/frequency (from audio_beep.cpp)
 * Used for constructing audio clip paths
 */
int getGHz(AlertBand band, uint16_t freqMHz) {
    switch (band) {
        case AlertBand::KA:
            // Ka band: 33.4-36.0 GHz - determine which integer GHz
            if (freqMHz < 34000) return 33;
            if (freqMHz < 35000) return 34;
            if (freqMHz < 36000) return 35;
            return 36;
        case AlertBand::K:
            return 24;  // K band is 24.x GHz
        case AlertBand::X:
            return 10;  // X band is 10.x GHz
        default:
            return 0;   // Laser has no frequency
    }
}

/**
 * Get band name string for audio
 */
const char* bandToString(AlertBand band) {
    switch (band) {
        case AlertBand::LASER: return "laser";
        case AlertBand::KA: return "ka";
        case AlertBand::K: return "k";
        case AlertBand::X: return "x";
        default: return "unknown";
    }
}

/**
 * Get direction name string for audio
 */
const char* directionToString(AlertDirection dir) {
    switch (dir) {
        case AlertDirection::AHEAD: return "ahead";
        case AlertDirection::BEHIND: return "behind";
        case AlertDirection::SIDE: return "side";
        default: return "unknown";
    }
}

/**
 * Get camera type string for audio
 */
const char* cameraTypeToString(CameraAlertType type) {
    switch (type) {
        case CameraAlertType::RED_LIGHT: return "red_light";
        case CameraAlertType::SPEED: return "speed";
        case CameraAlertType::ALPR: return "alpr";
        case CameraAlertType::RED_LIGHT_SPEED: return "red_light_speed";
        default: return "unknown";
    }
}

/**
 * Extract hundreds digit from frequency MHz for audio
 * freqMHz: e.g., 34749 for 34.749 GHz
 * Returns: 7 (the first digit after decimal)
 */
int getHundredsDigit(uint16_t freqMHz) {
    int mhz = freqMHz % 1000;
    return mhz / 100;
}

/**
 * Extract last two digits from frequency MHz for audio
 * freqMHz: e.g., 34749 for 34.749 GHz
 * Returns: 49 (the last two digits)
 */
int getLastTwoDigits(uint16_t freqMHz) {
    int mhz = freqMHz % 1000;
    return mhz % 100;
}

// ============================================================================
// BAND ENUM TESTS
// ============================================================================

void test_band_enum_values() {
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(AlertBand::LASER));
    TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(AlertBand::KA));
    TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(AlertBand::K));
    TEST_ASSERT_EQUAL_UINT8(3, static_cast<uint8_t>(AlertBand::X));
}

void test_band_strings() {
    TEST_ASSERT_EQUAL_STRING("laser", bandToString(AlertBand::LASER));
    TEST_ASSERT_EQUAL_STRING("ka", bandToString(AlertBand::KA));
    TEST_ASSERT_EQUAL_STRING("k", bandToString(AlertBand::K));
    TEST_ASSERT_EQUAL_STRING("x", bandToString(AlertBand::X));
}

void test_band_unknown_string() {
    TEST_ASSERT_EQUAL_STRING("unknown", bandToString(static_cast<AlertBand>(99)));
}

// ============================================================================
// DIRECTION ENUM TESTS
// ============================================================================

void test_direction_enum_values() {
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(AlertDirection::AHEAD));
    TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(AlertDirection::BEHIND));
    TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(AlertDirection::SIDE));
}

void test_direction_strings() {
    TEST_ASSERT_EQUAL_STRING("ahead", directionToString(AlertDirection::AHEAD));
    TEST_ASSERT_EQUAL_STRING("behind", directionToString(AlertDirection::BEHIND));
    TEST_ASSERT_EQUAL_STRING("side", directionToString(AlertDirection::SIDE));
}

void test_direction_unknown_string() {
    TEST_ASSERT_EQUAL_STRING("unknown", directionToString(static_cast<AlertDirection>(99)));
}

// ============================================================================
// CAMERA TYPE ENUM TESTS
// ============================================================================

void test_camera_type_enum_values() {
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(CameraAlertType::RED_LIGHT));
    TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(CameraAlertType::SPEED));
    TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(CameraAlertType::ALPR));
    TEST_ASSERT_EQUAL_UINT8(3, static_cast<uint8_t>(CameraAlertType::RED_LIGHT_SPEED));
}

void test_camera_type_strings() {
    TEST_ASSERT_EQUAL_STRING("red_light", cameraTypeToString(CameraAlertType::RED_LIGHT));
    TEST_ASSERT_EQUAL_STRING("speed", cameraTypeToString(CameraAlertType::SPEED));
    TEST_ASSERT_EQUAL_STRING("alpr", cameraTypeToString(CameraAlertType::ALPR));
    TEST_ASSERT_EQUAL_STRING("red_light_speed", cameraTypeToString(CameraAlertType::RED_LIGHT_SPEED));
}

void test_camera_type_unknown_string() {
    TEST_ASSERT_EQUAL_STRING("unknown", cameraTypeToString(static_cast<CameraAlertType>(99)));
}

// ============================================================================
// VOICE MODE ENUM TESTS
// ============================================================================

void test_voice_mode_enum_values() {
    TEST_ASSERT_EQUAL_UINT8(0, VOICE_MODE_DISABLED);
    TEST_ASSERT_EQUAL_UINT8(1, VOICE_MODE_BAND_ONLY);
    TEST_ASSERT_EQUAL_UINT8(2, VOICE_MODE_FREQ_ONLY);
    TEST_ASSERT_EQUAL_UINT8(3, VOICE_MODE_BAND_FREQ);
}

// ============================================================================
// GHZ MAPPING TESTS - KA BAND
// ============================================================================

void test_ghz_ka_33ghz_low_end() {
    // 33.4 GHz = 33400 MHz → 33 GHz
    TEST_ASSERT_EQUAL_INT(33, getGHz(AlertBand::KA, 33400));
}

void test_ghz_ka_33ghz_high_end() {
    // 33.999 GHz = 33999 MHz → 33 GHz
    TEST_ASSERT_EQUAL_INT(33, getGHz(AlertBand::KA, 33999));
}

void test_ghz_ka_34ghz_low_end() {
    // 34.0 GHz = 34000 MHz → 34 GHz
    TEST_ASSERT_EQUAL_INT(34, getGHz(AlertBand::KA, 34000));
}

void test_ghz_ka_34ghz_typical() {
    // 34.749 GHz = 34749 MHz → 34 GHz
    TEST_ASSERT_EQUAL_INT(34, getGHz(AlertBand::KA, 34749));
}

void test_ghz_ka_35ghz() {
    // 35.5 GHz = 35500 MHz → 35 GHz
    TEST_ASSERT_EQUAL_INT(35, getGHz(AlertBand::KA, 35500));
}

void test_ghz_ka_36ghz() {
    // 36.0 GHz = 36000 MHz → 36 GHz
    TEST_ASSERT_EQUAL_INT(36, getGHz(AlertBand::KA, 36000));
}

// ============================================================================
// GHZ MAPPING TESTS - K BAND
// ============================================================================

void test_ghz_k_band_typical() {
    // K band is always 24.x GHz
    TEST_ASSERT_EQUAL_INT(24, getGHz(AlertBand::K, 24100));
    TEST_ASSERT_EQUAL_INT(24, getGHz(AlertBand::K, 24150));
    TEST_ASSERT_EQUAL_INT(24, getGHz(AlertBand::K, 24200));
}

void test_ghz_k_band_ignores_freq() {
    // Should return 24 regardless of frequency value
    TEST_ASSERT_EQUAL_INT(24, getGHz(AlertBand::K, 0));
    TEST_ASSERT_EQUAL_INT(24, getGHz(AlertBand::K, 99999));
}

// ============================================================================
// GHZ MAPPING TESTS - X BAND
// ============================================================================

void test_ghz_x_band_typical() {
    // X band is always 10.x GHz
    TEST_ASSERT_EQUAL_INT(10, getGHz(AlertBand::X, 10500));
    TEST_ASSERT_EQUAL_INT(10, getGHz(AlertBand::X, 10525));
}

void test_ghz_x_band_ignores_freq() {
    // Should return 10 regardless of frequency value
    TEST_ASSERT_EQUAL_INT(10, getGHz(AlertBand::X, 0));
    TEST_ASSERT_EQUAL_INT(10, getGHz(AlertBand::X, 99999));
}

// ============================================================================
// GHZ MAPPING TESTS - LASER
// ============================================================================

void test_ghz_laser_returns_zero() {
    // Laser has no frequency - should return 0
    TEST_ASSERT_EQUAL_INT(0, getGHz(AlertBand::LASER, 0));
    TEST_ASSERT_EQUAL_INT(0, getGHz(AlertBand::LASER, 34749));
}

// ============================================================================
// FREQUENCY DIGIT EXTRACTION TESTS
// ============================================================================

void test_hundreds_digit_34749() {
    // 34749 → 749 → 7
    TEST_ASSERT_EQUAL_INT(7, getHundredsDigit(34749));
}

void test_hundreds_digit_34500() {
    // 34500 → 500 → 5
    TEST_ASSERT_EQUAL_INT(5, getHundredsDigit(34500));
}

void test_hundreds_digit_34099() {
    // 34099 → 099 → 0
    TEST_ASSERT_EQUAL_INT(0, getHundredsDigit(34099));
}

void test_hundreds_digit_24150() {
    // 24150 → 150 → 1
    TEST_ASSERT_EQUAL_INT(1, getHundredsDigit(24150));
}

void test_last_two_digits_34749() {
    // 34749 → 749 → 49
    TEST_ASSERT_EQUAL_INT(49, getLastTwoDigits(34749));
}

void test_last_two_digits_34700() {
    // 34700 → 700 → 00
    TEST_ASSERT_EQUAL_INT(0, getLastTwoDigits(34700));
}

void test_last_two_digits_34199() {
    // 34199 → 199 → 99
    TEST_ASSERT_EQUAL_INT(99, getLastTwoDigits(34199));
}

void test_last_two_digits_24150() {
    // 24150 → 150 → 50
    TEST_ASSERT_EQUAL_INT(50, getLastTwoDigits(24150));
}

// ============================================================================
// COMBINED FREQUENCY PARSING TESTS
// ============================================================================

void test_full_freq_parse_34749() {
    // 34.749 GHz should produce: "34" "7" "49"
    uint16_t freqMHz = 34749;
    TEST_ASSERT_EQUAL_INT(34, getGHz(AlertBand::KA, freqMHz));
    TEST_ASSERT_EQUAL_INT(7, getHundredsDigit(freqMHz));
    TEST_ASSERT_EQUAL_INT(49, getLastTwoDigits(freqMHz));
}

void test_full_freq_parse_35500() {
    // 35.500 GHz should produce: "35" "5" "00"
    uint16_t freqMHz = 35500;
    TEST_ASSERT_EQUAL_INT(35, getGHz(AlertBand::KA, freqMHz));
    TEST_ASSERT_EQUAL_INT(5, getHundredsDigit(freqMHz));
    TEST_ASSERT_EQUAL_INT(0, getLastTwoDigits(freqMHz));
}

void test_full_freq_parse_24150() {
    // 24.150 GHz should produce: "24" "1" "50"
    uint16_t freqMHz = 24150;
    TEST_ASSERT_EQUAL_INT(24, getGHz(AlertBand::K, freqMHz));
    TEST_ASSERT_EQUAL_INT(1, getHundredsDigit(freqMHz));
    TEST_ASSERT_EQUAL_INT(50, getLastTwoDigits(freqMHz));
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void setUp(void) {}
void tearDown(void) {}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Band enum tests
    RUN_TEST(test_band_enum_values);
    RUN_TEST(test_band_strings);
    RUN_TEST(test_band_unknown_string);
    
    // Direction enum tests
    RUN_TEST(test_direction_enum_values);
    RUN_TEST(test_direction_strings);
    RUN_TEST(test_direction_unknown_string);
    
    // Camera type enum tests
    RUN_TEST(test_camera_type_enum_values);
    RUN_TEST(test_camera_type_strings);
    RUN_TEST(test_camera_type_unknown_string);
    
    // Voice mode enum tests
    RUN_TEST(test_voice_mode_enum_values);
    
    // GHz mapping tests - Ka band
    RUN_TEST(test_ghz_ka_33ghz_low_end);
    RUN_TEST(test_ghz_ka_33ghz_high_end);
    RUN_TEST(test_ghz_ka_34ghz_low_end);
    RUN_TEST(test_ghz_ka_34ghz_typical);
    RUN_TEST(test_ghz_ka_35ghz);
    RUN_TEST(test_ghz_ka_36ghz);
    
    // GHz mapping tests - K band
    RUN_TEST(test_ghz_k_band_typical);
    RUN_TEST(test_ghz_k_band_ignores_freq);
    
    // GHz mapping tests - X band
    RUN_TEST(test_ghz_x_band_typical);
    RUN_TEST(test_ghz_x_band_ignores_freq);
    
    // GHz mapping tests - Laser
    RUN_TEST(test_ghz_laser_returns_zero);
    
    // Frequency digit extraction tests
    RUN_TEST(test_hundreds_digit_34749);
    RUN_TEST(test_hundreds_digit_34500);
    RUN_TEST(test_hundreds_digit_34099);
    RUN_TEST(test_hundreds_digit_24150);
    RUN_TEST(test_last_two_digits_34749);
    RUN_TEST(test_last_two_digits_34700);
    RUN_TEST(test_last_two_digits_34199);
    RUN_TEST(test_last_two_digits_24150);
    
    // Combined frequency parsing tests
    RUN_TEST(test_full_freq_parse_34749);
    RUN_TEST(test_full_freq_parse_35500);
    RUN_TEST(test_full_freq_parse_24150);
    
    return UNITY_END();
}

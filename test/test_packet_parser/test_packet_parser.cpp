/**
 * test_packet_parser.cpp - Unit tests for V1 packet parsing
 * 
 * Tests the critical path: V1 BLE packets → parsed display state + alerts.
 * Any bug here directly affects what the user sees on screen.
 */
#include <unity.h>
#include <cstdint>
#include <cstring>
#include <array>

// ============================================================================
// Inline implementations from packet_parser.cpp (for isolated testing)
// ============================================================================

// Packet framing constants
constexpr uint8_t ESP_PACKET_START = 0xAA;
constexpr uint8_t ESP_PACKET_END = 0xAB;
constexpr uint8_t PACKET_ID_DISPLAY_DATA = 0x31;
constexpr uint8_t PACKET_ID_ALERT_DATA = 0x43;

// Band constants
enum Band {
    BAND_NONE   = 0,
    BAND_LASER  = 1 << 0,
    BAND_KA     = 1 << 1,
    BAND_K      = 1 << 2,
    BAND_X      = 1 << 3
};

// Direction constants
enum Direction {
    DIR_NONE    = 0,
    DIR_FRONT   = 1,
    DIR_SIDE    = 2,
    DIR_REAR    = 4
};

// Decode band from V1 bandArrow byte (bits 0-3)
static Band decodeBand(uint8_t bandArrow) {
    if (bandArrow & 0x01) return BAND_LASER;
    if (bandArrow & 0x02) return BAND_KA;
    if (bandArrow & 0x04) return BAND_K;
    if (bandArrow & 0x08) return BAND_X;
    return BAND_NONE;
}

// Decode direction from V1 bandArrow byte (bits 5-7)
static Direction decodeDirection(uint8_t bandArrow) {
    int dir = DIR_NONE;
    if (bandArrow & 0x20) dir |= DIR_FRONT;
    if (bandArrow & 0x40) dir |= DIR_SIDE;
    if (bandArrow & 0x80) dir |= DIR_REAR;
    return static_cast<Direction>(dir);
}

// Process band/arrow byte into structured data
struct BandArrowData {
    bool laser, ka, k, x;
    bool mute;
    bool front, side, rear;
};

static BandArrowData processBandArrow(uint8_t v) {
    BandArrowData d;
    d.laser = (v & 0x01) != 0;
    d.ka    = (v & 0x02) != 0;
    d.k     = (v & 0x04) != 0;
    d.x     = (v & 0x08) != 0;
    d.mute  = (v & 0x10) != 0;
    d.front = (v & 0x20) != 0;
    d.side  = (v & 0x40) != 0;
    d.rear  = (v & 0x80) != 0;
    return d;
}

// Decode 7-segment bogey counter byte
static char decodeBogeyCounterByte(uint8_t bogeyImage, bool& hasDot) {
    hasDot = (bogeyImage & 0x80) != 0;
    
    switch (bogeyImage & 0x7F) {
        case 6:   return '1';
        case 7:   return '7';
        case 24:  return '&';  // Little L (logic mode)
        case 28:  return 'u';
        case 30:  return 'J';  // Junk
        case 56:  return 'L';  // Logic
        case 57:  return 'C';
        case 62:  return 'U';
        case 63:  return '0';
        case 73:  return '#';  // LASER bars
        case 79:  return '3';
        case 88:  return 'c';
        case 91:  return '2';
        case 94:  return 'd';
        case 102: return '4';
        case 109: return '5';
        case 111: return '9';
        case 113: return 'F';
        case 115: return 'P';  // Photo radar
        case 119: return 'A';
        case 121: return 'E';
        case 124: return 'b';
        case 125: return '6';
        case 127: return '8';
        default:  return ' ';
    }
}

// Combine MSB/LSB into frequency
static uint16_t combineMSBLSB(uint8_t msb, uint8_t lsb) {
    return (static_cast<uint16_t>(msb) << 8) | lsb;
}

// Basic packet validation
static bool validatePacket(const uint8_t* data, size_t length) {
    if (length < 8) return false;
    if (data[0] != ESP_PACKET_START) return false;
    if (data[length - 1] != ESP_PACKET_END) return false;
    return true;
}

// ============================================================================
// Test Cases: Band Decoding
// ============================================================================

void test_decode_band_laser() {
    TEST_ASSERT_EQUAL(BAND_LASER, decodeBand(0x01));
}

void test_decode_band_ka() {
    TEST_ASSERT_EQUAL(BAND_KA, decodeBand(0x02));
}

void test_decode_band_k() {
    TEST_ASSERT_EQUAL(BAND_K, decodeBand(0x04));
}

void test_decode_band_x() {
    TEST_ASSERT_EQUAL(BAND_X, decodeBand(0x08));
}

void test_decode_band_none() {
    TEST_ASSERT_EQUAL(BAND_NONE, decodeBand(0x00));
    TEST_ASSERT_EQUAL(BAND_NONE, decodeBand(0xF0));  // Direction bits only
}

void test_decode_band_priority() {
    // When multiple bands present, priority: Laser > Ka > K > X
    TEST_ASSERT_EQUAL(BAND_LASER, decodeBand(0x0F));  // All bands
    TEST_ASSERT_EQUAL(BAND_KA, decodeBand(0x0E));     // Ka + K + X
    TEST_ASSERT_EQUAL(BAND_K, decodeBand(0x0C));      // K + X
}

// ============================================================================
// Test Cases: Direction Decoding
// ============================================================================

void test_decode_direction_front() {
    TEST_ASSERT_EQUAL(DIR_FRONT, decodeDirection(0x20));
}

void test_decode_direction_side() {
    TEST_ASSERT_EQUAL(DIR_SIDE, decodeDirection(0x40));
}

void test_decode_direction_rear() {
    TEST_ASSERT_EQUAL(DIR_REAR, decodeDirection(0x80));
}

void test_decode_direction_front_and_rear() {
    Direction dir = decodeDirection(0xA0);  // Front + Rear
    TEST_ASSERT_TRUE(dir & DIR_FRONT);
    TEST_ASSERT_TRUE(dir & DIR_REAR);
    TEST_ASSERT_FALSE(dir & DIR_SIDE);
}

void test_decode_direction_all() {
    Direction dir = decodeDirection(0xE0);  // All directions
    TEST_ASSERT_TRUE(dir & DIR_FRONT);
    TEST_ASSERT_TRUE(dir & DIR_SIDE);
    TEST_ASSERT_TRUE(dir & DIR_REAR);
}

void test_decode_direction_none() {
    TEST_ASSERT_EQUAL(DIR_NONE, decodeDirection(0x00));
    TEST_ASSERT_EQUAL(DIR_NONE, decodeDirection(0x1F));  // Band + mute bits only
}

// ============================================================================
// Test Cases: Combined Band/Arrow Processing
// ============================================================================

void test_process_band_arrow_ka_front() {
    // Ka band, front arrow: 0x02 | 0x20 = 0x22
    BandArrowData d = processBandArrow(0x22);
    TEST_ASSERT_TRUE(d.ka);
    TEST_ASSERT_TRUE(d.front);
    TEST_ASSERT_FALSE(d.laser);
    TEST_ASSERT_FALSE(d.k);
    TEST_ASSERT_FALSE(d.x);
    TEST_ASSERT_FALSE(d.mute);
    TEST_ASSERT_FALSE(d.side);
    TEST_ASSERT_FALSE(d.rear);
}

void test_process_band_arrow_k_rear_muted() {
    // K band, rear arrow, muted: 0x04 | 0x80 | 0x10 = 0x94
    BandArrowData d = processBandArrow(0x94);
    TEST_ASSERT_TRUE(d.k);
    TEST_ASSERT_TRUE(d.rear);
    TEST_ASSERT_TRUE(d.mute);
    TEST_ASSERT_FALSE(d.front);
}

// ============================================================================
// Test Cases: Bogey Counter (7-segment display)
// ============================================================================

void test_bogey_counter_digit_0() {
    bool dot;
    TEST_ASSERT_EQUAL('0', decodeBogeyCounterByte(63, dot));
    TEST_ASSERT_FALSE(dot);
}

void test_bogey_counter_digit_1_through_9() {
    bool dot;
    TEST_ASSERT_EQUAL('1', decodeBogeyCounterByte(6, dot));
    TEST_ASSERT_EQUAL('2', decodeBogeyCounterByte(91, dot));
    TEST_ASSERT_EQUAL('3', decodeBogeyCounterByte(79, dot));
    TEST_ASSERT_EQUAL('4', decodeBogeyCounterByte(102, dot));
    TEST_ASSERT_EQUAL('5', decodeBogeyCounterByte(109, dot));
    TEST_ASSERT_EQUAL('6', decodeBogeyCounterByte(125, dot));
    TEST_ASSERT_EQUAL('7', decodeBogeyCounterByte(7, dot));
    TEST_ASSERT_EQUAL('8', decodeBogeyCounterByte(127, dot));
    TEST_ASSERT_EQUAL('9', decodeBogeyCounterByte(111, dot));
}

void test_bogey_counter_junk() {
    bool dot;
    TEST_ASSERT_EQUAL('J', decodeBogeyCounterByte(30, dot));
}

void test_bogey_counter_photo() {
    bool dot;
    TEST_ASSERT_EQUAL('P', decodeBogeyCounterByte(115, dot));
}

void test_bogey_counter_with_decimal() {
    bool dot;
    // 127 = '8', with bit 7 set = decimal point
    TEST_ASSERT_EQUAL('8', decodeBogeyCounterByte(127 | 0x80, dot));
    TEST_ASSERT_TRUE(dot);
}

// ============================================================================
// Test Cases: Frequency Combining
// ============================================================================

void test_frequency_combine_ka_band() {
    // Ka band frequency example: 34.712 GHz = 34712 MHz
    // MSB = 0x87, LSB = 0x98 → 0x8798 = 34712
    uint16_t freq = combineMSBLSB(0x87, 0x98);
    TEST_ASSERT_EQUAL(34712, freq);
}

void test_frequency_combine_k_band() {
    // K band frequency example: 24.150 GHz = 24150 MHz
    // MSB = 0x5E, LSB = 0x56 → 0x5E56 = 24150
    uint16_t freq = combineMSBLSB(0x5E, 0x56);
    TEST_ASSERT_EQUAL(24150, freq);
}

void test_frequency_combine_x_band() {
    // X band frequency example: 10.525 GHz = 10525 MHz
    uint16_t freq = combineMSBLSB(0x29, 0x1D);
    TEST_ASSERT_EQUAL(10525, freq);
}

// ============================================================================
// Test Cases: Packet Validation
// ============================================================================

void test_validate_packet_valid() {
    uint8_t packet[] = {0xAA, 0x01, 0x02, 0x31, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAB};
    TEST_ASSERT_TRUE(validatePacket(packet, sizeof(packet)));
}

void test_validate_packet_wrong_start() {
    uint8_t packet[] = {0xBB, 0x01, 0x02, 0x31, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAB};
    TEST_ASSERT_FALSE(validatePacket(packet, sizeof(packet)));
}

void test_validate_packet_wrong_end() {
    uint8_t packet[] = {0xAA, 0x01, 0x02, 0x31, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAC};
    TEST_ASSERT_FALSE(validatePacket(packet, sizeof(packet)));
}

void test_validate_packet_too_short() {
    uint8_t packet[] = {0xAA, 0x01, 0x02, 0x31, 0xAB};
    TEST_ASSERT_FALSE(validatePacket(packet, sizeof(packet)));
}

// ============================================================================
// Test Cases: Frequency Tolerance (for lockout matching)
// ============================================================================

void test_frequency_tolerance_same() {
    uint16_t f1 = 34712;
    uint16_t f2 = 34712;
    int diff = abs((int)f1 - (int)f2);
    TEST_ASSERT_TRUE(diff <= 25);  // 25 MHz tolerance
}

void test_frequency_tolerance_within() {
    // 34.712 vs 34.720 = 8 MHz difference
    uint16_t f1 = 34712;
    uint16_t f2 = 34720;
    int diff = abs((int)f1 - (int)f2);
    TEST_ASSERT_TRUE(diff <= 25);
}

void test_frequency_tolerance_exceeded() {
    // 34.712 vs 34.800 = 88 MHz difference (different source)
    uint16_t f1 = 34712;
    uint16_t f2 = 34800;
    int diff = abs((int)f1 - (int)f2);
    TEST_ASSERT_FALSE(diff <= 25);
}

void test_frequency_tolerance_door_opener_vs_speed_sign() {
    // Classic false positive case:
    // Door opener at 24.150 GHz vs speed sign at 24.125 GHz = 25 MHz
    uint16_t doorOpener = 24150;
    uint16_t speedSign = 24125;
    int diff = abs((int)doorOpener - (int)speedSign);
    TEST_ASSERT_EQUAL(25, diff);
    // At exactly 25 MHz, this is borderline - should be separate lockouts
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Band decoding
    RUN_TEST(test_decode_band_laser);
    RUN_TEST(test_decode_band_ka);
    RUN_TEST(test_decode_band_k);
    RUN_TEST(test_decode_band_x);
    RUN_TEST(test_decode_band_none);
    RUN_TEST(test_decode_band_priority);
    
    // Direction decoding
    RUN_TEST(test_decode_direction_front);
    RUN_TEST(test_decode_direction_side);
    RUN_TEST(test_decode_direction_rear);
    RUN_TEST(test_decode_direction_front_and_rear);
    RUN_TEST(test_decode_direction_all);
    RUN_TEST(test_decode_direction_none);
    
    // Combined processing
    RUN_TEST(test_process_band_arrow_ka_front);
    RUN_TEST(test_process_band_arrow_k_rear_muted);
    
    // Bogey counter display
    RUN_TEST(test_bogey_counter_digit_0);
    RUN_TEST(test_bogey_counter_digit_1_through_9);
    RUN_TEST(test_bogey_counter_junk);
    RUN_TEST(test_bogey_counter_photo);
    RUN_TEST(test_bogey_counter_with_decimal);
    
    // Frequency handling
    RUN_TEST(test_frequency_combine_ka_band);
    RUN_TEST(test_frequency_combine_k_band);
    RUN_TEST(test_frequency_combine_x_band);
    
    // Packet validation
    RUN_TEST(test_validate_packet_valid);
    RUN_TEST(test_validate_packet_wrong_start);
    RUN_TEST(test_validate_packet_wrong_end);
    RUN_TEST(test_validate_packet_too_short);
    
    // Frequency tolerance (lockout matching)
    RUN_TEST(test_frequency_tolerance_same);
    RUN_TEST(test_frequency_tolerance_within);
    RUN_TEST(test_frequency_tolerance_exceeded);
    RUN_TEST(test_frequency_tolerance_door_opener_vs_speed_sign);
    
    return UNITY_END();
}

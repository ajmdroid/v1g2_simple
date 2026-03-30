// Tests for pure-logic helper functions in time_service.cpp:
//   - isValidUnixMs(int64_t)
//   - clampTzOffset(int32_t)
//   - parseInt64Strict(const String&, int64_t&)
//   - normalizeSource(uint8_t&)
//   - hasMaterialPersistChange(...)
//
// Hardware-dependent paths (TimeService::begin, setEpochBaseMs, persistCurrentTime,
// periodicSave) compile via mocks but are not called from these tests.

#include <unity.h>

#include "../mocks/Arduino.h"
#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../mocks/Preferences.h"

#include "../../src/time_service.cpp"

// -----------------------------------------------------------------------
// isValidUnixMs
// -----------------------------------------------------------------------

// MIN_VALID_UNIX_MS ~= 2023-11, MAX_VALID_UNIX_MS = 2100-01-01
static constexpr int64_t VALID_EPOCH_MS = 1750000000000LL;  // 2025 — comfortably valid

void test_is_valid_unix_ms_returns_true_for_mid_range() {
    TEST_ASSERT_TRUE(isValidUnixMs(VALID_EPOCH_MS));
}

void test_is_valid_unix_ms_returns_false_for_zero() {
    TEST_ASSERT_FALSE(isValidUnixMs(0LL));
}

void test_is_valid_unix_ms_returns_false_for_negative() {
    TEST_ASSERT_FALSE(isValidUnixMs(-1LL));
}

void test_is_valid_unix_ms_returns_false_for_too_old() {
    // Just below MIN_VALID_UNIX_MS (1700000000000)
    TEST_ASSERT_FALSE(isValidUnixMs(1699999999999LL));
}

void test_is_valid_unix_ms_returns_true_at_min_boundary() {
    TEST_ASSERT_TRUE(isValidUnixMs(1700000000000LL));
}

void test_is_valid_unix_ms_returns_true_at_max_boundary() {
    TEST_ASSERT_TRUE(isValidUnixMs(4102444800000LL));
}

void test_is_valid_unix_ms_returns_false_above_max() {
    TEST_ASSERT_FALSE(isValidUnixMs(4102444800001LL));
}

// -----------------------------------------------------------------------
// clampTzOffset
// -----------------------------------------------------------------------

void test_clamp_tz_in_range_unchanged() {
    TEST_ASSERT_EQUAL_INT32(0, clampTzOffset(0));
    TEST_ASSERT_EQUAL_INT32(60, clampTzOffset(60));
    TEST_ASSERT_EQUAL_INT32(-300, clampTzOffset(-300));
}

void test_clamp_tz_exactly_at_min() {
    TEST_ASSERT_EQUAL_INT32(-840, clampTzOffset(-840));
}

void test_clamp_tz_below_min_clamped() {
    TEST_ASSERT_EQUAL_INT32(-840, clampTzOffset(-841));
    TEST_ASSERT_EQUAL_INT32(-840, clampTzOffset(-9999));
}

void test_clamp_tz_exactly_at_max() {
    TEST_ASSERT_EQUAL_INT32(840, clampTzOffset(840));
}

void test_clamp_tz_above_max_clamped() {
    TEST_ASSERT_EQUAL_INT32(840, clampTzOffset(841));
    TEST_ASSERT_EQUAL_INT32(840, clampTzOffset(9999));
}

// -----------------------------------------------------------------------
// parseInt64Strict
// -----------------------------------------------------------------------

void test_parse_int64_valid_positive() {
    int64_t out = 0;
    TEST_ASSERT_TRUE(parseInt64Strict(String("12345"), out));
    TEST_ASSERT_EQUAL_INT64(12345LL, out);
}

void test_parse_int64_valid_negative() {
    int64_t out = 0;
    TEST_ASSERT_TRUE(parseInt64Strict(String("-99"), out));
    TEST_ASSERT_EQUAL_INT64(-99LL, out);
}

void test_parse_int64_zero() {
    int64_t out = -1;
    TEST_ASSERT_TRUE(parseInt64Strict(String("0"), out));
    TEST_ASSERT_EQUAL_INT64(0LL, out);
}

void test_parse_int64_valid_epoch_ms() {
    int64_t out = 0;
    TEST_ASSERT_TRUE(parseInt64Strict(String("1750000000000"), out));
    TEST_ASSERT_EQUAL_INT64(1750000000000LL, out);
}

void test_parse_int64_empty_string_fails() {
    int64_t out = 42;
    TEST_ASSERT_FALSE(parseInt64Strict(String(""), out));
}

void test_parse_int64_non_numeric_fails() {
    int64_t out = 42;
    TEST_ASSERT_FALSE(parseInt64Strict(String("abc"), out));
}

void test_parse_int64_trailing_chars_fails() {
    int64_t out = 42;
    TEST_ASSERT_FALSE(parseInt64Strict(String("123abc"), out));
}

// strtoll skips leading whitespace — document this accepted behavior
void test_parse_int64_leading_space_accepted() {
    int64_t out = 0;
    TEST_ASSERT_TRUE(parseInt64Strict(String(" 123"), out));
    TEST_ASSERT_EQUAL_INT64(123LL, out);
}

// -----------------------------------------------------------------------
// normalizeSource
// -----------------------------------------------------------------------

void test_normalize_source_none_becomes_client_ap() {
    uint8_t src = TimeService::SOURCE_NONE;
    TEST_ASSERT_TRUE(normalizeSource(src));
    TEST_ASSERT_EQUAL_UINT8(TimeService::SOURCE_CLIENT_AP, src);
}

void test_normalize_source_valid_client_ap_unchanged() {
    uint8_t src = TimeService::SOURCE_CLIENT_AP;
    TEST_ASSERT_TRUE(normalizeSource(src));
    TEST_ASSERT_EQUAL_UINT8(TimeService::SOURCE_CLIENT_AP, src);
}

void test_normalize_source_valid_gps_unchanged() {
    uint8_t src = TimeService::SOURCE_GPS;
    TEST_ASSERT_TRUE(normalizeSource(src));
    TEST_ASSERT_EQUAL_UINT8(TimeService::SOURCE_GPS, src);
}

void test_normalize_source_valid_rtc_unchanged() {
    uint8_t src = TimeService::SOURCE_RTC;
    TEST_ASSERT_TRUE(normalizeSource(src));
    TEST_ASSERT_EQUAL_UINT8(TimeService::SOURCE_RTC, src);
}

void test_normalize_source_out_of_range_fails() {
    uint8_t src = static_cast<uint8_t>(TimeService::SOURCE_RTC + 1);
    TEST_ASSERT_FALSE(normalizeSource(src));
}

// -----------------------------------------------------------------------
// hasMaterialPersistChange
// -----------------------------------------------------------------------

// No previous valid snapshot → always material
void test_material_change_no_previous_valid() {
    TEST_ASSERT_TRUE(hasMaterialPersistChange(
        false, 0LL, 0, TimeService::SOURCE_CLIENT_AP,
        VALID_EPOCH_MS, 0, TimeService::SOURCE_CLIENT_AP));
}

// Previous valid but epoch was invalid → material
void test_material_change_previous_epoch_invalid() {
    TEST_ASSERT_TRUE(hasMaterialPersistChange(
        true, 0LL, 0, TimeService::SOURCE_CLIENT_AP,
        VALID_EPOCH_MS, 0, TimeService::SOURCE_CLIENT_AP));
}

// TZ offset changed → material
void test_material_change_tz_changed() {
    TEST_ASSERT_TRUE(hasMaterialPersistChange(
        true, VALID_EPOCH_MS, 0, TimeService::SOURCE_CLIENT_AP,
        VALID_EPOCH_MS, 60, TimeService::SOURCE_CLIENT_AP));
}

// Source changed → material
void test_material_change_source_changed() {
    TEST_ASSERT_TRUE(hasMaterialPersistChange(
        true, VALID_EPOCH_MS, 0, TimeService::SOURCE_CLIENT_AP,
        VALID_EPOCH_MS, 0, TimeService::SOURCE_GPS));
}

// Delta > 5 minutes → material
void test_material_change_delta_over_5min() {
    const int64_t fiveMinMs = 5LL * 60LL * 1000LL;
    TEST_ASSERT_TRUE(hasMaterialPersistChange(
        true, VALID_EPOCH_MS, 0, TimeService::SOURCE_CLIENT_AP,
        VALID_EPOCH_MS + fiveMinMs, 0, TimeService::SOURCE_CLIENT_AP));
}

// Delta < 5 minutes, same TZ and source → NOT material
void test_no_material_change_small_delta() {
    const int64_t twoMinMs = 2LL * 60LL * 1000LL;
    TEST_ASSERT_FALSE(hasMaterialPersistChange(
        true, VALID_EPOCH_MS, 0, TimeService::SOURCE_CLIENT_AP,
        VALID_EPOCH_MS + twoMinMs, 0, TimeService::SOURCE_CLIENT_AP));
}

// Same epoch, same TZ, same source → NOT material
void test_no_material_change_identical() {
    TEST_ASSERT_FALSE(hasMaterialPersistChange(
        true, VALID_EPOCH_MS, 60, TimeService::SOURCE_GPS,
        VALID_EPOCH_MS, 60, TimeService::SOURCE_GPS));
}

// -----------------------------------------------------------------------

void setUp() {
    mock_preferences::reset();
}

void tearDown() {}

int main() {
    UNITY_BEGIN();

    // isValidUnixMs
    RUN_TEST(test_is_valid_unix_ms_returns_true_for_mid_range);
    RUN_TEST(test_is_valid_unix_ms_returns_false_for_zero);
    RUN_TEST(test_is_valid_unix_ms_returns_false_for_negative);
    RUN_TEST(test_is_valid_unix_ms_returns_false_for_too_old);
    RUN_TEST(test_is_valid_unix_ms_returns_true_at_min_boundary);
    RUN_TEST(test_is_valid_unix_ms_returns_true_at_max_boundary);
    RUN_TEST(test_is_valid_unix_ms_returns_false_above_max);

    // clampTzOffset
    RUN_TEST(test_clamp_tz_in_range_unchanged);
    RUN_TEST(test_clamp_tz_exactly_at_min);
    RUN_TEST(test_clamp_tz_below_min_clamped);
    RUN_TEST(test_clamp_tz_exactly_at_max);
    RUN_TEST(test_clamp_tz_above_max_clamped);

    // parseInt64Strict
    RUN_TEST(test_parse_int64_valid_positive);
    RUN_TEST(test_parse_int64_valid_negative);
    RUN_TEST(test_parse_int64_zero);
    RUN_TEST(test_parse_int64_valid_epoch_ms);
    RUN_TEST(test_parse_int64_empty_string_fails);
    RUN_TEST(test_parse_int64_non_numeric_fails);
    RUN_TEST(test_parse_int64_trailing_chars_fails);
    RUN_TEST(test_parse_int64_leading_space_accepted);

    // normalizeSource
    RUN_TEST(test_normalize_source_none_becomes_client_ap);
    RUN_TEST(test_normalize_source_valid_client_ap_unchanged);
    RUN_TEST(test_normalize_source_valid_gps_unchanged);
    RUN_TEST(test_normalize_source_valid_rtc_unchanged);
    RUN_TEST(test_normalize_source_out_of_range_fails);

    // hasMaterialPersistChange
    RUN_TEST(test_material_change_no_previous_valid);
    RUN_TEST(test_material_change_previous_epoch_invalid);
    RUN_TEST(test_material_change_tz_changed);
    RUN_TEST(test_material_change_source_changed);
    RUN_TEST(test_material_change_delta_over_5min);
    RUN_TEST(test_no_material_change_small_delta);
    RUN_TEST(test_no_material_change_identical);

    return UNITY_END();
}

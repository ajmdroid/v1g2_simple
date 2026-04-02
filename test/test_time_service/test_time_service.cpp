// Tests for time_service.cpp helper logic plus boot-path behavior via native mocks:
//   - isValidUnixMs(int64_t)
//   - clampTzOffset(int32_t)
//   - parseInt64Strict(const String&, int64_t&)
//   - normalizeSource(uint8_t&)
//   - hasMaterialPersistChange(...)

#include <unity.h>
#include <sys/time.h>

#include "../mocks/Arduino.h"
#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../mocks/Preferences.h"

static int mockGettimeofdayResult = -1;
static int64_t mockSystemEpochMs = 0;
static int mockSettimeofdayCalls = 0;
static int64_t mockLastSettimeofdayEpochMs = 0;

extern "C" int mock_gettimeofday(struct timeval* tv, void* /*tz*/) {
    if (mockGettimeofdayResult != 0) {
        return mockGettimeofdayResult;
    }
    if (!tv) {
        return -1;
    }

    tv->tv_sec = static_cast<time_t>(mockSystemEpochMs / 1000LL);
    tv->tv_usec = static_cast<suseconds_t>((mockSystemEpochMs % 1000LL) * 1000LL);
    return 0;
}

extern "C" int mock_settimeofday(const struct timeval* tv, const struct timezone* /*tz*/) {
    mockSettimeofdayCalls++;
    if (!tv) {
        mockLastSettimeofdayEpochMs = 0;
        return -1;
    }

    mockLastSettimeofdayEpochMs = static_cast<int64_t>(tv->tv_sec) * 1000LL
                                + static_cast<int64_t>(tv->tv_usec / 1000);
    return 0;
}

#define gettimeofday mock_gettimeofday
#define settimeofday mock_settimeofday

#include "../../src/time_service.cpp"

#undef gettimeofday
#undef settimeofday

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

// Source changed from CLIENT_AP to RTC → material
void test_material_change_source_changed() {
    TEST_ASSERT_TRUE(hasMaterialPersistChange(
        true, VALID_EPOCH_MS, 0, TimeService::SOURCE_CLIENT_AP,
        VALID_EPOCH_MS, 0, TimeService::SOURCE_RTC));
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

// Same epoch, same TZ, same source (RTC) → NOT material
void test_no_material_change_identical() {
    TEST_ASSERT_FALSE(hasMaterialPersistChange(
        true, VALID_EPOCH_MS, 60, TimeService::SOURCE_RTC,
        VALID_EPOCH_MS, 60, TimeService::SOURCE_RTC));
}

// -----------------------------------------------------------------------
// begin()
// -----------------------------------------------------------------------

void test_begin_restores_valid_system_clock_with_rtc_source() {
    PersistedTime snapshot;
    snapshot.valid = true;
    snapshot.epochMs = VALID_EPOCH_MS - 60000LL;
    snapshot.tzOffsetMin = -240;
    snapshot.source = TimeService::SOURCE_CLIENT_AP;
    TEST_ASSERT_TRUE(savePersistedTimeSnapshot(snapshot));

    mockMillis = 2500;
    mockGettimeofdayResult = 0;
    mockSystemEpochMs = VALID_EPOCH_MS;

    TimeService service;
    service.begin();

    TEST_ASSERT_TRUE(service.timeValid());
    TEST_ASSERT_EQUAL_UINT8(TimeService::SOURCE_RTC, service.timeSource());
    TEST_ASSERT_EQUAL_UINT8(TimeService::CONFIDENCE_ACCURATE, service.timeConfidence());
    TEST_ASSERT_EQUAL_INT32(-240, service.tzOffsetMinutes());
    TEST_ASSERT_EQUAL_INT64(VALID_EPOCH_MS, service.nowEpochMsOr0());
}

void test_begin_ignores_persisted_snapshot_when_system_clock_invalid() {
    PersistedTime snapshot;
    snapshot.valid = true;
    snapshot.epochMs = VALID_EPOCH_MS;
    snapshot.tzOffsetMin = -300;
    snapshot.source = TimeService::SOURCE_CLIENT_AP;
    TEST_ASSERT_TRUE(savePersistedTimeSnapshot(snapshot));

    mockGettimeofdayResult = -1;

    TimeService service;
    service.begin();

    TEST_ASSERT_FALSE(service.timeValid());
    TEST_ASSERT_EQUAL_UINT8(TimeService::SOURCE_NONE, service.timeSource());
    TEST_ASSERT_EQUAL_UINT8(TimeService::CONFIDENCE_NONE, service.timeConfidence());
    TEST_ASSERT_EQUAL_INT32(0, service.tzOffsetMinutes());
    TEST_ASSERT_EQUAL_INT64(0LL, service.nowEpochMsOr0());
    TEST_ASSERT_EQUAL_INT(0, mockSettimeofdayCalls);
}

// -----------------------------------------------------------------------

void setUp() {
    mock_preferences::reset();
    mockMillis = 0;
    mockMicros = 0;
    mockGettimeofdayResult = -1;
    mockSystemEpochMs = 0;
    mockSettimeofdayCalls = 0;
    mockLastSettimeofdayEpochMs = 0;
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

    // begin()
    RUN_TEST(test_begin_restores_valid_system_clock_with_rtc_source);
    RUN_TEST(test_begin_ignores_persisted_snapshot_when_system_clock_invalid);

    return UNITY_END();
}

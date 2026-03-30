// Tests for pure-logic functions in main_boot.cpp:
//   - resetReasonToString(esp_reset_reason_t)
//   - normalizeLegacyLockoutRadiusScale(JsonDocument&)
//
// Hardware-dependent boot helpers (logPanicBreadcrumbs, nvsHealthCheck,
// nextBootId, fatalBootError) require ESP32 peripherals and are not
// exercised here; they compile via the mocks but are not called.

#include <unity.h>
#include <ArduinoJson.h>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/Arduino.h"
#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../mocks/esp_system.h"
#include "../mocks/esp_heap_caps.h"
#include "../mocks/esp_core_dump.h"
#include "../mocks/nvs_flash.h"
#include "../mocks/nvs.h"
#include "../mocks/LittleFS.h"
#include "../mocks/Preferences.h"
#include "../mocks/display.h"

// main_boot.cpp requires 'extern V1Display display'
V1Display display;

#include "../../src/main_boot.cpp"

// -----------------------------------------------------------------------
// resetReasonToString
// -----------------------------------------------------------------------

void test_reset_reason_poweron() {
    TEST_ASSERT_EQUAL_STRING("POWERON", resetReasonToString(ESP_RST_POWERON));
}

void test_reset_reason_sw() {
    TEST_ASSERT_EQUAL_STRING("SW", resetReasonToString(ESP_RST_SW));
}

void test_reset_reason_panic() {
    TEST_ASSERT_EQUAL_STRING("PANIC", resetReasonToString(ESP_RST_PANIC));
}

void test_reset_reason_int_wdt() {
    TEST_ASSERT_EQUAL_STRING("WDT_INT", resetReasonToString(ESP_RST_INT_WDT));
}

void test_reset_reason_task_wdt() {
    TEST_ASSERT_EQUAL_STRING("WDT_TASK", resetReasonToString(ESP_RST_TASK_WDT));
}

void test_reset_reason_wdt() {
    TEST_ASSERT_EQUAL_STRING("WDT", resetReasonToString(ESP_RST_WDT));
}

void test_reset_reason_deepsleep() {
    TEST_ASSERT_EQUAL_STRING("DEEPSLEEP", resetReasonToString(ESP_RST_DEEPSLEEP));
}

void test_reset_reason_brownout() {
    TEST_ASSERT_EQUAL_STRING("BROWNOUT", resetReasonToString(ESP_RST_BROWNOUT));
}

void test_reset_reason_sdio() {
    TEST_ASSERT_EQUAL_STRING("SDIO", resetReasonToString(ESP_RST_SDIO));
}

void test_reset_reason_unknown() {
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", resetReasonToString(ESP_RST_UNKNOWN));
}

// -----------------------------------------------------------------------
// normalizeLegacyLockoutRadiusScale
// -----------------------------------------------------------------------

void test_normalize_empty_doc_returns_zero() {
    JsonDocument doc;
    TEST_ASSERT_EQUAL_UINT32(0u, normalizeLegacyLockoutRadiusScale(doc));
}

void test_normalize_no_zones_key_returns_zero() {
    JsonDocument doc;
    doc["other"] = 42;
    TEST_ASSERT_EQUAL_UINT32(0u, normalizeLegacyLockoutRadiusScale(doc));
}

void test_normalize_zone_missing_rad_skipped() {
    JsonDocument doc;
    doc["zones"][0]["lat"] = 123;
    TEST_ASSERT_EQUAL_UINT32(0u, normalizeLegacyLockoutRadiusScale(doc));
}

// Values below LEGACY_RADIUS_MIN_E5 (450) are already in normalized scale.
void test_normalize_already_normalized_not_changed() {
    JsonDocument doc;
    doc["zones"][0]["rad"] = 135;  // 135 < 450 → skip
    uint32_t migrated = normalizeLegacyLockoutRadiusScale(doc);
    TEST_ASSERT_EQUAL_UINT32(0u, migrated);
    TEST_ASSERT_EQUAL_UINT16(135, doc["zones"][0]["rad"].as<uint16_t>());
}

// 449 is just below the legacy threshold — not migrated.
void test_normalize_boundary_below_threshold_not_migrated() {
    JsonDocument doc;
    doc["zones"][0]["rad"] = 449;
    uint32_t migrated = normalizeLegacyLockoutRadiusScale(doc);
    TEST_ASSERT_EQUAL_UINT32(0u, migrated);
    TEST_ASSERT_EQUAL_UINT16(449, doc["zones"][0]["rad"].as<uint16_t>());
}

// 450 is the minimum legacy value → 450/10 = 45 (= LOCKOUT_LEARNER_RADIUS_E5_MIN).
void test_normalize_boundary_at_threshold_migrated() {
    JsonDocument doc;
    doc["zones"][0]["rad"] = 450;
    uint32_t migrated = normalizeLegacyLockoutRadiusScale(doc);
    TEST_ASSERT_EQUAL_UINT32(1u, migrated);
    TEST_ASSERT_EQUAL_UINT16(45, doc["zones"][0]["rad"].as<uint16_t>());
}

// Typical legacy value: 1350 → 135 (default radius).
void test_normalize_typical_legacy_value() {
    JsonDocument doc;
    doc["zones"][0]["rad"] = 1350;
    uint32_t migrated = normalizeLegacyLockoutRadiusScale(doc);
    TEST_ASSERT_EQUAL_UINT32(1u, migrated);
    TEST_ASSERT_EQUAL_UINT16(135, doc["zones"][0]["rad"].as<uint16_t>());
}

// Very large legacy value is clamped to LOCKOUT_LEARNER_RADIUS_E5_MAX (360).
void test_normalize_large_legacy_clamped_to_max() {
    JsonDocument doc;
    doc["zones"][0]["rad"] = 9999;  // 9999/10 = 999 → clamped to 360
    uint32_t migrated = normalizeLegacyLockoutRadiusScale(doc);
    TEST_ASSERT_EQUAL_UINT32(1u, migrated);
    TEST_ASSERT_EQUAL_UINT16(360, doc["zones"][0]["rad"].as<uint16_t>());
}

// Multiple zones: only those with rad >= 450 are migrated.
void test_normalize_mixed_zones() {
    JsonDocument doc;
    doc["zones"][0]["rad"] = 200;   // < 450 → skip
    doc["zones"][1]["rad"] = 1350;  // legacy → 135
    doc["zones"][2]["rad"] = 3600;  // legacy → 360
    doc["zones"][3]["rad"] = 100;   // < 450 → skip
    uint32_t migrated = normalizeLegacyLockoutRadiusScale(doc);
    TEST_ASSERT_EQUAL_UINT32(2u, migrated);
    TEST_ASSERT_EQUAL_UINT16(200, doc["zones"][0]["rad"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT16(135, doc["zones"][1]["rad"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT16(360, doc["zones"][2]["rad"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT16(100, doc["zones"][3]["rad"].as<uint16_t>());
}

// Zone without "rad" key is skipped even if other zones migrate.
void test_normalize_zone_without_rad_skipped_in_mixed() {
    JsonDocument doc;
    doc["zones"][0]["lat"] = 1000;  // no rad
    doc["zones"][1]["rad"] = 1350;  // legacy
    uint32_t migrated = normalizeLegacyLockoutRadiusScale(doc);
    TEST_ASSERT_EQUAL_UINT32(1u, migrated);
    TEST_ASSERT_EQUAL_UINT16(135, doc["zones"][1]["rad"].as<uint16_t>());
}

// -----------------------------------------------------------------------

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();

    // resetReasonToString
    RUN_TEST(test_reset_reason_poweron);
    RUN_TEST(test_reset_reason_sw);
    RUN_TEST(test_reset_reason_panic);
    RUN_TEST(test_reset_reason_int_wdt);
    RUN_TEST(test_reset_reason_task_wdt);
    RUN_TEST(test_reset_reason_wdt);
    RUN_TEST(test_reset_reason_deepsleep);
    RUN_TEST(test_reset_reason_brownout);
    RUN_TEST(test_reset_reason_sdio);
    RUN_TEST(test_reset_reason_unknown);

    // normalizeLegacyLockoutRadiusScale
    RUN_TEST(test_normalize_empty_doc_returns_zero);
    RUN_TEST(test_normalize_no_zones_key_returns_zero);
    RUN_TEST(test_normalize_zone_missing_rad_skipped);
    RUN_TEST(test_normalize_already_normalized_not_changed);
    RUN_TEST(test_normalize_boundary_below_threshold_not_migrated);
    RUN_TEST(test_normalize_boundary_at_threshold_migrated);
    RUN_TEST(test_normalize_typical_legacy_value);
    RUN_TEST(test_normalize_large_legacy_clamped_to_max);
    RUN_TEST(test_normalize_mixed_zones);
    RUN_TEST(test_normalize_zone_without_rad_skipped_in_mixed);

    return UNITY_END();
}

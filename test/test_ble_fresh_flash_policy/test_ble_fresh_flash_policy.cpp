#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/ble_fresh_flash_policy.h"
#include "../../src/ble_fresh_flash_policy.cpp"

void setUp() {
    mock_preferences::reset();
}

void tearDown() {}

void test_missing_version_is_treated_as_mismatch() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));

    TEST_ASSERT_TRUE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, "4.0.0-dev"));

    prefs.end();
}

void test_matching_version_is_not_a_mismatch() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeFirmwareVersion(prefs, "4.0.0-dev"));

    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, "4.0.0-dev"));

    prefs.end();
}

void test_store_version_updates_persisted_value() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));

    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeFirmwareVersion(prefs, "4.0.1"));
    TEST_ASSERT_EQUAL_STRING("4.0.1", BleFreshFlashPolicy::readStoredFirmwareVersion(prefs).c_str());
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, "4.0.2"));
    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, "4.0.1"));

    prefs.end();
}

void test_null_current_version_normalizes_to_empty_string() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));

    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeFirmwareVersion(prefs, nullptr));
    TEST_ASSERT_EQUAL_STRING("", BleFreshFlashPolicy::readStoredFirmwareVersion(prefs).c_str());
    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, nullptr));
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, "4.0.0-dev"));

    prefs.end();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_missing_version_is_treated_as_mismatch);
    RUN_TEST(test_matching_version_is_not_a_mismatch);
    RUN_TEST(test_store_version_updates_persisted_value);
    RUN_TEST(test_null_current_version_normalizes_to_empty_string);
    return UNITY_END();
}

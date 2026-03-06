#include <unity.h>

#include "../../src/settings.h"

void setUp(void) {}
void tearDown(void) {}

void test_camera_setting_defaults_match_phase1_contract() {
    V1Settings settings;

    TEST_ASSERT_TRUE(settings.cameraAlertsEnabled);
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_DEFAULT, settings.cameraAlertRangeCm);
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_NEAR_RANGE_CM_DEFAULT, settings.cameraAlertNearRangeCm);
    TEST_ASSERT_TRUE(settings.cameraTypeAlpr);
    TEST_ASSERT_TRUE(settings.cameraTypeRedLight);
    TEST_ASSERT_TRUE(settings.cameraTypeSpeed);
    TEST_ASSERT_FALSE(settings.cameraTypeBusLane);
    TEST_ASSERT_EQUAL_HEX16(0x780F, settings.colorCameraArrow);
    TEST_ASSERT_EQUAL_HEX16(0x780F, settings.colorCameraText);
    TEST_ASSERT_TRUE(settings.cameraVoiceFarEnabled);
    TEST_ASSERT_TRUE(settings.cameraVoiceNearEnabled);
}

void test_camera_alert_range_clamps_to_supported_limits() {
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_MIN, clampCameraAlertRangeCmValue(0));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_MIN,
                             clampCameraAlertRangeCmValue(static_cast<int>(CAMERA_ALERT_RANGE_CM_MIN)));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_DEFAULT,
                             clampCameraAlertRangeCmValue(static_cast<int>(CAMERA_ALERT_RANGE_CM_DEFAULT)));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_MAX,
                             clampCameraAlertRangeCmValue(static_cast<int>(CAMERA_ALERT_RANGE_CM_MAX)));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_MAX, clampCameraAlertRangeCmValue(999999));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_NEAR_RANGE_CM_MIN, clampCameraAlertNearRangeCmValue(0));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_NEAR_RANGE_CM_DEFAULT,
                             clampCameraAlertNearRangeCmValue(
                                 static_cast<int>(CAMERA_ALERT_NEAR_RANGE_CM_DEFAULT)));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_NEAR_RANGE_CM_MAX,
                             clampCameraAlertNearRangeCmValue(999999));
}

void test_camera_close_alert_range_never_exceeds_first_alert_range() {
    uint32_t first = 40234;
    uint32_t close = 99999;

    normalizeCameraAlertRanges(first, close);

    TEST_ASSERT_EQUAL_UINT32(40234, first);
    TEST_ASSERT_EQUAL_UINT32(40234, close);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_camera_setting_defaults_match_phase1_contract);
    RUN_TEST(test_camera_alert_range_clamps_to_supported_limits);
    RUN_TEST(test_camera_close_alert_range_never_exceeds_first_alert_range);
    return UNITY_END();
}

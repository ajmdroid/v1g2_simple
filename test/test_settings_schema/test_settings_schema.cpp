#include <unity.h>

#include "../../src/settings.h"

void setUp(void) {}
void tearDown(void) {}

void test_camera_setting_defaults_match_alpr_only_contract() {
	V1Settings settings;

	TEST_ASSERT_TRUE(settings.cameraAlertsEnabled);
	TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_DEFAULT, settings.cameraAlertRangeCm);
}

void test_camera_alert_range_clamps_to_supported_limits() {
	TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_MIN, clampCameraAlertRangeCmValue(0));
	TEST_ASSERT_EQUAL_UINT32(
		CAMERA_ALERT_RANGE_CM_MIN,
		clampCameraAlertRangeCmValue(static_cast<int>(CAMERA_ALERT_RANGE_CM_MIN)));
	TEST_ASSERT_EQUAL_UINT32(
		CAMERA_ALERT_RANGE_CM_DEFAULT,
		clampCameraAlertRangeCmValue(static_cast<int>(CAMERA_ALERT_RANGE_CM_DEFAULT)));
	TEST_ASSERT_EQUAL_UINT32(
		CAMERA_ALERT_RANGE_CM_MAX,
		clampCameraAlertRangeCmValue(static_cast<int>(CAMERA_ALERT_RANGE_CM_MAX)));
	TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_MAX, clampCameraAlertRangeCmValue(999999));
}

int main() {
	UNITY_BEGIN();
	RUN_TEST(test_camera_setting_defaults_match_alpr_only_contract);
	RUN_TEST(test_camera_alert_range_clamps_to_supported_limits);
	return UNITY_END();
}

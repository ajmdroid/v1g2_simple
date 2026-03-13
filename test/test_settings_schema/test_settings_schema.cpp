#include <unity.h>

#include "../../src/settings.h"

void setUp(void) {}
void tearDown(void) {}

void test_camera_setting_defaults_match_alpr_only_contract() {
	V1Settings settings;

	TEST_ASSERT_TRUE(settings.cameraAlertsEnabled);
	TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_DEFAULT, settings.cameraAlertRangeCm);
}

void test_v1_settings_defaults_cover_current_runtime_shape() {
	V1Settings settings;

	TEST_ASSERT_TRUE(settings.enableWifi);
	TEST_ASSERT_EQUAL_INT(V1_WIFI_AP, settings.wifiMode);
	TEST_ASSERT_EQUAL_STRING("V1-Simple", settings.apSSID.c_str());
	TEST_ASSERT_TRUE(settings.proxyBLE);
	TEST_ASSERT_FALSE(settings.gpsEnabled);
	TEST_ASSERT_EQUAL_INT(LOCKOUT_RUNTIME_OFF, settings.gpsLockoutMode);
	TEST_ASSERT_TRUE(settings.gpsLockoutCoreGuardEnabled);
	TEST_ASSERT_EQUAL_UINT8(LOCKOUT_LEARNER_HITS_DEFAULT, settings.gpsLockoutLearnerPromotionHits);
	TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_DEFAULT, settings.cameraAlertRangeCm);
	TEST_ASSERT_EQUAL_UINT8(200, settings.brightness);
	TEST_ASSERT_EQUAL_INT(DISPLAY_STYLE_CLASSIC, settings.displayStyle);
	TEST_ASSERT_EQUAL_INT(VOICE_MODE_BAND_FREQ, settings.voiceAlertMode);
	TEST_ASSERT_TRUE(settings.voiceDirectionEnabled);
	TEST_ASSERT_EQUAL_UINT8(75, settings.voiceVolume);
	TEST_ASSERT_FALSE(settings.autoPushEnabled);
	TEST_ASSERT_EQUAL_INT(0, settings.activeSlot);
	TEST_ASSERT_EQUAL_STRING("DEFAULT", settings.slot0Name.c_str());
	TEST_ASSERT_EQUAL_INT(V1_MODE_UNKNOWN, settings.slot0_default.mode);
	TEST_ASSERT_FALSE(settings.obdEnabled);
	TEST_ASSERT_EQUAL_INT8(-80, settings.obdMinRssi);
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
	RUN_TEST(test_v1_settings_defaults_cover_current_runtime_shape);
	RUN_TEST(test_camera_alert_range_clamps_to_supported_limits);
	return UNITY_END();
}

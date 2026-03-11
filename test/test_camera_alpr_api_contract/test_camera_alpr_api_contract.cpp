#include <unity.h>

#include <cstring>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/camera_alert/camera_alert_api_service.cpp"
#include "../../src/modules/lockout/road_map_reader.cpp"

namespace {

bool responseContains(const WebServer& server, const char* needle) {
	return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

}  // namespace

void setUp() {
	mockMillis = 1000;
	mockMicros = 1000000;
}

void tearDown() {}

void test_contract_settings_get_exposes_only_two_fields() {
	WebServer server(80);
	SettingsManager settingsManager;

	CameraAlertApiService::handleApiSettingsGet(server, settingsManager, []() {});

	TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
	TEST_ASSERT_TRUE(responseContains(server, "\"cameraAlertsEnabled\":true"));
	TEST_ASSERT_TRUE(responseContains(server, "\"cameraAlertRangeCm\":128748"));
	TEST_ASSERT_FALSE(responseContains(server, "cameraType"));
	TEST_ASSERT_FALSE(responseContains(server, "cameraVoice"));
	TEST_ASSERT_FALSE(responseContains(server, "colorCamera"));
}

void test_contract_settings_post_rejects_removed_arg_names() {
	static constexpr const char* kRemovedArgs[] = {
		"cameraAlertNearRangeCm",
		"cameraTypeAlpr",
		"cameraTypeRedLight",
		"cameraTypeSpeed",
		"cameraTypeBusLane",
		"colorCameraArrow",
		"colorCameraText",
		"cameraVoiceFarEnabled",
		"cameraVoiceNearEnabled",
	};

	for (const char* arg : kRemovedArgs) {
		WebServer server(80);
		SettingsManager settingsManager;
		server.setArg(arg, "1");

		CameraAlertApiService::handleApiSettingsPost(
			server,
			settingsManager,
			[]() { return true; },
			[]() {});

		TEST_ASSERT_EQUAL_INT_MESSAGE(400, server.lastStatusCode, arg);
		TEST_ASSERT_TRUE_MESSAGE(responseContains(server, "\"error\":\"unsupported "), arg);
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, settingsManager.saveCalls, arg);
	}
}

int main() {
	UNITY_BEGIN();
	RUN_TEST(test_contract_settings_get_exposes_only_two_fields);
	RUN_TEST(test_contract_settings_post_rejects_removed_arg_names);
	return UNITY_END();
}

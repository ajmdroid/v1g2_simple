#include <unity.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#define private public
#include "../../src/modules/camera_alert/camera_alert_module.h"
#undef private

#include "../../src/modules/camera_alert/camera_alert_api_service.cpp"
#include "../../src/modules/lockout/road_map_reader.cpp"

namespace {

constexpr int32_t BASE_LAT_E5 = 3974000;
constexpr int32_t BASE_LON_E5 = -10499000;

bool responseContains(const WebServer& server, const char* needle) {
	return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

std::vector<uint8_t> buildSingleAlprMap() {
	const uint32_t gridIndexOffset = sizeof(RoadMapHeader);
	const uint32_t gridSize = sizeof(RoadMapGridEntry);
	const uint32_t segDataOffset = gridIndexOffset + gridSize;
	const uint32_t cameraIndexOffset = segDataOffset;
	const uint32_t cameraDataOffset = cameraIndexOffset + gridSize;
	const uint32_t fileSize = cameraDataOffset + sizeof(CameraRecord);

	RoadMapHeader header{};
	memcpy(header.magic, "RMAP", 4);
	header.version = 2;
	header.roadClassCount = 1;
	header.minLatE5 = BASE_LAT_E5 - 1000;
	header.maxLatE5 = BASE_LAT_E5 + 1000;
	header.minLonE5 = BASE_LON_E5 - 1000;
	header.maxLonE5 = BASE_LON_E5 + 1000;
	header.gridRows = 1;
	header.gridCols = 1;
	header.cellSizeE5 = 5000;
	header.gridIndexOffset = gridIndexOffset;
	header.segDataOffset = segDataOffset;
	header.fileSize = fileSize;
	header.cameraIndexOffset = cameraIndexOffset;
	header.cameraCount = 1;

	RoadMapGridEntry roadGrid{};
	RoadMapGridEntry cameraGrid{};
	cameraGrid.segCount = 1;

	CameraRecord camera{};
	camera.latE5 = BASE_LAT_E5;
	camera.lonE5 = BASE_LON_E5;
	camera.bearing = 0xFFFF;
	camera.flags = static_cast<uint8_t>(CameraType::ALPR);
	camera.speedMph = 35;

	std::vector<uint8_t> buffer(fileSize, 0);
	memcpy(buffer.data(), &header, sizeof(header));
	memcpy(buffer.data() + gridIndexOffset, &roadGrid, sizeof(roadGrid));
	memcpy(buffer.data() + cameraIndexOffset, &cameraGrid, sizeof(cameraGrid));
	memcpy(buffer.data() + cameraDataOffset, &camera, sizeof(camera));
	return buffer;
}

}  // namespace

void setUp() {
	mockMillis = 1000;
	mockMicros = 1000000;
}

void tearDown() {}

void test_settings_get_serializes_alpr_only_payload() {
	WebServer server(80);
	SettingsManager settingsManager;
	int uiActivityCalls = 0;
	V1Settings& settings = settingsManager.mutableSettings();

	settings.cameraAlertsEnabled = false;
	settings.cameraAlertRangeCm = 77777;

	CameraAlertApiService::handleApiSettingsGet(
		server,
		settingsManager,
		[&uiActivityCalls]() { ++uiActivityCalls; });

	TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
	TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
	TEST_ASSERT_TRUE(responseContains(server, "\"cameraAlertsEnabled\":false"));
	TEST_ASSERT_TRUE(responseContains(server, "\"cameraAlertRangeCm\":77777"));
	TEST_ASSERT_FALSE(responseContains(server, "cameraAlertNearRangeCm"));
	TEST_ASSERT_FALSE(responseContains(server, "cameraType"));
	TEST_ASSERT_FALSE(responseContains(server, "cameraVoice"));
}

void test_settings_post_updates_alpr_fields_and_saves_once() {
	WebServer server(80);
	SettingsManager settingsManager;
	int rateLimitCalls = 0;
	int uiActivityCalls = 0;

	server.setArg("cameraAlertsEnabled", "0");
	server.setArg("cameraAlertRangeCm", "40000");

	CameraAlertApiService::handleApiSettingsPost(
		server,
		settingsManager,
		[&rateLimitCalls]() {
			++rateLimitCalls;
			return true;
		},
		[&uiActivityCalls]() { ++uiActivityCalls; });

	TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
	TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
	TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
	TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
	TEST_ASSERT_FALSE(settingsManager.get().cameraAlertsEnabled);
	TEST_ASSERT_EQUAL_UINT32(40000u, settingsManager.get().cameraAlertRangeCm);
	TEST_ASSERT_EQUAL_INT(1, settingsManager.saveCalls);
}

void test_settings_post_rejects_removed_legacy_args() {
	WebServer server(80);
	SettingsManager settingsManager;

	server.setArg("cameraTypeSpeed", "true");

	CameraAlertApiService::handleApiSettingsPost(
		server,
		settingsManager,
		[]() { return true; },
		[]() {});

	TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
	TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"unsupported cameraTypeSpeed\""));
	TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
}

void test_settings_post_rejects_invalid_numeric_token() {
	WebServer server(80);
	SettingsManager settingsManager;

	server.setArg("cameraAlertRangeCm", "0x1234");

	CameraAlertApiService::handleApiSettingsPost(
		server,
		settingsManager,
		[]() { return true; },
		[]() {});

	TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
	TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"invalid cameraAlertRangeCm\""));
	TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
}

void test_status_returns_camera_count_and_distance_without_type() {
	WebServer server(80);
	RoadMapReader roadMapReader;
	CameraAlertModule module;
	int uiActivityCalls = 0;
	const std::vector<uint8_t> mapData = buildSingleAlprMap();

	TEST_ASSERT_TRUE(
		roadMapReader.loadFromBuffer(const_cast<uint8_t*>(mapData.data()), static_cast<uint32_t>(mapData.size())));

	module.displayPayload_.active = true;
	module.displayPayload_.distanceCm = 18750;

	CameraAlertApiService::handleApiStatus(
		server,
		module,
		roadMapReader,
		[&uiActivityCalls]() { ++uiActivityCalls; });

	TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
	TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
	TEST_ASSERT_TRUE(responseContains(server, "\"cameraCount\":1"));
	TEST_ASSERT_TRUE(responseContains(server, "\"displayActive\":true"));
	TEST_ASSERT_TRUE(responseContains(server, "\"distanceCm\":18750"));
	TEST_ASSERT_FALSE(responseContains(server, "\"type\""));
}

void test_status_nulls_distance_when_inactive() {
	WebServer server(80);
	RoadMapReader roadMapReader;
	CameraAlertModule module;

	CameraAlertApiService::handleApiStatus(
		server,
		module,
		roadMapReader,
		[]() {});

	TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
	TEST_ASSERT_TRUE(responseContains(server, "\"cameraCount\":0"));
	TEST_ASSERT_TRUE(responseContains(server, "\"displayActive\":false"));
	TEST_ASSERT_TRUE(responseContains(server, "\"distanceCm\":null"));
}

int main() {
	UNITY_BEGIN();
	RUN_TEST(test_settings_get_serializes_alpr_only_payload);
	RUN_TEST(test_settings_post_updates_alpr_fields_and_saves_once);
	RUN_TEST(test_settings_post_rejects_removed_legacy_args);
	RUN_TEST(test_settings_post_rejects_invalid_numeric_token);
	RUN_TEST(test_status_returns_camera_count_and_distance_without_type);
	RUN_TEST(test_status_nulls_distance_when_inactive);
	return UNITY_END();
}

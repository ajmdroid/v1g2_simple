#include <unity.h>
#include <cstring>
#include <cstdio>

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

uint8_t* fixtureData = nullptr;
uint32_t fixtureSize = 0;

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

bool loadFixtureFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fseek(file, 0, SEEK_END);
    fixtureSize = static_cast<uint32_t>(ftell(file));
    fseek(file, 0, SEEK_SET);
    fixtureData = new uint8_t[fixtureSize];
    const size_t bytesRead = fread(fixtureData, 1, fixtureSize, file);
    fclose(file);
    return bytesRead == fixtureSize;
}

void resetFixture() {
    delete[] fixtureData;
    fixtureData = nullptr;
    fixtureSize = 0;
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {
    resetFixture();
}

void test_settings_get_serializes_camera_payload() {
    WebServer server(80);
    SettingsManager settingsManager;
    int uiActivityCalls = 0;
    V1Settings& settings = settingsManager.mutableSettings();

    settings.cameraAlertsEnabled = false;
    settings.cameraAlertRangeCm = 77777;
    settings.cameraTypeBusLane = true;
    settings.colorCameraArrow = 0x1234;
    settings.cameraVoiceNearEnabled = false;

    CameraAlertApiService::handleApiSettingsGet(
        server,
        settingsManager,
        [&uiActivityCalls]() { ++uiActivityCalls; });

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"cameraAlertsEnabled\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"cameraAlertRangeCm\":77777"));
    TEST_ASSERT_TRUE(responseContains(server, "\"cameraTypeBusLane\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"colorCameraArrow\":4660"));
    TEST_ASSERT_TRUE(responseContains(server, "\"cameraVoiceNearEnabled\":false"));
}

void test_settings_post_rate_limit_short_circuits() {
    WebServer server(80);
    SettingsManager settingsManager;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    server.setArg("cameraAlertsEnabled", "false");

    CameraAlertApiService::handleApiSettingsPost(
        server,
        settingsManager,
        [&rateLimitCalls]() {
            ++rateLimitCalls;
            return false;
        },
        [&uiActivityCalls]() { ++uiActivityCalls; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_TRUE(settingsManager.get().cameraAlertsEnabled);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
}

void test_settings_post_updates_all_fields_and_saves_once() {
    WebServer server(80);
    SettingsManager settingsManager;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    server.setArg("cameraAlertsEnabled", "0");
    server.setArg("cameraAlertRangeCm", "999999");
    server.setArg("cameraTypeAlpr", "0");
    server.setArg("cameraTypeRedLight", "1");
    server.setArg("cameraTypeSpeed", "false");
    server.setArg("cameraTypeBusLane", "true");
    server.setArg("colorCameraArrow", "70000");
    server.setArg("colorCameraText", "1234");
    server.setArg("cameraVoiceFarEnabled", "false");
    server.setArg("cameraVoiceNearEnabled", "1");

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
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_MAX, settingsManager.get().cameraAlertRangeCm);
    TEST_ASSERT_FALSE(settingsManager.get().cameraTypeAlpr);
    TEST_ASSERT_TRUE(settingsManager.get().cameraTypeRedLight);
    TEST_ASSERT_FALSE(settingsManager.get().cameraTypeSpeed);
    TEST_ASSERT_TRUE(settingsManager.get().cameraTypeBusLane);
    TEST_ASSERT_EQUAL_UINT16(65535, settingsManager.get().colorCameraArrow);
    TEST_ASSERT_EQUAL_UINT16(1234, settingsManager.get().colorCameraText);
    TEST_ASSERT_FALSE(settingsManager.get().cameraVoiceFarEnabled);
    TEST_ASSERT_TRUE(settingsManager.get().cameraVoiceNearEnabled);
    TEST_ASSERT_EQUAL_INT(1, settingsManager.saveCalls);
}

void test_settings_post_rejects_invalid_bool_without_partial_mutation() {
    WebServer server(80);
    SettingsManager settingsManager;
    int uiActivityCalls = 0;
    V1Settings& settings = settingsManager.mutableSettings();

    settings.cameraTypeSpeed = true;
    server.setArg("cameraTypeSpeed", "maybe");
    server.setArg("cameraAlertRangeCm", "20000");

    CameraAlertApiService::handleApiSettingsPost(
        server,
        settingsManager,
        []() { return true; },
        [&uiActivityCalls]() { ++uiActivityCalls; });

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"invalid cameraTypeSpeed\""));
    TEST_ASSERT_TRUE(settingsManager.get().cameraTypeSpeed);
    TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_DEFAULT, settingsManager.get().cameraAlertRangeCm);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
}

void test_settings_post_rejects_invalid_numeric_token() {
    WebServer server(80);
    SettingsManager settingsManager;

    server.setArg("colorCameraText", "0x1234");

    CameraAlertApiService::handleApiSettingsPost(
        server,
        settingsManager,
        []() { return true; },
        []() {});

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"invalid colorCameraText\""));
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
}

void test_status_returns_camera_count_and_active_payload() {
    WebServer server(80);
    RoadMapReader roadMapReader;
    CameraAlertModule module;
    int uiActivityCalls = 0;

    if (!loadFixtureFile("test/fixtures/camera_types_road_map.bin")) {
        TEST_ASSERT_TRUE(loadFixtureFile("../../test/fixtures/camera_types_road_map.bin"));
    }
    TEST_ASSERT_TRUE(roadMapReader.loadFromBuffer(fixtureData, fixtureSize));

    module.displayPayload_.active = true;
    module.displayPayload_.type = CameraType::BUS_LANE;
    module.displayPayload_.distanceCm = 18750;

    CameraAlertApiService::handleApiStatus(
        server,
        module,
        roadMapReader,
        [&uiActivityCalls]() { ++uiActivityCalls; });

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"cameraCount\":4"));
    TEST_ASSERT_TRUE(responseContains(server, "\"displayActive\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"type\":\"bus_lane\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"distanceCm\":18750"));

}

void test_status_nulls_type_and_distance_when_inactive() {
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
    TEST_ASSERT_TRUE(responseContains(server, "\"type\":null"));
    TEST_ASSERT_TRUE(responseContains(server, "\"distanceCm\":null"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_settings_get_serializes_camera_payload);
    RUN_TEST(test_settings_post_rate_limit_short_circuits);
    RUN_TEST(test_settings_post_updates_all_fields_and_saves_once);
    RUN_TEST(test_settings_post_rejects_invalid_bool_without_partial_mutation);
    RUN_TEST(test_settings_post_rejects_invalid_numeric_token);
    RUN_TEST(test_status_returns_camera_count_and_active_payload);
    RUN_TEST(test_status_nulls_type_and_distance_when_inactive);
    return UNITY_END();
}

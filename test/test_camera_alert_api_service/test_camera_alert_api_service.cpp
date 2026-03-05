#include <unity.h>
#include <cstring>

#include "../../src/modules/camera_alert/camera_alert_api_service.h"
#include "../../src/modules/camera_alert/camera_alert_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace {

struct FakeRuntime {
    V1Settings settings;
    CameraAlertStatusSnapshot status;
    uint32_t cameraCount = 0;
    int saveCalls = 0;
};

CameraAlertApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return CameraAlertApiService::Runtime{
        [&rt]() -> const V1Settings& { return rt.settings; },
        [&rt]() -> V1Settings& { return rt.settings; },
        [&rt]() { rt.saveCalls++; },
        [&rt]() -> uint32_t { return rt.cameraCount; },
        [&rt](CameraAlertStatusSnapshot& snapshot) -> bool {
            snapshot = rt.status;
            return true;
        },
    };
}

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_get_settings_returns_full_payload_with_camera_count() {
    WebServer server(80);
    FakeRuntime rt;
    rt.cameraCount = 1234;
    rt.settings.cameraAlertRangeM = 900;
    rt.settings.cameraVoiceClose = false;

    CameraAlertApiService::handleApiSettingsGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"cameraCount\":1234"));
    TEST_ASSERT_TRUE(responseContains(server, "\"cameraAlertRangeM\":900"));
    TEST_ASSERT_TRUE(responseContains(server, "\"cameraVoiceClose\":false"));
}

void test_post_settings_supports_subset_patch_and_save() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.cameraTypeBusLane = false;
    rt.settings.cameraVoiceClose = true;
    server.setArg("plain", "{\"cameraTypeBusLane\":true,\"cameraVoiceClose\":false}");

    CameraAlertApiService::handleApiSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(rt.settings.cameraTypeBusLane);
    TEST_ASSERT_FALSE(rt.settings.cameraVoiceClose);
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
}

void test_post_settings_clamps_range_value() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"cameraAlertRangeM\":99999}");

    CameraAlertApiService::handleApiSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);

    server.clearArgs();
    server.setArg("plain", "{\"cameraAlertRangeM\":5001}");
    CameraAlertApiService::handleApiSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_UINT16(5000, rt.settings.cameraAlertRangeM);
}

void test_post_settings_rejects_malformed_json_or_type_mismatch() {
    WebServer server(80);
    FakeRuntime rt;

    server.setArg("plain", "{not json");
    CameraAlertApiService::handleApiSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });
    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);

    server.clearArgs();
    server.setArg("plain", "{\"cameraAlertsEnabled\":\"yes\"}");
    CameraAlertApiService::handleApiSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });
    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
}

void test_get_status_returns_live_snapshot() {
    WebServer server(80);
    FakeRuntime rt;
    rt.cameraCount = 99;
    rt.status.displayActive = true;
    rt.status.encounterActive = true;
    rt.status.hasPayload = true;
    rt.status.distanceCm = 15240;
    rt.status.typeName = "speed";
    rt.status.flags = 1;
    rt.status.pendingNear = true;

    CameraAlertApiService::handleApiStatusGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"active\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"cameraCount\":99"));
    TEST_ASSERT_TRUE(responseContains(server, "\"type\":\"speed\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"pendingNear\":true"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_get_settings_returns_full_payload_with_camera_count);
    RUN_TEST(test_post_settings_supports_subset_patch_and_save);
    RUN_TEST(test_post_settings_clamps_range_value);
    RUN_TEST(test_post_settings_rejects_malformed_json_or_type_mismatch);
    RUN_TEST(test_get_status_returns_live_snapshot);
    return UNITY_END();
}

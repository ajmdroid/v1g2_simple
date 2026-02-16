#include <unity.h>
#include <cstring>
#include <vector>

#include "../../src/modules/wifi/wifi_v1_profile_api_service.h"
#include "../../src/modules/wifi/wifi_v1_profile_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeRuntime {
    bool connected = false;
    bool requestResult = false;
    bool profileFound = false;
    bool parseSettingsOk = true;
    bool writeResult = true;

    uint8_t profileBytes[6] = {0};
    bool profileDisplayOn = true;

    int requestCalls = 0;
    int loadProfileSettingsCalls = 0;
    int parseSettingsCalls = 0;
    int writeCalls = 0;
    int setDisplayCalls = 0;

    uint8_t lastWriteBytes[6] = {0};
    bool lastDisplayOn = true;
};

static WifiV1ProfileApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1ProfileApiService::Runtime{
        []() { return std::vector<String>{}; },
        [](const String&, WifiV1ProfileApiService::ProfileSummary&) { return false; },
        [](const String&, String&) { return false; },
        [&rt](const String&, uint8_t outBytes[6], bool& displayOn) {
            rt.loadProfileSettingsCalls++;
            if (!rt.profileFound) {
                return false;
            }
            memcpy(outBytes, rt.profileBytes, 6);
            displayOn = rt.profileDisplayOn;
            return true;
        },
        [&rt](const JsonObject& settingsObj, uint8_t outBytes[6]) {
            rt.parseSettingsCalls++;
            if (!rt.parseSettingsOk) {
                return false;
            }
            memset(outBytes, 0, 6);
            if (settingsObj["byte0"].is<int>()) {
                outBytes[0] = static_cast<uint8_t>(settingsObj["byte0"].as<int>());
            }
            return true;
        },
        [](const String&, const String&, bool, const uint8_t[6], String&) { return false; },
        [](const String&) { return false; },
        [&rt]() {
            rt.requestCalls++;
            return rt.requestResult;
        },
        [&rt](const uint8_t inBytes[6]) {
            rt.writeCalls++;
            memcpy(rt.lastWriteBytes, inBytes, 6);
            return rt.writeResult;
        },
        [&rt](bool displayOn) {
            rt.setDisplayCalls++;
            rt.lastDisplayOn = displayOn;
        },
        []() { return false; },
        []() { return String("{}"); },
        [&rt]() { return rt.connected; },
        []() {},
    };
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_pull_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.requestResult = true;

    WifiV1ProfileApiService::handleApiSettingsPull(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.requestCalls);
}

void test_pull_requires_connected_v1() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = false;

    WifiV1ProfileApiService::handleApiSettingsPull(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"V1 not connected\""));
}

void test_pull_request_failure_returns_500() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.requestResult = false;

    WifiV1ProfileApiService::handleApiSettingsPull(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Failed to send request\""));
    TEST_ASSERT_EQUAL_INT(1, rt.requestCalls);
}

void test_pull_success_returns_200() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.requestResult = true;

    WifiV1ProfileApiService::handleApiSettingsPull(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"Request sent. Check current settings.\""));
    TEST_ASSERT_EQUAL_INT(1, rt.requestCalls);
}

void test_push_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.writeCalls);
}

void test_push_requires_connected_v1() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = false;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"V1 not connected\""));
}

void test_push_requires_body() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing request body\""));
}

void test_push_invalid_json_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    server.setArg("plain", "{bad json");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Invalid JSON\""));
}

void test_push_profile_name_not_found_returns_404() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.profileFound = false;
    server.setArg("plain", "{\"name\":\"Unknown\"}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Profile not found\""));
}

void test_push_profile_name_success_writes_and_sets_display() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.profileFound = true;
    rt.profileDisplayOn = false;
    rt.profileBytes[0] = 0xAA;
    rt.profileBytes[5] = 0x55;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.loadProfileSettingsCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.writeCalls);
    TEST_ASSERT_EQUAL_UINT8(0xAA, rt.lastWriteBytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0x55, rt.lastWriteBytes[5]);
    TEST_ASSERT_EQUAL_INT(1, rt.setDisplayCalls);
    TEST_ASSERT_FALSE(rt.lastDisplayOn);
}

void test_push_raw_bytes_requires_six_items() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    server.setArg("plain", "{\"bytes\":[1,2,3]}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Invalid bytes array\""));
}

void test_push_raw_bytes_success() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    server.setArg("plain", "{\"bytes\":[1,2,3,4,5,6],\"displayOn\":false}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.writeCalls);
    TEST_ASSERT_EQUAL_UINT8(1, rt.lastWriteBytes[0]);
    TEST_ASSERT_EQUAL_UINT8(6, rt.lastWriteBytes[5]);
    TEST_ASSERT_EQUAL_INT(1, rt.setDisplayCalls);
    TEST_ASSERT_FALSE(rt.lastDisplayOn);
}

void test_push_settings_parse_failure_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.parseSettingsOk = false;
    server.setArg("plain", "{\"settings\":{\"byte0\":9}}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Invalid settings\""));
    TEST_ASSERT_EQUAL_INT(0, rt.writeCalls);
}

void test_push_settings_root_fallback_success() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.parseSettingsOk = true;
    server.setArg("plain", "{\"byte0\":9}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.parseSettingsCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.writeCalls);
    TEST_ASSERT_EQUAL_UINT8(9, rt.lastWriteBytes[0]);
    TEST_ASSERT_EQUAL_INT(1, rt.setDisplayCalls);
    TEST_ASSERT_TRUE(rt.lastDisplayOn);
}

void test_push_write_failure_returns_500() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.parseSettingsOk = true;
    rt.writeResult = false;
    server.setArg("plain", "{\"settings\":{\"byte0\":2},\"displayOn\":false}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Write command failed - check V1 connection\""));
    TEST_ASSERT_EQUAL_INT(0, rt.setDisplayCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_pull_rate_limited_short_circuits);
    RUN_TEST(test_pull_requires_connected_v1);
    RUN_TEST(test_pull_request_failure_returns_500);
    RUN_TEST(test_pull_success_returns_200);
    RUN_TEST(test_push_rate_limited_short_circuits);
    RUN_TEST(test_push_requires_connected_v1);
    RUN_TEST(test_push_requires_body);
    RUN_TEST(test_push_invalid_json_returns_400);
    RUN_TEST(test_push_profile_name_not_found_returns_404);
    RUN_TEST(test_push_profile_name_success_writes_and_sets_display);
    RUN_TEST(test_push_raw_bytes_requires_six_items);
    RUN_TEST(test_push_raw_bytes_success);
    RUN_TEST(test_push_settings_parse_failure_returns_400);
    RUN_TEST(test_push_settings_root_fallback_success);
    RUN_TEST(test_push_write_failure_returns_500);
    return UNITY_END();
}

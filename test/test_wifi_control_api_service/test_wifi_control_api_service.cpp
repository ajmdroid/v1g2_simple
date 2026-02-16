#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_control_api_service.h"
#include "../../src/modules/wifi/wifi_control_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_profile_push_requires_v1_connection() {
    WebServer server(80);
    int rateLimitCalls = 0;

    WifiControlApiService::handleProfilePush(
        server,
        false,
        []() { return true; },
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"V1 not connected\""));
}

void test_profile_push_reports_missing_callback() {
    WebServer server(80);

    WifiControlApiService::handleProfilePush(
        server,
        true,
        std::function<bool()>(),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Push handler unavailable\""));
}

void test_profile_push_reports_queued() {
    WebServer server(80);

    WifiControlApiService::handleProfilePush(
        server,
        true,
        []() { return true; },
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "Profile push queued - check display for progress"));
}

void test_profile_push_rate_limited_short_circuits() {
    WebServer server(80);
    int callbackCalls = 0;

    WifiControlApiService::handleProfilePush(
        server,
        true,
        [&callbackCalls]() {
            callbackCalls++;
            return true;
        },
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, callbackCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_dark_mode_missing_state_param() {
    WebServer server(80);

    WifiControlApiService::handleDarkMode(
        server,
        []([[maybe_unused]] const char* cmd, [[maybe_unused]] bool value) { return true; },
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing state parameter\""));
}

void test_dark_mode_inverts_display_command() {
    WebServer server(80);
    server.setArg("state", "true");
    String capturedCommand;
    bool capturedValue = true;

    WifiControlApiService::handleDarkMode(
        server,
        [&capturedCommand, &capturedValue](const char* cmd, bool value) {
            capturedCommand = cmd;
            capturedValue = value;
            return true;
        },
        []() { return true; });

    TEST_ASSERT_EQUAL_STRING("display", capturedCommand.c_str());
    TEST_ASSERT_FALSE(capturedValue);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"darkMode\":true"));
}

void test_mute_missing_state_param() {
    WebServer server(80);

    WifiControlApiService::handleMute(
        server,
        []([[maybe_unused]] const char* cmd, [[maybe_unused]] bool value) { return true; },
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing state parameter\""));
}

void test_mute_invokes_command() {
    WebServer server(80);
    server.setArg("state", "1");
    String capturedCommand;
    bool capturedValue = false;

    WifiControlApiService::handleMute(
        server,
        [&capturedCommand, &capturedValue](const char* cmd, bool value) {
            capturedCommand = cmd;
            capturedValue = value;
            return true;
        },
        []() { return true; });

    TEST_ASSERT_EQUAL_STRING("mute", capturedCommand.c_str());
    TEST_ASSERT_TRUE(capturedValue);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"muted\":true"));
}

void test_dark_mode_rate_limited_short_circuits() {
    WebServer server(80);
    server.setArg("state", "false");
    int commandCalls = 0;

    WifiControlApiService::handleDarkMode(
        server,
        [&commandCalls]([[maybe_unused]] const char* cmd, [[maybe_unused]] bool value) {
            commandCalls++;
            return true;
        },
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, commandCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_mute_rate_limited_short_circuits() {
    WebServer server(80);
    server.setArg("state", "true");
    int commandCalls = 0;

    WifiControlApiService::handleMute(
        server,
        [&commandCalls]([[maybe_unused]] const char* cmd, [[maybe_unused]] bool value) {
            commandCalls++;
            return true;
        },
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, commandCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_profile_push_requires_v1_connection);
    RUN_TEST(test_profile_push_reports_missing_callback);
    RUN_TEST(test_profile_push_reports_queued);
    RUN_TEST(test_profile_push_rate_limited_short_circuits);
    RUN_TEST(test_dark_mode_missing_state_param);
    RUN_TEST(test_dark_mode_inverts_display_command);
    RUN_TEST(test_mute_missing_state_param);
    RUN_TEST(test_mute_invokes_command);
    RUN_TEST(test_dark_mode_rate_limited_short_circuits);
    RUN_TEST(test_mute_rate_limited_short_circuits);
    return UNITY_END();
}

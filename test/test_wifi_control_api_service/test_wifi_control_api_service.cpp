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

// --- Test context helpers ---

struct PushCtx {
    int calls = 0;
    WifiControlApiService::ProfilePushResult result =
        WifiControlApiService::ProfilePushResult::QUEUED;
};

struct RateLimitCtx {
    int calls = 0;
    bool allow = true;
};

struct V1CommandCtx {
    int calls = 0;
    String capturedCommand;
    bool capturedValue = false;
    bool returnValue = true;
};

static WifiControlApiService::ProfilePushResult doPush(void* ctx) {
    auto* c = static_cast<PushCtx*>(ctx);
    c->calls++;
    return c->result;
}

static bool doRateLimit(void* ctx) {
    auto* c = static_cast<RateLimitCtx*>(ctx);
    c->calls++;
    return c->allow;
}

static bool doV1Command(const char* cmd, bool val, void* ctx) {
    auto* c = static_cast<V1CommandCtx*>(ctx);
    c->calls++;
    c->capturedCommand = cmd;
    c->capturedValue = val;
    return c->returnValue;
}

// ---

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_profile_push_requires_v1_connection() {
    WebServer server(80);
    PushCtx pushCtx;

    WifiControlApiService::handleApiProfilePush(
        server, false,
        doPush, &pushCtx,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"V1 not connected\""));
}

void test_profile_push_reports_missing_callback() {
    WebServer server(80);

    WifiControlApiService::handleApiProfilePush(
        server, true,
        nullptr, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Push handler unavailable\""));
}

void test_profile_push_reports_queued() {
    WebServer server(80);
    PushCtx pushCtx;

    WifiControlApiService::handleApiProfilePush(
        server, true,
        doPush, &pushCtx,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "Profile push queued - check display for progress"));
}

void test_profile_push_rate_limited_short_circuits() {
    WebServer server(80);
    PushCtx pushCtx;

    WifiControlApiService::handleApiProfilePush(
        server, true,
        doPush, &pushCtx,
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, pushCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_api_profile_push_rate_limited_short_circuits_on_route_guard() {
    WebServer server(80);
    PushCtx pushCtx;
    RateLimitCtx rateLimitCtx{0, false};

    WifiControlApiService::handleApiProfilePush(
        server, true,
        doPush, &pushCtx,
        doRateLimit, &rateLimitCtx);

    TEST_ASSERT_EQUAL_INT(1, rateLimitCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, pushCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_api_profile_push_preserves_double_rate_limit_behavior() {
    WebServer server(80);
    PushCtx pushCtx;
    RateLimitCtx rateLimitCtx{0, true};

    WifiControlApiService::handleApiProfilePush(
        server, false,
        doPush, &pushCtx,
        doRateLimit, &rateLimitCtx);

    TEST_ASSERT_EQUAL_INT(2, rateLimitCtx.calls);
    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"V1 not connected\""));
}

void test_profile_push_reports_already_in_progress() {
    WebServer server(80);
    PushCtx pushCtx;
    pushCtx.result = WifiControlApiService::ProfilePushResult::ALREADY_IN_PROGRESS;

    WifiControlApiService::handleApiProfilePush(
        server, true,
        doPush, &pushCtx,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Push already in progress\""));
}

void test_dark_mode_missing_state_param() {
    WebServer server(80);
    V1CommandCtx cmdCtx;

    WifiControlApiService::handleApiDarkMode(
        server,
        doV1Command, &cmdCtx,
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing state parameter\""));
}

void test_dark_mode_inverts_display_command() {
    WebServer server(80);
    server.setArg("state", "true");
    V1CommandCtx cmdCtx;

    WifiControlApiService::handleApiDarkMode(
        server,
        doV1Command, &cmdCtx,
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_STRING("display", cmdCtx.capturedCommand.c_str());
    TEST_ASSERT_FALSE(cmdCtx.capturedValue);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"darkMode\":true"));
}

void test_mute_missing_state_param() {
    WebServer server(80);
    V1CommandCtx cmdCtx;

    WifiControlApiService::handleApiMute(
        server,
        doV1Command, &cmdCtx,
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing state parameter\""));
}

void test_mute_invokes_command() {
    WebServer server(80);
    server.setArg("state", "1");
    V1CommandCtx cmdCtx;

    WifiControlApiService::handleApiMute(
        server,
        doV1Command, &cmdCtx,
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_STRING("mute", cmdCtx.capturedCommand.c_str());
    TEST_ASSERT_TRUE(cmdCtx.capturedValue);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"muted\":true"));
}

void test_dark_mode_rate_limited_short_circuits() {
    WebServer server(80);
    server.setArg("state", "false");
    V1CommandCtx cmdCtx;

    WifiControlApiService::handleApiDarkMode(
        server,
        doV1Command, &cmdCtx,
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, cmdCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_mute_rate_limited_short_circuits() {
    WebServer server(80);
    server.setArg("state", "true");
    V1CommandCtx cmdCtx;

    WifiControlApiService::handleApiMute(
        server,
        doV1Command, &cmdCtx,
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, cmdCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_profile_push_requires_v1_connection);
    RUN_TEST(test_profile_push_reports_missing_callback);
    RUN_TEST(test_profile_push_reports_queued);
    RUN_TEST(test_profile_push_rate_limited_short_circuits);
    RUN_TEST(test_api_profile_push_rate_limited_short_circuits_on_route_guard);
    RUN_TEST(test_api_profile_push_preserves_double_rate_limit_behavior);
    RUN_TEST(test_profile_push_reports_already_in_progress);
    RUN_TEST(test_dark_mode_missing_state_param);
    RUN_TEST(test_dark_mode_inverts_display_command);
    RUN_TEST(test_mute_missing_state_param);
    RUN_TEST(test_mute_invokes_command);
    RUN_TEST(test_dark_mode_rate_limited_short_circuits);
    RUN_TEST(test_mute_rate_limited_short_circuits);
    return UNITY_END();
}

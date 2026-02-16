#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_portal_api_service.h"
#include "../../src/modules/wifi/wifi_portal_api_service.cpp"  // Pull implementation for UNIT_TEST.

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

void test_ping_marks_ui_activity_and_returns_ok() {
    WebServer server(80);
    int uiActivityCalls = 0;

    WifiPortalApiService::handlePing(
        server,
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/plain", server.lastContentType.c_str());
    TEST_ASSERT_TRUE(responseContains(server, "OK"));
}

void test_generate_204_marks_ui_activity_and_returns_empty_204() {
    WebServer server(80);
    int uiActivityCalls = 0;

    WifiPortalApiService::handleGenerate204(
        server,
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(204, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/plain", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("", server.lastBody.c_str());
}

void test_gen_204_marks_ui_activity_and_returns_empty_204() {
    WebServer server(80);
    int uiActivityCalls = 0;

    WifiPortalApiService::handleGen204(
        server,
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(204, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/plain", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("", server.lastBody.c_str());
}

void test_hotspot_detect_marks_ui_activity_and_redirects_to_settings() {
    WebServer server(80);
    int uiActivityCalls = 0;

    WifiPortalApiService::handleHotspotDetect(
        server,
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_STRING("/settings", server.sentHeader("Location").c_str());
    TEST_ASSERT_EQUAL_INT(302, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/html", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("", server.lastBody.c_str());
}

void test_fwlink_redirects_to_settings() {
    WebServer server(80);

    WifiPortalApiService::handleFwlink(server);

    TEST_ASSERT_EQUAL_STRING("/settings", server.sentHeader("Location").c_str());
    TEST_ASSERT_EQUAL_INT(302, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/html", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("", server.lastBody.c_str());
}

void test_ncsi_returns_expected_body() {
    WebServer server(80);

    WifiPortalApiService::handleNcsiTxt(server);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/plain", server.lastContentType.c_str());
    TEST_ASSERT_TRUE(responseContains(server, "Microsoft NCSI"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ping_marks_ui_activity_and_returns_ok);
    RUN_TEST(test_generate_204_marks_ui_activity_and_returns_empty_204);
    RUN_TEST(test_gen_204_marks_ui_activity_and_returns_empty_204);
    RUN_TEST(test_hotspot_detect_marks_ui_activity_and_redirects_to_settings);
    RUN_TEST(test_fwlink_redirects_to_settings);
    RUN_TEST(test_ncsi_returns_expected_body);
    return UNITY_END();
}

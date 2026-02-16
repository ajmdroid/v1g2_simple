#include <unity.h>
#include <cstring>

#include "../../src/modules/camera/camera_api_service.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

class CameraRuntimeModule {};
class StorageManager {};

namespace {

int sendStatusCalls = 0;
int sendCatalogCalls = 0;
int sendEventsCalls = 0;
int handleDemoCalls = 0;
int handleDemoClearCalls = 0;

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

}  // namespace

namespace CameraApiService {

void sendStatus(WebServer& server, CameraRuntimeModule&) {
    sendStatusCalls++;
    server.send(200, "application/json", "{\"route\":\"camera-status\"}");
}

void sendCatalog(WebServer& server, StorageManager&) {
    sendCatalogCalls++;
    server.send(200, "application/json", "{\"route\":\"camera-catalog\"}");
}

void sendEvents(WebServer& server, CameraRuntimeModule&) {
    sendEventsCalls++;
    server.send(200, "application/json", "{\"route\":\"camera-events\"}");
}

void handleDemo(WebServer& server) {
    handleDemoCalls++;
    server.send(200, "application/json", "{\"route\":\"camera-demo\"}");
}

void handleDemoClear(WebServer& server) {
    handleDemoClearCalls++;
    server.send(200, "application/json", "{\"route\":\"camera-demo-clear\"}");
}

}  // namespace CameraApiService

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;

    sendStatusCalls = 0;
    sendCatalogCalls = 0;
    sendEventsCalls = 0;
    handleDemoCalls = 0;
    handleDemoClearCalls = 0;
}

void tearDown() {}

void test_handle_api_status_rate_limited_short_circuits() {
    WebServer server(80);
    CameraRuntimeModule runtime;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    CameraApiService::handleApiStatus(
        server,
        runtime,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, sendStatusCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_status_delegates_when_allowed() {
    WebServer server(80);
    CameraRuntimeModule runtime;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    CameraApiService::handleApiStatus(
        server,
        runtime,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendStatusCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"camera-status\""));
}

void test_handle_api_catalog_delegates_when_allowed() {
    WebServer server(80);
    StorageManager storage;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    CameraApiService::handleApiCatalog(
        server,
        storage,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendCatalogCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"camera-catalog\""));
}

void test_handle_api_events_delegates_when_allowed() {
    WebServer server(80);
    CameraRuntimeModule runtime;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    CameraApiService::handleApiEvents(
        server,
        runtime,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendEventsCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"camera-events\""));
}

void test_handle_api_demo_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    CameraApiService::handleApiDemo(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handleDemoCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"camera-demo\""));
}

void test_handle_api_demo_clear_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    CameraApiService::handleApiDemoClear(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handleDemoClearCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"camera-demo-clear\""));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_api_status_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_status_delegates_when_allowed);
    RUN_TEST(test_handle_api_catalog_delegates_when_allowed);
    RUN_TEST(test_handle_api_events_delegates_when_allowed);
    RUN_TEST(test_handle_api_demo_delegates_when_allowed);
    RUN_TEST(test_handle_api_demo_clear_delegates_when_allowed);
    return UNITY_END();
}

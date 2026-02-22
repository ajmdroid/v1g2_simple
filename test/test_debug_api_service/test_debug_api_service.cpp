#include <unity.h>
#include <cstring>

#include "../../src/modules/debug/debug_api_service.h"
#include "../support/wrappers/debug_api_service_wrappers.cpp"  // Pull wrappers for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

int sendMetricsCalls = 0;
int sendPanicCalls = 0;
int handleDebugEnableCalls = 0;
int handleMetricsResetCalls = 0;
int sendPerfFilesListCalls = 0;
int handlePerfFileDownloadCalls = 0;
int handlePerfFileDeleteCalls = 0;

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

}  // namespace

namespace DebugApiService {

void sendMetrics(WebServer& server) {
    sendMetricsCalls++;
    server.send(200, "application/json", "{\"route\":\"metrics\"}");
}

void handleDebugEnable(WebServer& server) {
    handleDebugEnableCalls++;
    server.send(200, "application/json", "{\"route\":\"enable\"}");
}

void handleMetricsReset(WebServer& server) {
    handleMetricsResetCalls++;
    server.send(200, "application/json", "{\"route\":\"metrics-reset\"}");
}

void sendPanic(WebServer& server) {
    sendPanicCalls++;
    server.send(200, "application/json", "{\"route\":\"panic\"}");
}

void sendPerfFilesList(WebServer& server) {
    sendPerfFilesListCalls++;
    server.send(200, "application/json", "{\"route\":\"perf-files\"}");
}

void handlePerfFileDownload(WebServer& server) {
    handlePerfFileDownloadCalls++;
    server.send(200, "application/json", "{\"route\":\"perf-download\"}");
}

void handlePerfFileDelete(WebServer& server) {
    handlePerfFileDeleteCalls++;
    server.send(200, "application/json", "{\"route\":\"perf-delete\"}");
}

}  // namespace DebugApiService

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;

    sendMetricsCalls = 0;
    sendPanicCalls = 0;
    handleDebugEnableCalls = 0;
    handleMetricsResetCalls = 0;
    sendPerfFilesListCalls = 0;
    handlePerfFileDownloadCalls = 0;
    handlePerfFileDeleteCalls = 0;
}

void tearDown() {}

void test_handle_api_metrics_delegates() {
    WebServer server(80);

    DebugApiService::handleApiMetrics(server);

    TEST_ASSERT_EQUAL_INT(1, sendMetricsCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"metrics\""));
}

void test_handle_api_panic_delegates() {
    WebServer server(80);

    DebugApiService::handleApiPanic(server);

    TEST_ASSERT_EQUAL_INT(1, sendPanicCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"panic\""));
}

void test_handle_api_debug_enable_rate_limited_short_circuits() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiDebugEnable(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, handleDebugEnableCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_debug_enable_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiDebugEnable(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, handleDebugEnableCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enable\""));
}

void test_handle_api_metrics_reset_rate_limited_short_circuits() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiMetricsReset(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, handleMetricsResetCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_metrics_reset_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiMetricsReset(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, handleMetricsResetCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"metrics-reset\""));
}

void test_handle_api_perf_files_list_rate_limited_short_circuits() {
    WebServer server(80);
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    DebugApiService::handleApiPerfFilesList(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, sendPerfFilesListCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_perf_files_list_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    DebugApiService::handleApiPerfFilesList(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendPerfFilesListCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"perf-files\""));
}

void test_handle_api_perf_files_download_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    DebugApiService::handleApiPerfFilesDownload(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handlePerfFileDownloadCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"perf-download\""));
}

void test_handle_api_perf_files_delete_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    DebugApiService::handleApiPerfFilesDelete(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handlePerfFileDeleteCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"perf-delete\""));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_api_metrics_delegates);
    RUN_TEST(test_handle_api_panic_delegates);
    RUN_TEST(test_handle_api_debug_enable_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_debug_enable_delegates_when_allowed);
    RUN_TEST(test_handle_api_metrics_reset_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_metrics_reset_delegates_when_allowed);
    RUN_TEST(test_handle_api_perf_files_list_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_perf_files_list_delegates_when_allowed);
    RUN_TEST(test_handle_api_perf_files_download_delegates_when_allowed);
    RUN_TEST(test_handle_api_perf_files_delete_delegates_when_allowed);
    return UNITY_END();
}

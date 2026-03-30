#include <unity.h>
#include <cstring>

#include "../../src/perf_metrics.h"
#include "../../src/modules/debug/debug_api_service.h"
#include "../support/wrappers/debug_api_service_wrappers.cpp"  // Pull wrappers for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

int sendMetricsCalls = 0;
int sendPanicCalls = 0;
int sendV1ScenarioListCalls = 0;
int sendV1ScenarioStatusCalls = 0;
int handleDebugEnableCalls = 0;
int handleMetricsResetCalls = 0;
int handleV1ScenarioLoadCalls = 0;
int handleV1ScenarioStartCalls = 0;
int handleV1ScenarioStopCalls = 0;
int sendPerfFilesListCalls = 0;
int handlePerfFileDownloadCalls = 0;
int handlePerfFileDeleteCalls = 0;

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

}  // namespace

// --- Test context helpers ---

struct RateLimitCtx {
    int calls = 0;
    bool allow = true;
};

struct UiActivityCtx {
    int calls = 0;
};

static bool doRateLimit(void* ctx) {
    auto* c = static_cast<RateLimitCtx*>(ctx);
    c->calls++;
    return c->allow;
}

static void doUiActivity(void* ctx) {
    static_cast<UiActivityCtx*>(ctx)->calls++;
}

// ---

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

void sendV1ScenarioList(WebServer& server) {
    sendV1ScenarioListCalls++;
    server.send(200, "application/json", "{\"route\":\"v1-scenario-list\"}");
}

void sendV1ScenarioStatus(WebServer& server) {
    sendV1ScenarioStatusCalls++;
    server.send(200, "application/json", "{\"route\":\"v1-scenario-status\"}");
}

void handleV1ScenarioLoad(WebServer& server) {
    handleV1ScenarioLoadCalls++;
    server.send(200, "application/json", "{\"route\":\"v1-scenario-load\"}");
}

void handleV1ScenarioStart(WebServer& server) {
    handleV1ScenarioStartCalls++;
    server.send(200, "application/json", "{\"route\":\"v1-scenario-start\"}");
}

void handleV1ScenarioStop(WebServer& server) {
    handleV1ScenarioStopCalls++;
    server.send(200, "application/json", "{\"route\":\"v1-scenario-stop\"}");
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
    sendV1ScenarioListCalls = 0;
    sendV1ScenarioStatusCalls = 0;
    handleDebugEnableCalls = 0;
    handleMetricsResetCalls = 0;
    handleV1ScenarioLoadCalls = 0;
    handleV1ScenarioStartCalls = 0;
    handleV1ScenarioStopCalls = 0;
    sendPerfFilesListCalls = 0;
    handlePerfFileDownloadCalls = 0;
    handlePerfFileDeleteCalls = 0;
    perfCounters.reset();
    perfExtended.reset();
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

void test_handle_api_v1_scenario_list_delegates() {
    WebServer server(80);

    DebugApiService::handleApiV1ScenarioList(server);

    TEST_ASSERT_EQUAL_INT(1, sendV1ScenarioListCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"v1-scenario-list\""));
}

void test_handle_api_v1_scenario_status_delegates() {
    WebServer server(80);

    DebugApiService::handleApiV1ScenarioStatus(server);

    TEST_ASSERT_EQUAL_INT(1, sendV1ScenarioStatusCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"v1-scenario-status\""));
}

void test_handle_api_debug_enable_rate_limited_short_circuits() {
    WebServer server(80);
    RateLimitCtx rlCtx{ .allow = false };

    DebugApiService::handleApiDebugEnable(
        server,
        doRateLimit, &rlCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, handleDebugEnableCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_debug_enable_delegates_when_allowed() {
    WebServer server(80);
    RateLimitCtx rlCtx;

    DebugApiService::handleApiDebugEnable(
        server,
        doRateLimit, &rlCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, handleDebugEnableCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enable\""));
}

void test_handle_api_metrics_reset_rate_limited_short_circuits() {
    WebServer server(80);
    RateLimitCtx rlCtx{ .allow = false };

    DebugApiService::handleApiMetricsReset(
        server,
        doRateLimit, &rlCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, handleMetricsResetCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_metrics_reset_delegates_when_allowed() {
    WebServer server(80);
    RateLimitCtx rlCtx;

    DebugApiService::handleApiMetricsReset(
        server,
        doRateLimit, &rlCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, handleMetricsResetCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"metrics-reset\""));
}

void test_handle_api_v1_scenario_load_rate_limited_short_circuits() {
    WebServer server(80);
    RateLimitCtx rlCtx{ .allow = false };

    DebugApiService::handleApiV1ScenarioLoad(
        server,
        doRateLimit, &rlCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, handleV1ScenarioLoadCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_v1_scenario_load_delegates_when_allowed() {
    WebServer server(80);
    RateLimitCtx rlCtx;

    DebugApiService::handleApiV1ScenarioLoad(
        server,
        doRateLimit, &rlCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, handleV1ScenarioLoadCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"v1-scenario-load\""));
}

void test_handle_api_v1_scenario_start_delegates_when_allowed() {
    WebServer server(80);
    RateLimitCtx rlCtx;

    DebugApiService::handleApiV1ScenarioStart(
        server,
        doRateLimit, &rlCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, handleV1ScenarioStartCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"v1-scenario-start\""));
}

void test_handle_api_v1_scenario_stop_delegates_when_allowed() {
    WebServer server(80);
    RateLimitCtx rlCtx;

    DebugApiService::handleApiV1ScenarioStop(
        server,
        doRateLimit, &rlCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, handleV1ScenarioStopCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"v1-scenario-stop\""));
}

void test_handle_api_perf_files_list_rate_limited_short_circuits() {
    WebServer server(80);
    RateLimitCtx rlCtx{ .allow = false };
    UiActivityCtx uiCtx;

    DebugApiService::handleApiPerfFilesList(
        server,
        doRateLimit, &rlCtx,
        doUiActivity, &uiCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, uiCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, sendPerfFilesListCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_perf_files_list_delegates_when_allowed() {
    WebServer server(80);
    RateLimitCtx rlCtx;
    UiActivityCtx uiCtx;

    DebugApiService::handleApiPerfFilesList(
        server,
        doRateLimit, &rlCtx,
        doUiActivity, &uiCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, sendPerfFilesListCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"perf-files\""));
}

void test_handle_api_perf_files_download_delegates_when_allowed() {
    WebServer server(80);
    RateLimitCtx rlCtx;
    UiActivityCtx uiCtx;

    DebugApiService::handleApiPerfFilesDownload(
        server,
        doRateLimit, &rlCtx,
        doUiActivity, &uiCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, handlePerfFileDownloadCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"perf-download\""));
}

void test_handle_api_perf_files_delete_delegates_when_allowed() {
    WebServer server(80);
    RateLimitCtx rlCtx;
    UiActivityCtx uiCtx;

    DebugApiService::handleApiPerfFilesDelete(
        server,
        doRateLimit, &rlCtx,
        doUiActivity, &uiCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, handlePerfFileDeleteCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"perf-delete\""));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_api_metrics_delegates);
    RUN_TEST(test_handle_api_panic_delegates);
    RUN_TEST(test_handle_api_v1_scenario_list_delegates);
    RUN_TEST(test_handle_api_v1_scenario_status_delegates);
    RUN_TEST(test_handle_api_debug_enable_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_debug_enable_delegates_when_allowed);
    RUN_TEST(test_handle_api_metrics_reset_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_metrics_reset_delegates_when_allowed);
    RUN_TEST(test_handle_api_v1_scenario_load_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_v1_scenario_load_delegates_when_allowed);
    RUN_TEST(test_handle_api_v1_scenario_start_delegates_when_allowed);
    RUN_TEST(test_handle_api_v1_scenario_stop_delegates_when_allowed);
    RUN_TEST(test_handle_api_perf_files_list_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_perf_files_list_delegates_when_allowed);
    RUN_TEST(test_handle_api_perf_files_download_delegates_when_allowed);
    RUN_TEST(test_handle_api_perf_files_delete_delegates_when_allowed);
    return UNITY_END();
}

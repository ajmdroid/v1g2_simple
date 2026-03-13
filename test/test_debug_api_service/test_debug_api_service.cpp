#include <unity.h>
#include <cstring>

#include "../../src/perf_metrics.h"
#include "../../src/modules/debug/debug_api_service.h"
#include "../../src/modules/debug/debug_metrics_payload.cpp"
#include "../support/wrappers/debug_api_service_wrappers.cpp"  // Pull wrappers for UNIT_TEST.

// ── Stubs for symbols introduced by debug_metrics_payload.cpp ──
V1BLEClient::V1BLEClient() {}
V1BLEClient::~V1BLEClient() {}
bool V1BLEClient::isProxyAdvertising() const { return false; }
const char* V1BLEClient::getSubscribeStepName() const { return "STUB"; }
void V1BLEClient::releaseProxyQueues() {}
V1BLEClient bleClient;
WifiAutoStartModule wifiAutoStartModule;
const char* wifiAutoStartGateName(WifiAutoStartGate) { return "stub"; }
uint32_t perfGetProxyAdvertisingLastTransitionReason() { return 0; }
const char* perfProxyAdvertisingTransitionReasonName(uint32_t) { return "stub"; }

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
int handleCameraAlertRenderCalls = 0;
int handleCameraAlertClearCalls = 0;
int handleV1ScenarioLoadCalls = 0;
int handleV1ScenarioStartCalls = 0;
int handleV1ScenarioStopCalls = 0;
int sendPerfFilesListCalls = 0;
int handlePerfFileDownloadCalls = 0;
int handlePerfFileDeleteCalls = 0;

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

void assertCameraMetricsPayload(const JsonDocument& doc,
                                uint32_t cameraDisplayActive,
                                uint32_t cameraDebugOverrideActive,
                                uint32_t cameraDisplayFrames,
                                uint32_t cameraDebugDisplayFrames,
                                uint32_t cameraDisplayMaxUs,
                                uint32_t cameraDebugDisplayMaxUs,
                                uint32_t cameraProcessMaxUs) {
    TEST_ASSERT_FALSE(doc["cameraDisplayActive"].isNull());
    TEST_ASSERT_FALSE(doc["cameraDebugOverrideActive"].isNull());
    TEST_ASSERT_FALSE(doc["cameraDisplayFrames"].isNull());
    TEST_ASSERT_FALSE(doc["cameraDebugDisplayFrames"].isNull());
    TEST_ASSERT_FALSE(doc["cameraDisplayMaxUs"].isNull());
    TEST_ASSERT_FALSE(doc["cameraDebugDisplayMaxUs"].isNull());
    TEST_ASSERT_FALSE(doc["cameraProcessMaxUs"].isNull());
    TEST_ASSERT_TRUE(doc["cameraVoiceQueued"].isNull());
    TEST_ASSERT_TRUE(doc["cameraVoiceStarted"].isNull());
    TEST_ASSERT_EQUAL_UINT32(cameraDisplayActive, doc["cameraDisplayActive"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(cameraDebugOverrideActive, doc["cameraDebugOverrideActive"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(cameraDisplayFrames, doc["cameraDisplayFrames"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(cameraDebugDisplayFrames, doc["cameraDebugDisplayFrames"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(cameraDisplayMaxUs, doc["cameraDisplayMaxUs"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(cameraDebugDisplayMaxUs, doc["cameraDebugDisplayMaxUs"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(cameraProcessMaxUs, doc["cameraProcessMaxUs"].as<uint32_t>());
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

void handleCameraAlertRender(WebServer& server) {
    handleCameraAlertRenderCalls++;
    server.send(200, "application/json", "{\"route\":\"camera-alert-render\"}");
}

void handleCameraAlertClear(WebServer& server) {
    handleCameraAlertClearCalls++;
    server.send(200, "application/json", "{\"route\":\"camera-alert-clear\"}");
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
    handleCameraAlertRenderCalls = 0;
    handleCameraAlertClearCalls = 0;
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

void test_handle_api_camera_alert_render_rate_limited_short_circuits() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiCameraAlertRender(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, handleCameraAlertRenderCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_camera_alert_render_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiCameraAlertRender(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, handleCameraAlertRenderCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"camera-alert-render\""));
}

void test_handle_api_camera_alert_clear_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiCameraAlertClear(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, handleCameraAlertClearCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"camera-alert-clear\""));
}

void test_handle_api_v1_scenario_load_rate_limited_short_circuits() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiV1ScenarioLoad(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, handleV1ScenarioLoadCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_v1_scenario_load_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiV1ScenarioLoad(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, handleV1ScenarioLoadCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"v1-scenario-load\""));
}

void test_handle_api_v1_scenario_start_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiV1ScenarioStart(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, handleV1ScenarioStartCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"v1-scenario-start\""));
}

void test_handle_api_v1_scenario_stop_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;

    DebugApiService::handleApiV1ScenarioStop(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, handleV1ScenarioStopCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"v1-scenario-stop\""));
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

void test_append_camera_metrics_payload_for_normal_metrics_shape() {
    perfCounters.cameraDisplayActive = 1;
    perfCounters.cameraDebugOverrideActive = 0;
    perfCounters.cameraDisplayFrames = 12;
    perfCounters.cameraDebugDisplayFrames = 3;
    perfExtended.cameraDisplayMaxUs = 60123;
    perfExtended.cameraDebugDisplayMaxUs = 32100;
    perfExtended.cameraProcessMaxUs = 7654;

    JsonDocument doc;
    doc["rxPackets"] = 10;
    doc["displayUpdates"] = 20;

    DebugApiService::appendCameraMetricsPayload(doc);

    assertCameraMetricsPayload(doc, 1, 0, 12, 3, 60123, 32100, 7654);
}

void test_append_camera_metrics_payload_for_soak_metrics_shape() {
    perfCounters.cameraDisplayActive = 0;
    perfCounters.cameraDebugOverrideActive = 1;
    perfCounters.cameraDisplayFrames = 7;
    perfCounters.cameraDebugDisplayFrames = 19;
    perfExtended.cameraDisplayMaxUs = 44000;
    perfExtended.cameraDebugDisplayMaxUs = 55000;
    perfExtended.cameraProcessMaxUs = 9876;

    JsonDocument doc;
    doc["queueDrops"] = 0;
    doc["dispPipeMaxUs"] = 4321;

    DebugApiService::appendCameraMetricsPayload(doc);

    assertCameraMetricsPayload(doc, 0, 1, 7, 19, 44000, 55000, 9876);
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
    RUN_TEST(test_handle_api_camera_alert_render_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_camera_alert_render_delegates_when_allowed);
    RUN_TEST(test_handle_api_camera_alert_clear_delegates_when_allowed);
    RUN_TEST(test_handle_api_v1_scenario_load_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_v1_scenario_load_delegates_when_allowed);
    RUN_TEST(test_handle_api_v1_scenario_start_delegates_when_allowed);
    RUN_TEST(test_handle_api_v1_scenario_stop_delegates_when_allowed);
    RUN_TEST(test_handle_api_perf_files_list_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_perf_files_list_delegates_when_allowed);
    RUN_TEST(test_handle_api_perf_files_download_delegates_when_allowed);
    RUN_TEST(test_handle_api_perf_files_delete_delegates_when_allowed);
    RUN_TEST(test_append_camera_metrics_payload_for_normal_metrics_shape);
    RUN_TEST(test_append_camera_metrics_payload_for_soak_metrics_shape);
    return UNITY_END();
}

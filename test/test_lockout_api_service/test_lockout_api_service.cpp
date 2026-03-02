#include <unity.h>
#include <cstring>

#include "../../src/modules/lockout/lockout_api_service.h"
#include "../support/wrappers/lockout_api_service_wrappers.cpp"  // Pull wrappers for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

class LockoutIndex {};
class LockoutStore {};
class LockoutLearner {};
class SignalObservationLog {};
class SignalObservationSdLogger {};
class SettingsManager {};

namespace {

int sendSummaryCalls = 0;
int sendEventsCalls = 0;
int sendZonesCalls = 0;
int handleZoneDeleteCalls = 0;
int handleZoneCreateCalls = 0;
int handleZoneUpdateCalls = 0;
int sendZoneExportCalls = 0;
int handleZoneImportCalls = 0;

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

}  // namespace

namespace LockoutApiService {

void sendSummary(WebServer& server,
                 SignalObservationLog&,
                 SignalObservationSdLogger&) {
    sendSummaryCalls++;
    server.send(200, "application/json", "{\"route\":\"lockout-summary\"}");
}

void sendEvents(WebServer& server,
                SignalObservationLog&,
                SignalObservationSdLogger&) {
    sendEventsCalls++;
    server.send(200, "application/json", "{\"route\":\"lockout-events\"}");
}

void sendZones(WebServer& server,
               LockoutIndex&,
               LockoutLearner&,
               SettingsManager&) {
    sendZonesCalls++;
    server.send(200, "application/json", "{\"route\":\"lockout-zones\"}");
}

void handleZoneDelete(WebServer& server,
                      LockoutIndex&,
                      LockoutStore&) {
    handleZoneDeleteCalls++;
    server.send(200, "application/json", "{\"route\":\"lockout-delete\"}");
}

void handleZoneCreate(WebServer& server,
                      LockoutIndex&,
                      LockoutStore&) {
    handleZoneCreateCalls++;
    server.send(200, "application/json", "{\"route\":\"lockout-create\"}");
}

void handleZoneUpdate(WebServer& server,
                      LockoutIndex&,
                      LockoutStore&) {
    handleZoneUpdateCalls++;
    server.send(200, "application/json", "{\"route\":\"lockout-update\"}");
}

void sendZoneExport(WebServer& server,
                    LockoutStore&) {
    sendZoneExportCalls++;
    server.send(200, "application/json", "{\"route\":\"lockout-export\"}");
}

void handleZoneImport(WebServer& server,
                      LockoutIndex&,
                      LockoutStore&) {
    handleZoneImportCalls++;
    server.send(200, "application/json", "{\"route\":\"lockout-import\"}");
}

}  // namespace LockoutApiService

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;

    sendSummaryCalls = 0;
    sendEventsCalls = 0;
    sendZonesCalls = 0;
    handleZoneDeleteCalls = 0;
    handleZoneCreateCalls = 0;
    handleZoneUpdateCalls = 0;
    sendZoneExportCalls = 0;
    handleZoneImportCalls = 0;
}

void tearDown() {}

void test_handle_api_summary_rate_limited_short_circuits() {
    WebServer server(80);
    SignalObservationLog signalLog;
    SignalObservationSdLogger sdLogger;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    LockoutApiService::handleApiSummary(
        server,
        signalLog,
        sdLogger,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, sendSummaryCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_summary_delegates_when_allowed() {
    WebServer server(80);
    SignalObservationLog signalLog;
    SignalObservationSdLogger sdLogger;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    LockoutApiService::handleApiSummary(
        server,
        signalLog,
        sdLogger,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendSummaryCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"lockout-summary\""));
}

void test_handle_api_events_delegates_when_allowed() {
    WebServer server(80);
    SignalObservationLog signalLog;
    SignalObservationSdLogger sdLogger;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    LockoutApiService::handleApiEvents(
        server,
        signalLog,
        sdLogger,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendEventsCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"lockout-events\""));
}

void test_handle_api_zones_delegates_when_allowed() {
    WebServer server(80);
    LockoutIndex index;
    LockoutLearner learner;
    SettingsManager settings;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    LockoutApiService::handleApiZones(
        server,
        index,
        learner,
        settings,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendZonesCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"lockout-zones\""));
}

void test_handle_api_zone_delete_delegates_when_allowed() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    LockoutApiService::handleApiZoneDelete(
        server,
        index,
        store,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handleZoneDeleteCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"lockout-delete\""));
}

void test_handle_api_zone_create_delegates_when_allowed() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    LockoutApiService::handleApiZoneCreate(
        server,
        index,
        store,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handleZoneCreateCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"lockout-create\""));
}

void test_handle_api_zone_update_delegates_when_allowed() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    LockoutApiService::handleApiZoneUpdate(
        server,
        index,
        store,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handleZoneUpdateCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"lockout-update\""));
}

void test_handle_api_zone_export_delegates_when_allowed() {
    WebServer server(80);
    LockoutStore store;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    LockoutApiService::handleApiZoneExport(
        server,
        store,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendZoneExportCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"lockout-export\""));
}

void test_handle_api_zone_import_delegates_when_allowed() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    LockoutApiService::handleApiZoneImport(
        server,
        index,
        store,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handleZoneImportCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"lockout-import\""));
}

void test_handle_api_zone_create_rate_limited_short_circuits() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    LockoutApiService::handleApiZoneCreate(
        server,
        index,
        store,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, handleZoneCreateCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_api_summary_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_summary_delegates_when_allowed);
    RUN_TEST(test_handle_api_events_delegates_when_allowed);
    RUN_TEST(test_handle_api_zones_delegates_when_allowed);
    RUN_TEST(test_handle_api_zone_delete_delegates_when_allowed);
    RUN_TEST(test_handle_api_zone_create_delegates_when_allowed);
    RUN_TEST(test_handle_api_zone_update_delegates_when_allowed);
    RUN_TEST(test_handle_api_zone_export_delegates_when_allowed);
    RUN_TEST(test_handle_api_zone_import_delegates_when_allowed);
    RUN_TEST(test_handle_api_zone_create_rate_limited_short_circuits);
    return UNITY_END();
}

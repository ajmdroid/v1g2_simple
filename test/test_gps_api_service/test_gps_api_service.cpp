#include <unity.h>
#include <cstring>

#include "../../src/modules/gps/gps_api_service.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

class GpsRuntimeModule {};
class GpsObservationLog {};
class SpeedSourceSelector {};
class LockoutLearner {};
class SettingsManager {};
struct PerfCounters {};
class SystemEventBus {};

namespace {

int sendStatusCalls = 0;
int sendObservationsCalls = 0;
int handleConfigCalls = 0;

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

}  // namespace

namespace GpsApiService {

void sendStatus(WebServer& server,
                GpsRuntimeModule&,
                SpeedSourceSelector&,
                SettingsManager&,
                GpsObservationLog&,
                LockoutLearner&,
                PerfCounters&,
                SystemEventBus&) {
    sendStatusCalls++;
    server.send(200, "application/json", "{\"route\":\"gps-status\"}");
}

void sendObservations(WebServer& server, GpsObservationLog&) {
    sendObservationsCalls++;
    server.send(200, "application/json", "{\"route\":\"gps-observations\"}");
}

void handleConfig(WebServer& server,
                  SettingsManager&,
                  GpsRuntimeModule&,
                  SpeedSourceSelector&,
                  LockoutLearner&,
                  GpsObservationLog&,
                  PerfCounters&,
                  SystemEventBus&) {
    handleConfigCalls++;
    server.send(200, "application/json", "{\"route\":\"gps-config\"}");
}

}  // namespace GpsApiService

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;

    sendStatusCalls = 0;
    sendObservationsCalls = 0;
    handleConfigCalls = 0;
}

void tearDown() {}

void test_handle_api_status_marks_ui_activity_and_delegates() {
    WebServer server(80);
    GpsRuntimeModule gpsRuntime;
    SpeedSourceSelector speedSelector;
    SettingsManager settings;
    GpsObservationLog gpsLog;
    LockoutLearner lockoutLearner;
    PerfCounters perfCounters;
    SystemEventBus eventBus;
    int uiActivityCalls = 0;

    GpsApiService::handleApiStatus(
        server,
        gpsRuntime,
        speedSelector,
        settings,
        gpsLog,
        lockoutLearner,
        perfCounters,
        eventBus,
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendStatusCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"gps-status\""));
}

void test_handle_api_observations_rate_limited_short_circuits() {
    WebServer server(80);
    GpsObservationLog gpsLog;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    GpsApiService::handleApiObservations(
        server,
        gpsLog,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, sendObservationsCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_observations_delegates_when_allowed() {
    WebServer server(80);
    GpsObservationLog gpsLog;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    GpsApiService::handleApiObservations(
        server,
        gpsLog,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendObservationsCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"gps-observations\""));
}

void test_handle_api_config_rate_limited_short_circuits() {
    WebServer server(80);
    SettingsManager settings;
    GpsRuntimeModule gpsRuntime;
    SpeedSourceSelector speedSelector;
    LockoutLearner lockoutLearner;
    GpsObservationLog gpsLog;
    PerfCounters perfCounters;
    SystemEventBus eventBus;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    GpsApiService::handleApiConfig(
        server,
        settings,
        gpsRuntime,
        speedSelector,
        lockoutLearner,
        gpsLog,
        perfCounters,
        eventBus,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, handleConfigCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_config_delegates_when_allowed() {
    WebServer server(80);
    SettingsManager settings;
    GpsRuntimeModule gpsRuntime;
    SpeedSourceSelector speedSelector;
    LockoutLearner lockoutLearner;
    GpsObservationLog gpsLog;
    PerfCounters perfCounters;
    SystemEventBus eventBus;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    GpsApiService::handleApiConfig(
        server,
        settings,
        gpsRuntime,
        speedSelector,
        lockoutLearner,
        gpsLog,
        perfCounters,
        eventBus,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handleConfigCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"gps-config\""));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_api_status_marks_ui_activity_and_delegates);
    RUN_TEST(test_handle_api_observations_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_observations_delegates_when_allowed);
    RUN_TEST(test_handle_api_config_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_config_delegates_when_allowed);
    return UNITY_END();
}

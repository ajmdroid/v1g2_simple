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
int deprecatedHeaderCalls = 0;

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

}  // namespace LockoutApiService

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;

    sendSummaryCalls = 0;
    sendEventsCalls = 0;
    sendZonesCalls = 0;
    handleZoneDeleteCalls = 0;
    deprecatedHeaderCalls = 0;
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

void test_handle_api_summary_sends_deprecated_header_when_rate_limited() {
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
        [&uiActivityCalls]() { uiActivityCalls++; },
        []() { deprecatedHeaderCalls++; });

    TEST_ASSERT_EQUAL_INT(1, deprecatedHeaderCalls);
    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, sendSummaryCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_zones_with_deprecated_header_delegates_when_allowed() {
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
        [&uiActivityCalls]() { uiActivityCalls++; },
        []() { deprecatedHeaderCalls++; });

    TEST_ASSERT_EQUAL_INT(1, deprecatedHeaderCalls);
    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendZonesCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"lockout-zones\""));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_api_summary_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_summary_delegates_when_allowed);
    RUN_TEST(test_handle_api_events_delegates_when_allowed);
    RUN_TEST(test_handle_api_zones_delegates_when_allowed);
    RUN_TEST(test_handle_api_zone_delete_delegates_when_allowed);
    RUN_TEST(test_handle_api_summary_sends_deprecated_header_when_rate_limited);
    RUN_TEST(test_handle_api_zones_with_deprecated_header_delegates_when_allowed);
    return UNITY_END();
}

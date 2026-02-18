#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_time_api_service.h"
#include "../../src/modules/wifi/wifi_time_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeTimeRuntime {
    bool valid = false;
    int64_t epochMs = 0;
    int32_t tzOffsetMin = 0;
    uint8_t source = 0;
    uint8_t confidence = 0;
    uint32_t monoMs = 0;
    uint32_t ageMs = 0;
    int setCalls = 0;
    int64_t lastSetEpochMs = 0;
    int32_t lastSetTzOffsetMin = 0;
    uint8_t lastSetSource = 0;
};

static WifiTimeApiService::TimeRuntime makeRuntime(FakeTimeRuntime& rt) {
    return WifiTimeApiService::TimeRuntime{
        [&rt]() { return rt.valid; },
        [&rt]() { return rt.epochMs; },
        [&rt]() { return rt.tzOffsetMin; },
        [&rt]() { return rt.source; },
        [&rt](int64_t epochMs, int32_t tzOffsetMin, uint8_t source) {
            rt.setCalls++;
            rt.lastSetEpochMs = epochMs;
            rt.lastSetTzOffsetMin = tzOffsetMin;
            rt.lastSetSource = source;
            rt.valid = true;
            rt.epochMs = epochMs;
            rt.tzOffsetMin = tzOffsetMin;
            rt.source = source;
        },
        [&rt]() { return rt.confidence; },
        [&rt]() { return rt.monoMs; },
        [&rt]() { return rt.ageMs; },
    };
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_time_set_is_disabled_for_valid_payload() {
    WebServer server(80);
    FakeTimeRuntime rt;
    server.setArg("plain", "{\"unixMs\":1700000000001,\"tzOffsetMin\":-300,\"source\":\"client\"}");

    int invalidateCalls = 0;
    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        [&invalidateCalls]() { invalidateCalls++; },
        nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Time set disabled; GPS is authoritative\""));
    TEST_ASSERT_EQUAL_INT(0, rt.setCalls);
    TEST_ASSERT_EQUAL_INT(0, invalidateCalls);
}

void test_time_set_is_disabled_for_legacy_query_payload() {
    WebServer server(80);
    FakeTimeRuntime rt;
    server.setArg("clientEpochMs", "1700000000999");
    server.setArg("tzOffsetMinutes", "60");

    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        []() {},
        nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Time set disabled; GPS is authoritative\""));
    TEST_ASSERT_EQUAL_INT(0, rt.setCalls);
}

void test_time_set_disabled_response_includes_time_snapshot() {
    WebServer server(80);
    FakeTimeRuntime rt;
    rt.valid = true;
    rt.epochMs = 1700000100000LL;
    rt.tzOffsetMin = -300;
    rt.source = 2;
    rt.confidence = 2;
    rt.monoMs = 4567;
    rt.ageMs = 888;

    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        []() {},
        nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"timeValid\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"timeSource\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"timeConfidence\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"epochMs\":1700000100000"));
    TEST_ASSERT_TRUE(responseContains(server, "\"tzOffsetMin\":-300"));
    TEST_ASSERT_TRUE(responseContains(server, "\"monoMs\":4567"));
    TEST_ASSERT_TRUE(responseContains(server, "\"epochAgeMs\":888"));
    TEST_ASSERT_TRUE(responseContains(server, "\"tzOffsetMinutes\":-300"));
}

void test_api_time_set_rate_limited_short_circuits() {
    WebServer server(80);
    FakeTimeRuntime rt;
    server.setArg("plain", "{\"unixMs\":1700000000001}");
    int invalidateCalls = 0;
    int rateLimitCalls = 0;

    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        [&invalidateCalls]() { invalidateCalls++; },
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.setCalls);
    TEST_ASSERT_EQUAL_INT(0, invalidateCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_api_time_set_delegates_when_allowed() {
    WebServer server(80);
    FakeTimeRuntime rt;
    int invalidateCalls = 0;
    int rateLimitCalls = 0;

    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        [&invalidateCalls]() { invalidateCalls++; },
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.setCalls);
    TEST_ASSERT_EQUAL_INT(0, invalidateCalls);
    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Time set disabled; GPS is authoritative\""));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_time_set_is_disabled_for_valid_payload);
    RUN_TEST(test_time_set_is_disabled_for_legacy_query_payload);
    RUN_TEST(test_time_set_disabled_response_includes_time_snapshot);
    RUN_TEST(test_api_time_set_rate_limited_short_circuits);
    RUN_TEST(test_api_time_set_delegates_when_allowed);
    return UNITY_END();
}

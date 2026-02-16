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

void test_time_set_rejects_unsupported_source() {
    WebServer server(80);
    FakeTimeRuntime rt;
    server.setArg("plain", "{\"unixMs\":1700000000001,\"source\":\"gps\"}");

    int invalidateCalls = 0;
    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        [&invalidateCalls]() { invalidateCalls++; },
        nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Unsupported source\""));
    TEST_ASSERT_EQUAL_INT(0, rt.setCalls);
    TEST_ASSERT_EQUAL_INT(0, invalidateCalls);
}

void test_time_set_rejects_missing_unix_ms() {
    WebServer server(80);
    FakeTimeRuntime rt;
    server.setArg("plain", "{\"tzOffsetMin\":-300}");

    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        []() {},
        nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing or invalid unixMs\""));
    TEST_ASSERT_EQUAL_INT(0, rt.setCalls);
}

void test_time_set_rejects_unix_ms_out_of_range() {
    WebServer server(80);
    FakeTimeRuntime rt;
    server.setArg("plain", "{\"unixMs\":1600000000000}");

    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        []() {},
        nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"unixMs out of range\""));
    TEST_ASSERT_EQUAL_INT(0, rt.setCalls);
}

void test_time_set_applies_valid_payload_and_clamps_tz() {
    WebServer server(80);
    FakeTimeRuntime rt;
    rt.confidence = 77;
    rt.monoMs = 12345;
    rt.ageMs = 99;
    server.setArg("plain", "{\"unixMs\":1700000000001,\"tzOffsetMin\":9999}");

    int invalidateCalls = 0;
    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        [&invalidateCalls]() { invalidateCalls++; },
        nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.setCalls);
    TEST_ASSERT_EQUAL_INT64(1700000000001LL, rt.lastSetEpochMs);
    TEST_ASSERT_EQUAL_INT(840, rt.lastSetTzOffsetMin);
    TEST_ASSERT_EQUAL_INT(1, rt.lastSetSource);
    TEST_ASSERT_EQUAL_INT(1, invalidateCalls);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"tzOffsetMin\":840"));
}

void test_time_set_uses_epoch_ms_compatibility_key() {
    WebServer server(80);
    FakeTimeRuntime rt;
    server.setArg("plain", "{\"epochMs\":\"1700000000456\",\"tzOffsetMinutes\":-120}");

    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        []() {},
        nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.setCalls);
    TEST_ASSERT_EQUAL_INT64(1700000000456LL, rt.lastSetEpochMs);
    TEST_ASSERT_EQUAL_INT(-120, rt.lastSetTzOffsetMin);
}

void test_time_set_uses_query_fallback_keys() {
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

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.setCalls);
    TEST_ASSERT_EQUAL_INT64(1700000000999LL, rt.lastSetEpochMs);
    TEST_ASSERT_EQUAL_INT(60, rt.lastSetTzOffsetMin);
}

void test_time_set_near_noop_client_sync_skips_write_and_invalidate() {
    WebServer server(80);
    FakeTimeRuntime rt;
    rt.valid = true;
    rt.epochMs = 1700000002000LL;
    rt.tzOffsetMin = -300;
    rt.source = 1;
    server.setArg("plain", "{\"unixMs\":1700000003000,\"tzOffsetMin\":-300}");

    int invalidateCalls = 0;
    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        [&invalidateCalls]() { invalidateCalls++; },
        nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.setCalls);
    TEST_ASSERT_EQUAL_INT(0, invalidateCalls);
}

void test_time_set_non_noop_writes_and_invalidates() {
    WebServer server(80);
    FakeTimeRuntime rt;
    rt.valid = true;
    rt.epochMs = 1700000002000LL;
    rt.tzOffsetMin = -300;
    rt.source = 1;
    server.setArg("plain", "{\"unixMs\":1700000010005,\"tzOffsetMin\":-300}");

    int invalidateCalls = 0;
    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        [&invalidateCalls]() { invalidateCalls++; },
        nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.setCalls);
    TEST_ASSERT_EQUAL_INT(1, invalidateCalls);
}

void test_time_set_response_includes_backward_compatible_fields() {
    WebServer server(80);
    FakeTimeRuntime rt;
    rt.valid = true;
    rt.epochMs = 1700000100000LL;
    rt.tzOffsetMin = 120;
    rt.source = 1;
    rt.confidence = 2;
    rt.monoMs = 4567;
    rt.ageMs = 888;
    server.setArg("plain", "{\"unixMs\":1700000100000,\"tzOffsetMin\":120}");

    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        []() {},
        nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"timeValid\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"timeSource\":1"));
    TEST_ASSERT_TRUE(responseContains(server, "\"timeConfidence\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"monoMs\":4567"));
    TEST_ASSERT_TRUE(responseContains(server, "\"epochAgeMs\":888"));
    TEST_ASSERT_TRUE(responseContains(server, "\"tzOffsetMinutes\":120"));
}

void test_time_set_query_source_overrides_json_source() {
    WebServer server(80);
    FakeTimeRuntime rt;
    server.setArg("plain", "{\"unixMs\":1700000000001,\"source\":\"client\"}");
    server.setArg("source", "manual");

    WifiTimeApiService::handleApiTimeSet(
        server,
        makeRuntime(rt),
        1,
        []() {},
        nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Unsupported source\""));
    TEST_ASSERT_EQUAL_INT(0, rt.setCalls);
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
    server.setArg("plain", "{\"unixMs\":1700000001234,\"tzOffsetMin\":-60}");
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
    TEST_ASSERT_EQUAL_INT(1, rt.setCalls);
    TEST_ASSERT_EQUAL_INT(1, invalidateCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_time_set_rejects_unsupported_source);
    RUN_TEST(test_time_set_rejects_missing_unix_ms);
    RUN_TEST(test_time_set_rejects_unix_ms_out_of_range);
    RUN_TEST(test_time_set_applies_valid_payload_and_clamps_tz);
    RUN_TEST(test_time_set_uses_epoch_ms_compatibility_key);
    RUN_TEST(test_time_set_uses_query_fallback_keys);
    RUN_TEST(test_time_set_near_noop_client_sync_skips_write_and_invalidate);
    RUN_TEST(test_time_set_non_noop_writes_and_invalidates);
    RUN_TEST(test_time_set_response_includes_backward_compatible_fields);
    RUN_TEST(test_time_set_query_source_overrides_json_source);
    RUN_TEST(test_api_time_set_rate_limited_short_circuits);
    RUN_TEST(test_api_time_set_delegates_when_allowed);
    return UNITY_END();
}

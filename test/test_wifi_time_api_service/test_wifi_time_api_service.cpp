#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_time_api_service.h"
#include "../../src/modules/wifi/wifi_time_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct SetTimeCtx {
    int calls = 0;
    int64_t lastEpochMs = 0;
    int32_t lastTzOffsetMinutes = 0;
    bool returnValue = true;
};

static bool doSetTime(int64_t epochMs, int32_t tzOffsetMinutes, void* ctx) {
    auto* c = static_cast<SetTimeCtx*>(ctx);
    c->calls++;
    c->lastEpochMs = epochMs;
    c->lastTzOffsetMinutes = tzOffsetMinutes;
    return c->returnValue;
}

// A valid epoch in range (~2026-01-01 UTC)
static constexpr int64_t VALID_EPOCH_MS = 1751328000000LL;

// ---------------------------------------------------------------------------
// setUp / tearDown
// ---------------------------------------------------------------------------

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

// ---------------------------------------------------------------------------
// Body / parse errors
// ---------------------------------------------------------------------------

void test_missing_body_returns_400() {
    WebServer server(80);
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Missing JSON body"));
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);
}

void test_empty_body_returns_400() {
    WebServer server(80);
    server.setArg("plain", "");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);
}

void test_invalid_json_returns_400() {
    WebServer server(80);
    server.setArg("plain", "not-json{{");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Invalid JSON"));
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);
}

// ---------------------------------------------------------------------------
// epochMs validation
// ---------------------------------------------------------------------------

void test_missing_epoch_ms_returns_400() {
    WebServer server(80);
    server.setArg("plain", "{\"tzOffsetMinutes\":0}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Missing or invalid epochMs"));
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);
}

void test_epoch_ms_as_string_returns_400() {
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":\"1751328000000\"}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);
}

void test_epoch_ms_as_bool_returns_400() {
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":true}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);
}

void test_epoch_ms_too_small_returns_400() {
    // Below MIN_VALID_UNIX_MS (~2023-11)
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":1000000000000}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "out of valid range"));
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);
}

void test_epoch_ms_too_large_returns_400() {
    // Above MAX_VALID_UNIX_MS (2100-01-01)
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":9999999999999}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "out of valid range"));
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);
}

void test_zero_epoch_ms_returns_400() {
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":0}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, ctx.calls);
}

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

void test_valid_request_returns_200_and_calls_set_time() {
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":1751328000000,\"tzOffsetMinutes\":-240}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, ctx.calls);
    TEST_ASSERT_EQUAL_INT64(1751328000000LL, ctx.lastEpochMs);
    TEST_ASSERT_EQUAL_INT(-240, ctx.lastTzOffsetMinutes);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"source\":1"));
}

void test_valid_request_echoes_epoch_ms_in_response() {
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":1751328000000}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "1751328000000"));
}

void test_missing_tz_offset_defaults_to_zero() {
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":1751328000000}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, ctx.lastTzOffsetMinutes);
}

void test_positive_tz_offset_passed_through() {
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":1751328000000,\"tzOffsetMinutes\":330}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(330, ctx.lastTzOffsetMinutes);
}

// ---------------------------------------------------------------------------
// tzOffsetMinutes clamping
// ---------------------------------------------------------------------------

void test_tz_offset_clamped_to_max() {
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":1751328000000,\"tzOffsetMinutes\":9999}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(840, ctx.lastTzOffsetMinutes);
}

void test_tz_offset_clamped_to_min() {
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":1751328000000,\"tzOffsetMinutes\":-9999}");
    SetTimeCtx ctx;

    WifiTimeApiService::handleApiTimeSync(server, doSetTime, &ctx);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(-840, ctx.lastTzOffsetMinutes);
}

// ---------------------------------------------------------------------------
// Null callback guard
// ---------------------------------------------------------------------------

void test_null_set_time_callback_still_returns_200() {
    WebServer server(80);
    server.setArg("plain", "{\"epochMs\":1751328000000}");

    // No crash expected — callback is optional
    WifiTimeApiService::handleApiTimeSync(server, nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();

    RUN_TEST(test_missing_body_returns_400);
    RUN_TEST(test_empty_body_returns_400);
    RUN_TEST(test_invalid_json_returns_400);
    RUN_TEST(test_missing_epoch_ms_returns_400);
    RUN_TEST(test_epoch_ms_as_string_returns_400);
    RUN_TEST(test_epoch_ms_as_bool_returns_400);
    RUN_TEST(test_epoch_ms_too_small_returns_400);
    RUN_TEST(test_epoch_ms_too_large_returns_400);
    RUN_TEST(test_zero_epoch_ms_returns_400);
    RUN_TEST(test_valid_request_returns_200_and_calls_set_time);
    RUN_TEST(test_valid_request_echoes_epoch_ms_in_response);
    RUN_TEST(test_missing_tz_offset_defaults_to_zero);
    RUN_TEST(test_positive_tz_offset_passed_through);
    RUN_TEST(test_tz_offset_clamped_to_max);
    RUN_TEST(test_tz_offset_clamped_to_min);
    RUN_TEST(test_null_set_time_callback_still_returns_200);

    return UNITY_END();
}

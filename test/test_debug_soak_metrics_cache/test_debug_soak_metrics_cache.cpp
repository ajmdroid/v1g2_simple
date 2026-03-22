#include <unity.h>
#include <cstring>

#include <ArduinoJson.h>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/modules/debug/debug_soak_metrics_cache.h"
#include "../../src/modules/debug/debug_soak_metrics_cache.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeMetricsSource {
    int buildCalls = 0;
    String mode = "initial";
    uint32_t counter = 1;
    String blob;
};

String repeatChar(char ch, size_t count) {
    String value;
    value.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        value += ch;
    }
    return value;
}

size_t measurePayloadBytes(const FakeMetricsSource& source) {
    JsonDocument doc;
    doc["mode"] = source.mode;
    doc["counter"] = source.counter;
    if (source.blob.length() > 0) {
        doc["blob"] = source.blob;
    }
    return measureJson(doc) + 1u;
}

String buildBlobFittingCacheCap() {
    FakeMetricsSource probe;
    probe.mode = "cap";
    probe.counter = 42;

    size_t length = 8192u;
    while (length > 0u) {
        probe.blob = repeatChar('x', length);
        if (measurePayloadBytes(probe) <= 8192u) {
            return probe.blob;
        }
        --length;
    }
    return String();
}

DebugApiService::SoakMetricsBuildFn makeBuildFn(FakeMetricsSource& source) {
    return [&source](JsonDocument& doc) {
        source.buildCalls++;
        doc["mode"] = source.mode;
        doc["counter"] = source.counter;
        if (source.blob.length() > 0) {
            doc["blob"] = source.blob;
        }
    };
}

void releaseCache(DebugApiService::SoakMetricsJsonCache& cache) {
    DebugApiService::releaseSoakMetricsCache(cache);
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
}

void tearDown() {}

void test_first_request_builds_and_caches_soak_metrics() {
    WebServer server(80);
    DebugApiService::SoakMetricsJsonCache cache;
    FakeMetricsSource source;
    uint32_t now = 1000;

    const bool cached = DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    TEST_ASSERT_TRUE(cached);
    TEST_ASSERT_EQUAL_INT(1, source.buildCalls);
    TEST_ASSERT_TRUE(cache.valid);
    TEST_ASSERT_EQUAL_UINT32(1000, cache.lastBuildMs);
    TEST_ASSERT_TRUE(cache.inPsram);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_TRUE(cache.capacity >= cache.length + 1u);
    TEST_ASSERT_TRUE(responseContains(server, "\"mode\":\"initial\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"counter\":1"));
    releaseCache(cache);
}

void test_cache_hit_reuses_cached_payload_within_ttl() {
    WebServer server(80);
    DebugApiService::SoakMetricsJsonCache cache;
    FakeMetricsSource source;
    uint32_t now = 1000;

    DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    const String firstBody = server.lastBody;

    source.mode = "changed";
    source.counter = 2;
    now = 1200;

    DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    TEST_ASSERT_EQUAL_INT(1, source.buildCalls);
    TEST_ASSERT_EQUAL_STRING(firstBody.c_str(), server.lastBody.c_str());
    TEST_ASSERT_TRUE(responseContains(server, "\"mode\":\"initial\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"counter\":1"));
    releaseCache(cache);
}

void test_cache_expiry_rebuilds_payload() {
    WebServer server(80);
    DebugApiService::SoakMetricsJsonCache cache;
    FakeMetricsSource source;
    uint32_t now = 1000;

    DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    source.mode = "expired";
    source.counter = 7;
    now = 1300;

    DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    TEST_ASSERT_EQUAL_INT(2, source.buildCalls);
    TEST_ASSERT_EQUAL_UINT32(1300, cache.lastBuildMs);
    TEST_ASSERT_TRUE(responseContains(server, "\"mode\":\"expired\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"counter\":7"));
    releaseCache(cache);
}

void test_invalidation_forces_rebuild_within_ttl() {
    WebServer server(80);
    DebugApiService::SoakMetricsJsonCache cache;
    FakeMetricsSource source;
    uint32_t now = 1000;

    DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    DebugApiService::invalidateSoakMetricsCache(cache);
    source.mode = "reset";
    source.counter = 9;
    now = 1100;

    DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    TEST_ASSERT_EQUAL_INT(2, source.buildCalls);
    TEST_ASSERT_TRUE(cache.valid);
    TEST_ASSERT_EQUAL_UINT32(1100, cache.lastBuildMs);
    TEST_ASSERT_TRUE(responseContains(server, "\"mode\":\"reset\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"counter\":9"));
    releaseCache(cache);
}

void test_psram_failure_falls_back_to_internal_cache() {
    WebServer server(80);
    DebugApiService::SoakMetricsJsonCache cache;
    FakeMetricsSource source;
    uint32_t now = 1000;
    g_mock_heap_caps_fail_on_call = 1u;

    const bool cached = DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    TEST_ASSERT_TRUE(cached);
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_FALSE(cache.inPsram);
    releaseCache(cache);
}

void test_allocation_failure_falls_back_to_uncached_send() {
    WebServer server(80);
    DebugApiService::SoakMetricsJsonCache cache;
    FakeMetricsSource source;
    uint32_t now = 1000;
    g_mock_heap_caps_fail_call_mask = 0x3u;

    const bool cached = DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    TEST_ASSERT_FALSE(cached);
    TEST_ASSERT_EQUAL_INT(1, source.buildCalls);
    TEST_ASSERT_TRUE(responseContains(server, "\"mode\":\"initial\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"counter\":1"));
    TEST_ASSERT_FALSE(cache.valid);
    TEST_ASSERT_NULL(cache.data);
    TEST_ASSERT_EQUAL_UINT(0, cache.capacity);
    TEST_ASSERT_EQUAL_UINT(0, cache.length);
}


void test_oversized_first_payload_streams_uncached_without_allocating() {
    WebServer server(80);
    DebugApiService::SoakMetricsJsonCache cache;
    FakeMetricsSource source;
    uint32_t now = 1000;
    source.mode = "oversized";
    source.counter = 99;
    source.blob = repeatChar('x', 9000);

    const bool cached = DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    TEST_ASSERT_FALSE(cached);
    TEST_ASSERT_EQUAL_INT(1, source.buildCalls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_NULL(cache.data);
    TEST_ASSERT_EQUAL_UINT(0u, cache.capacity);
    TEST_ASSERT_EQUAL_UINT(0u, cache.length);
    TEST_ASSERT_EQUAL_UINT32(0u, cache.lastBuildMs);
    TEST_ASSERT_FALSE(cache.valid);
    TEST_ASSERT_TRUE(responseContains(server, "\"mode\":\"oversized\""));
}

void test_oversized_payload_invalidates_prior_cache_and_reuses_buffer_later() {
    WebServer server(80);
    DebugApiService::SoakMetricsJsonCache cache;
    FakeMetricsSource source;
    uint32_t now = 1000;

    TEST_ASSERT_TRUE(DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; }));

    char* originalData = cache.data;
    const size_t originalCapacity = cache.capacity;
    const uint32_t originalMallocCalls = g_mock_heap_caps_malloc_calls;

    source.mode = "oversized";
    source.counter = 2;
    source.blob = repeatChar('z', 9000);
    now = 1300;

    TEST_ASSERT_FALSE(DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; }));

    TEST_ASSERT_EQUAL_INT(2, source.buildCalls);
    TEST_ASSERT_FALSE(cache.valid);
    TEST_ASSERT_EQUAL_UINT(0u, cache.length);
    TEST_ASSERT_EQUAL_UINT32(0u, cache.lastBuildMs);
    TEST_ASSERT_EQUAL_PTR(originalData, cache.data);
    TEST_ASSERT_EQUAL_UINT(originalCapacity, cache.capacity);
    TEST_ASSERT_EQUAL_UINT32(originalMallocCalls, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_TRUE(responseContains(server, "\"mode\":\"oversized\""));

    source.mode = "small-again";
    source.counter = 3;
    source.blob = String();
    now = 1600;

    TEST_ASSERT_TRUE(DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; }));

    TEST_ASSERT_EQUAL_INT(3, source.buildCalls);
    TEST_ASSERT_TRUE(cache.valid);
    TEST_ASSERT_EQUAL_UINT32(1600u, cache.lastBuildMs);
    TEST_ASSERT_EQUAL_PTR(originalData, cache.data);
    TEST_ASSERT_EQUAL_UINT(originalCapacity, cache.capacity);
    TEST_ASSERT_EQUAL_UINT32(originalMallocCalls, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_TRUE(responseContains(server, "\"mode\":\"small-again\""));
    releaseCache(cache);
}

void test_cached_payload_capacity_never_exceeds_8k_cap() {
    WebServer server(80);
    DebugApiService::SoakMetricsJsonCache cache;
    FakeMetricsSource source;
    uint32_t now = 1000;
    source.mode = "cap";
    source.counter = 7;
    source.blob = buildBlobFittingCacheCap();

    TEST_ASSERT_TRUE(source.blob.length() > 0);
    TEST_ASSERT_TRUE(measurePayloadBytes(source) <= 8192u);

    const bool cached = DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    TEST_ASSERT_TRUE(cached);
    TEST_ASSERT_TRUE(cache.valid);
    TEST_ASSERT_TRUE(cache.capacity <= 8192u);
    TEST_ASSERT_TRUE(g_mock_heap_caps_last_malloc_size <= 8192u);
    releaseCache(cache);
}

void test_release_cache_frees_buffer_and_resets_state() {
    WebServer server(80);
    DebugApiService::SoakMetricsJsonCache cache;
    FakeMetricsSource source;
    uint32_t now = 1000;

    DebugApiService::sendCachedSoakMetrics(
        server,
        cache,
        250,
        makeBuildFn(source),
        [&now]() { return now; });

    TEST_ASSERT_NOT_NULL(cache.data);
    DebugApiService::releaseSoakMetricsCache(cache);

    TEST_ASSERT_EQUAL_UINT32(1, g_mock_heap_caps_free_calls);
    TEST_ASSERT_NULL(cache.data);
    TEST_ASSERT_EQUAL_UINT(0, cache.capacity);
    TEST_ASSERT_EQUAL_UINT(0, cache.length);
    TEST_ASSERT_FALSE(cache.inPsram);
    TEST_ASSERT_EQUAL_UINT32(0, cache.lastBuildMs);
    TEST_ASSERT_FALSE(cache.valid);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_request_builds_and_caches_soak_metrics);
    RUN_TEST(test_cache_hit_reuses_cached_payload_within_ttl);
    RUN_TEST(test_cache_expiry_rebuilds_payload);
    RUN_TEST(test_invalidation_forces_rebuild_within_ttl);
    RUN_TEST(test_psram_failure_falls_back_to_internal_cache);
    RUN_TEST(test_allocation_failure_falls_back_to_uncached_send);
    RUN_TEST(test_oversized_first_payload_streams_uncached_without_allocating);
    RUN_TEST(test_oversized_payload_invalidates_prior_cache_and_reuses_buffer_later);
    RUN_TEST(test_cached_payload_capacity_never_exceeds_8k_cap);
    RUN_TEST(test_release_cache_frees_buffer_and_resets_state);
    return UNITY_END();
}

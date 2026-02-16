#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_status_api_service.h"
#include "../../src/modules/wifi/wifi_status_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeStatusRuntime {
    bool setupModeActive = false;
    bool staConnected = false;
    String staIp = "";
    String apIp = "192.168.35.5";
    String connectedSsid = "";
    int32_t rssi = 0;
    bool staEnabled = false;
    String staSavedSsid = "";
    String apSsid = "V1-Simple";

    unsigned long uptimeSeconds = 0;
    uint32_t heapFree = 0;
    String hostname = "v1g2";
    String firmwareVersion = "test-fw";

    bool timeValid = false;
    uint8_t timeSource = 0;
    uint8_t timeConfidence = 0;
    int32_t timeTzOffsetMin = 0;
    int64_t timeEpochMs = 0;
    uint32_t timeAgeMs = 0;

    uint16_t batteryVoltageMv = 0;
    uint8_t batteryPercentage = 0;
    bool batteryOnBattery = false;
    bool batteryHasBattery = false;

    bool v1Connected = false;
    String statusJson = "";
    String alertJson = "";

    int setupModeActiveCalls = 0;
};

static WifiStatusApiService::StatusRuntime makeRuntime(FakeStatusRuntime& rt) {
    return WifiStatusApiService::StatusRuntime{
        [&rt]() {
            rt.setupModeActiveCalls++;
            return rt.setupModeActive;
        },
        [&rt]() { return rt.staConnected; },
        [&rt]() { return rt.staIp; },
        [&rt]() { return rt.apIp; },
        [&rt]() { return rt.connectedSsid; },
        [&rt]() { return rt.rssi; },
        [&rt]() { return rt.staEnabled; },
        [&rt]() { return rt.staSavedSsid; },
        [&rt]() { return rt.apSsid; },

        [&rt]() { return rt.uptimeSeconds; },
        [&rt]() { return rt.heapFree; },
        [&rt]() { return rt.hostname; },
        [&rt]() { return rt.firmwareVersion; },

        [&rt]() { return rt.timeValid; },
        [&rt]() { return rt.timeSource; },
        [&rt]() { return rt.timeConfidence; },
        [&rt]() { return rt.timeTzOffsetMin; },
        [&rt]() { return rt.timeEpochMs; },
        [&rt]() { return rt.timeAgeMs; },

        [&rt]() { return rt.batteryVoltageMv; },
        [&rt]() { return rt.batteryPercentage; },
        [&rt]() { return rt.batteryOnBattery; },
        [&rt]() { return rt.batteryHasBattery; },

        [&rt]() { return rt.v1Connected; },
        [&rt]() { return rt.statusJson; },
        [&rt]() { return rt.alertJson; },
    };
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_handle_status_builds_core_payload() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.setupModeActive = true;
    rt.staConnected = true;
    rt.staIp = "10.0.0.24";
    rt.connectedSsid = "HomeWiFi";
    rt.rssi = -55;
    rt.staEnabled = true;
    rt.staSavedSsid = "SavedWiFi";
    rt.uptimeSeconds = 321;
    rt.heapFree = 65432;
    rt.firmwareVersion = "1.2.3-test";
    rt.timeValid = true;
    rt.timeSource = 2;
    rt.timeConfidence = 3;
    rt.timeTzOffsetMin = -300;
    rt.timeEpochMs = 1700000000000LL;
    rt.timeAgeMs = 111;
    rt.batteryVoltageMv = 4042;
    rt.batteryPercentage = 84;
    rt.batteryOnBattery = true;
    rt.batteryHasBattery = true;
    rt.v1Connected = true;

    String cache;
    unsigned long cacheTime = 0;
    unsigned long now = 1000;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [&now]() { return now; },
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"setup_mode\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"sta_connected\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"sta_ip\":\"10.0.0.24\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ap_ip\":\"192.168.35.5\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"HomeWiFi\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"sta_enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"sta_ssid\":\"SavedWiFi\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"uptime\":321"));
    TEST_ASSERT_TRUE(responseContains(server, "\"heap_free\":65432"));
    TEST_ASSERT_TRUE(responseContains(server, "\"firmware_version\":\"1.2.3-test\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"time\":{\"valid\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"tzOffsetMinutes\":-300"));
    TEST_ASSERT_TRUE(responseContains(server, "\"voltage_mv\":4042"));
    TEST_ASSERT_TRUE(responseContains(server, "\"percentage\":84"));
    TEST_ASSERT_TRUE(responseContains(server, "\"v1_connected\":true"));
    TEST_ASSERT_EQUAL_UINT32(1000, cacheTime);
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);
}

void test_handle_status_merges_legacy_status_and_alert_json() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.v1Connected = true;
    rt.statusJson = "{\"foo\":123,\"v1_connected\":false}";
    rt.alertJson = "{\"band\":\"Ka\"}";

    String cache;
    unsigned long cacheTime = 0;
    unsigned long now = 2000;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [&now]() { return now; },
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"foo\":123"));
    TEST_ASSERT_TRUE(responseContains(server, "\"v1_connected\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"alert\":{\"band\":\"Ka\"}"));
}

void test_handle_status_cache_hit_reuses_cached_payload() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.apSsid = "InitialAP";

    String cache;
    unsigned long cacheTime = 0;
    unsigned long now = 1000;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [&now]() { return now; },
        []() { return true; });

    const String firstBody = server.lastBody;
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"InitialAP\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);

    rt.apSsid = "ChangedAP";
    now = 1200;  // within 500ms TTL

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [&now]() { return now; },
        []() { return true; });

    TEST_ASSERT_EQUAL_STRING(firstBody.c_str(), server.lastBody.c_str());
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"InitialAP\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);
}

void test_handle_status_cache_expiry_rebuilds_payload() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.apSsid = "InitialAP";

    String cache;
    unsigned long cacheTime = 0;
    unsigned long now = 1000;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [&now]() { return now; },
        []() { return true; });

    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"InitialAP\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);

    rt.apSsid = "ChangedAP";
    now = 2000;  // past 500ms TTL

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        [&now]() { return now; },
        []() { return true; });

    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"ChangedAP\""));
    TEST_ASSERT_EQUAL_INT(2, rt.setupModeActiveCalls);
}

void test_handle_api_status_rate_limited_short_circuits() {
    WebServer server(80);
    FakeStatusRuntime rt;
    String cache;
    unsigned long cacheTime = 0;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        []() { return 1000UL; },
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.setupModeActiveCalls);
}

void test_handle_api_status_delegates_when_allowed() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.apSsid = "StatusApiAP";
    String cache;
    unsigned long cacheTime = 0;

    WifiStatusApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        []() { return 2000UL; },
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"StatusApiAP\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);
}

void test_handle_legacy_status_delegates_without_rate_limit() {
    WebServer server(80);
    FakeStatusRuntime rt;
    rt.apSsid = "LegacyAP";
    String cache;
    unsigned long cacheTime = 0;

    WifiStatusApiService::handleApiLegacyStatus(
        server,
        makeRuntime(rt),
        cache,
        cacheTime,
        500,
        []() { return 3000UL; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"LegacyAP\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setupModeActiveCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_status_builds_core_payload);
    RUN_TEST(test_handle_status_merges_legacy_status_and_alert_json);
    RUN_TEST(test_handle_status_cache_hit_reuses_cached_payload);
    RUN_TEST(test_handle_status_cache_expiry_rebuilds_payload);
    RUN_TEST(test_handle_api_status_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_status_delegates_when_allowed);
    RUN_TEST(test_handle_legacy_status_delegates_without_rate_limit);
    return UNITY_END();
}

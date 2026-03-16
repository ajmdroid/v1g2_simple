#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_display_colors_api_service.h"
#include "../../src/modules/wifi/wifi_display_colors_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeRuntime {
    V1Settings settings;

    int setDisplayBrightnessCalls = 0;
    uint8_t lastDisplayBrightness = 0;
    int requestColorPreviewHoldCalls = 0;
    uint32_t lastPreviewHoldMs = 0;
    bool isColorPreviewRunning = false;
    int cancelColorPreviewCalls = 0;
    int saveDeferredBackupCalls = 0;
};

static WifiDisplayColorsApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiDisplayColorsApiService::Runtime{
        [&rt]() -> const V1Settings& {
            return rt.settings;
        },
        [&rt]() -> V1Settings& {
            return rt.settings;
        },
        [&rt](uint8_t brightness) {
            rt.setDisplayBrightnessCalls++;
            rt.lastDisplayBrightness = brightness;
        },
        [&rt](uint32_t holdMs) {
            rt.requestColorPreviewHoldCalls++;
            rt.lastPreviewHoldMs = holdMs;
        },
        [&rt]() {
            return rt.isColorPreviewRunning;
        },
        [&rt]() {
            rt.cancelColorPreviewCalls++;
        },
        [&rt]() {
            rt.saveDeferredBackupCalls++;
        },
    };
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_get_returns_500_when_runtime_missing() {
    WebServer server(80);
    WifiDisplayColorsApiService::Runtime runtime{};

    WifiDisplayColorsApiService::handleApiGet(server, runtime);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Settings unavailable\""));
}

void test_get_serializes_display_payload_only() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.colorBogey = 123;
    rt.settings.colorBandKa = 456;
    rt.settings.hideWifiIcon = true;
    rt.settings.brightness = 67;
    rt.settings.voiceVolume = 55;
    rt.settings.enableSignalTraceLogging = false;
    rt.settings.gpsEnabled = true;

    WifiDisplayColorsApiService::handleApiGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"bogey\":123"));
    TEST_ASSERT_TRUE(responseContains(server, "\"bandKa\":456"));
    TEST_ASSERT_TRUE(responseContains(server, "\"hideWifiIcon\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"brightness\":67"));
    TEST_ASSERT_FALSE(responseContains(server, "\"voiceVolume\":"));
    TEST_ASSERT_FALSE(responseContains(server, "\"enableSignalTraceLogging\":"));
    TEST_ASSERT_FALSE(responseContains(server, "\"gpsEnabled\":"));
}

void test_save_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("brightness", "100");

    WifiDisplayColorsApiService::handleApiSave(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.setDisplayBrightnessCalls);
}

void test_save_updates_display_settings_and_calls_side_effects() {
    WebServer server(80);
    FakeRuntime rt;

    server.setArg("bogey", "321");
    server.setArg("wifiConnected", "987");
    server.setArg("hideWifiIcon", "true");
    server.setArg("brightness", "111");

    WifiDisplayColorsApiService::handleApiSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_UINT16(321, rt.settings.colorBogey);
    TEST_ASSERT_EQUAL_UINT16(987, rt.settings.colorWiFiConnected);
    TEST_ASSERT_TRUE(rt.settings.hideWifiIcon);
    TEST_ASSERT_EQUAL_UINT8(111, rt.settings.brightness);
    TEST_ASSERT_EQUAL_INT(1, rt.setDisplayBrightnessCalls);
    TEST_ASSERT_EQUAL_UINT8(111, rt.lastDisplayBrightness);
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.requestColorPreviewHoldCalls);
    TEST_ASSERT_EQUAL_UINT32(5500, rt.lastPreviewHoldMs);
}

void test_save_skip_preview_suppresses_preview_request() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("skipPreview", "true");
    server.setArg("brightness", "50");

    WifiDisplayColorsApiService::handleApiSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.requestColorPreviewHoldCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
}

void test_save_ignores_non_display_args() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.voiceVolume = 67;
    rt.settings.enableSignalTraceLogging = false;
    rt.settings.gpsEnabled = true;

    server.setArg("voiceVolume", "71");
    server.setArg("gpsEnabled", "false");
    server.setArg("enableSignalTraceLogging", "true");

    WifiDisplayColorsApiService::handleApiSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_UINT8(67, rt.settings.voiceVolume);
    TEST_ASSERT_FALSE(rt.settings.enableSignalTraceLogging);
    TEST_ASSERT_TRUE(rt.settings.gpsEnabled);
    TEST_ASSERT_EQUAL_INT(0, rt.setDisplayBrightnessCalls);
}

void test_reset_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.colorBogey = 123;

    WifiDisplayColorsApiService::handleApiReset(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_UINT16(123, rt.settings.colorBogey);
    TEST_ASSERT_EQUAL_INT(0, rt.saveDeferredBackupCalls);
}

void test_reset_restores_defaults_and_triggers_preview() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.colorBogey = 1;
    rt.settings.freqUseBandColor = true;

    WifiDisplayColorsApiService::handleApiReset(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_UINT16(0xF800, rt.settings.colorBogey);
    TEST_ASSERT_EQUAL_UINT16(0x001F, rt.settings.colorBandL);
    TEST_ASSERT_EQUAL_UINT16(0x07E0, rt.settings.colorLockout);
    TEST_ASSERT_EQUAL_UINT16(0x07FF, rt.settings.colorGps);
    TEST_ASSERT_FALSE(rt.settings.freqUseBandColor);
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.requestColorPreviewHoldCalls);
    TEST_ASSERT_EQUAL_UINT32(5500, rt.lastPreviewHoldMs);
}

void test_api_preview_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;

    WifiDisplayColorsApiService::handleApiPreview(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.requestColorPreviewHoldCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.cancelColorPreviewCalls);
}

void test_api_preview_delegates_when_allowed() {
    WebServer server(80);
    FakeRuntime rt;

    WifiDisplayColorsApiService::handleApiPreview(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"active\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.requestColorPreviewHoldCalls);
}

void test_preview_toggles_off_when_running() {
    WebServer server(80);
    FakeRuntime rt;
    rt.isColorPreviewRunning = true;

    WifiDisplayColorsApiService::handleApiPreview(server, makeRuntime(rt), nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"active\":false"));
    TEST_ASSERT_EQUAL_INT(1, rt.cancelColorPreviewCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.requestColorPreviewHoldCalls);
}

void test_preview_starts_when_not_running() {
    WebServer server(80);
    FakeRuntime rt;
    rt.isColorPreviewRunning = false;

    WifiDisplayColorsApiService::handleApiPreview(server, makeRuntime(rt), nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"active\":true"));
    TEST_ASSERT_EQUAL_INT(0, rt.cancelColorPreviewCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.requestColorPreviewHoldCalls);
    TEST_ASSERT_EQUAL_UINT32(5500, rt.lastPreviewHoldMs);
}

void test_api_clear_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;

    WifiDisplayColorsApiService::handleApiClear(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.cancelColorPreviewCalls);
}

void test_api_clear_delegates_when_allowed() {
    WebServer server(80);
    FakeRuntime rt;

    WifiDisplayColorsApiService::handleApiClear(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"active\":false"));
    TEST_ASSERT_EQUAL_INT(1, rt.cancelColorPreviewCalls);
}

void test_clear_cancels_preview_and_returns_inactive() {
    WebServer server(80);
    FakeRuntime rt;

    WifiDisplayColorsApiService::handleApiClear(server, makeRuntime(rt), nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"active\":false"));
    TEST_ASSERT_EQUAL_INT(1, rt.cancelColorPreviewCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_get_returns_500_when_runtime_missing);
    RUN_TEST(test_get_serializes_display_payload_only);
    RUN_TEST(test_save_rate_limited_short_circuits);
    RUN_TEST(test_save_updates_display_settings_and_calls_side_effects);
    RUN_TEST(test_save_skip_preview_suppresses_preview_request);
    RUN_TEST(test_save_ignores_non_display_args);
    RUN_TEST(test_reset_rate_limited_short_circuits);
    RUN_TEST(test_reset_restores_defaults_and_triggers_preview);
    RUN_TEST(test_api_preview_rate_limited_short_circuits);
    RUN_TEST(test_api_preview_delegates_when_allowed);
    RUN_TEST(test_preview_toggles_off_when_running);
    RUN_TEST(test_preview_starts_when_not_running);
    RUN_TEST(test_api_clear_rate_limited_short_circuits);
    RUN_TEST(test_api_clear_delegates_when_allowed);
    RUN_TEST(test_clear_cancels_preview_and_returns_inactive);
    return UNITY_END();
}

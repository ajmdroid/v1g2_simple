#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_display_colors_api_service.h"
#include "../../src/modules/wifi/wifi_display_colors_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

LockoutRuntimeMode gpsLockoutParseRuntimeModeArg(const String& raw,
                                                 LockoutRuntimeMode fallback) {
    if (raw == "0" || raw == "off") return LOCKOUT_RUNTIME_OFF;
    if (raw == "1" || raw == "shadow") return LOCKOUT_RUNTIME_SHADOW;
    if (raw == "2" || raw == "advisory") return LOCKOUT_RUNTIME_ADVISORY;
    if (raw == "3" || raw == "enforce") return LOCKOUT_RUNTIME_ENFORCE;
    return fallback;
}

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeRuntime {
    V1Settings settings;

    int stopObdScanCalls = 0;
    int disconnectObdCalls = 0;
    int setGpsRuntimeEnabledCalls = 0;
    bool lastGpsRuntimeEnabled = false;
    int setSpeedSourceGpsEnabledCalls = 0;
    bool lastSpeedSourceGpsEnabled = false;
    int setCameraRuntimeEnabledCalls = 0;
    bool lastCameraRuntimeEnabled = false;
    int setDisplayBrightnessCalls = 0;
    uint8_t lastDisplayBrightness = 0;
    int setAudioVolumeCalls = 0;
    uint8_t lastAudioVolume = 0;
    int showDisplayDemoCalls = 0;
    int requestColorPreviewHoldCalls = 0;
    uint32_t lastPreviewHoldMs = 0;
    bool isColorPreviewRunning = false;
    int cancelColorPreviewCalls = 0;
    int saveSettingsCalls = 0;
};

static WifiDisplayColorsApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiDisplayColorsApiService::Runtime{
        [&rt]() -> const V1Settings& {
            return rt.settings;
        },
        [&rt]() -> V1Settings& {
            return rt.settings;
        },
        [&rt]() {
            rt.stopObdScanCalls++;
        },
        [&rt]() {
            rt.disconnectObdCalls++;
        },
        [&rt](bool enabled) {
            rt.setGpsRuntimeEnabledCalls++;
            rt.lastGpsRuntimeEnabled = enabled;
        },
        [&rt](bool enabled) {
            rt.setSpeedSourceGpsEnabledCalls++;
            rt.lastSpeedSourceGpsEnabled = enabled;
        },
        [&rt](bool enabled) {
            rt.setCameraRuntimeEnabledCalls++;
            rt.lastCameraRuntimeEnabled = enabled;
        },
        [&rt](uint8_t brightness) {
            rt.setDisplayBrightnessCalls++;
            rt.lastDisplayBrightness = brightness;
        },
        [&rt](uint8_t volume) {
            rt.setAudioVolumeCalls++;
            rt.lastAudioVolume = volume;
        },
        [&rt]() {
            rt.showDisplayDemoCalls++;
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
            rt.saveSettingsCalls++;
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

void test_get_serializes_color_payload() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.colorBogey = 123;
    rt.settings.colorBandKa = 456;
    rt.settings.hideWifiIcon = true;
    rt.settings.voiceVolume = 67;
    rt.settings.gpsEnabled = true;
    rt.settings.cameraEnabled = false;
    rt.settings.gpsLockoutMode = LOCKOUT_RUNTIME_SHADOW;

    WifiDisplayColorsApiService::handleApiGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"bogey\":123"));
    TEST_ASSERT_TRUE(responseContains(server, "\"bandKa\":456"));
    TEST_ASSERT_TRUE(responseContains(server, "\"hideWifiIcon\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"voiceVolume\":67"));
    TEST_ASSERT_TRUE(responseContains(server, "\"gpsEnabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"cameraEnabled\":false"));
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
    TEST_ASSERT_EQUAL_INT(0, rt.saveSettingsCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.setDisplayBrightnessCalls);
}

void test_save_updates_settings_and_calls_side_effects() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.obdEnabled = true;
    rt.settings.gpsEnabled = false;
    rt.settings.cameraEnabled = false;

    server.setArg("bogey", "321");
    server.setArg("wifiConnected", "987");
    server.setArg("hideWifiIcon", "true");
    server.setArg("brightness", "111");
    server.setArg("voiceVolume", "71");
    server.setArg("obdEnabled", "false");
    server.setArg("gpsEnabled", "true");
    server.setArg("cameraEnabled", "true");
    server.setArg("gpsLockoutMode", "enforce");

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
    TEST_ASSERT_EQUAL_UINT8(71, rt.settings.voiceVolume);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LOCKOUT_RUNTIME_ENFORCE),
                          static_cast<int>(rt.settings.gpsLockoutMode));
    TEST_ASSERT_FALSE(rt.settings.obdEnabled);
    TEST_ASSERT_TRUE(rt.settings.gpsEnabled);
    TEST_ASSERT_TRUE(rt.settings.cameraEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.stopObdScanCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.disconnectObdCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.setGpsRuntimeEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastGpsRuntimeEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.setSpeedSourceGpsEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastSpeedSourceGpsEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.setCameraRuntimeEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastCameraRuntimeEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.setDisplayBrightnessCalls);
    TEST_ASSERT_EQUAL_UINT8(111, rt.lastDisplayBrightness);
    TEST_ASSERT_EQUAL_INT(1, rt.setAudioVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(71, rt.lastAudioVolume);
    TEST_ASSERT_EQUAL_INT(1, rt.saveSettingsCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.showDisplayDemoCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.requestColorPreviewHoldCalls);
    TEST_ASSERT_EQUAL_UINT32(5500, rt.lastPreviewHoldMs);
}

void test_save_skip_preview_does_not_trigger_demo() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("skipPreview", "true");
    server.setArg("brightness", "50");

    WifiDisplayColorsApiService::handleApiSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.showDisplayDemoCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.requestColorPreviewHoldCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.saveSettingsCalls);
}

void test_save_clamps_numeric_ranges() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("voiceAlertMode", "99");
    server.setArg("alertVolumeFadeDelaySec", "0");
    server.setArg("alertVolumeFadeVolume", "99");
    server.setArg("speedVolumeThresholdMph", "300");
    server.setArg("speedVolumeBoost", "0");
    server.setArg("lowSpeedMuteThresholdMph", "0");
    server.setArg("gpsLockoutMaxQueueDrops", "70000");

    WifiDisplayColorsApiService::handleApiSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(rt.settings.voiceAlertMode));
    TEST_ASSERT_EQUAL_UINT8(1, rt.settings.alertVolumeFadeDelaySec);
    TEST_ASSERT_EQUAL_UINT8(9, rt.settings.alertVolumeFadeVolume);
    TEST_ASSERT_EQUAL_UINT8(100, rt.settings.speedVolumeThresholdMph);
    TEST_ASSERT_EQUAL_UINT8(1, rt.settings.speedVolumeBoost);
    TEST_ASSERT_EQUAL_UINT8(1, rt.settings.lowSpeedMuteThresholdMph);
    TEST_ASSERT_EQUAL_UINT16(65535, rt.settings.gpsLockoutMaxQueueDrops);
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
    TEST_ASSERT_EQUAL_INT(0, rt.saveSettingsCalls);
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
    TEST_ASSERT_FALSE(rt.settings.freqUseBandColor);
    TEST_ASSERT_EQUAL_INT(1, rt.saveSettingsCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.showDisplayDemoCalls);
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
    TEST_ASSERT_EQUAL_INT(0, rt.showDisplayDemoCalls);
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
    TEST_ASSERT_EQUAL_INT(1, rt.showDisplayDemoCalls);
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
    TEST_ASSERT_EQUAL_INT(0, rt.showDisplayDemoCalls);
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
    TEST_ASSERT_EQUAL_INT(1, rt.showDisplayDemoCalls);
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
    RUN_TEST(test_get_serializes_color_payload);
    RUN_TEST(test_save_rate_limited_short_circuits);
    RUN_TEST(test_save_updates_settings_and_calls_side_effects);
    RUN_TEST(test_save_skip_preview_does_not_trigger_demo);
    RUN_TEST(test_save_clamps_numeric_ranges);
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

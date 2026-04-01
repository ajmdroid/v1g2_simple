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
    int forceDisplayRedrawCalls = 0;
    int requestColorPreviewHoldCalls = 0;
    uint32_t lastPreviewHoldMs = 0;
    bool isColorPreviewRunning = false;
    int cancelColorPreviewCalls = 0;
    int saveDeferredBackupCalls = 0;
};

static void applyDisplaySettingsUpdateForTest(FakeRuntime& rt, const DisplaySettingsUpdate& update) {
    rt.saveDeferredBackupCalls++;
    if (update.hasColorBogey) rt.settings.colorBogey = update.colorBogey;
    if (update.hasColorFrequency) rt.settings.colorFrequency = update.colorFrequency;
    if (update.hasColorArrowFront) rt.settings.colorArrowFront = update.colorArrowFront;
    if (update.hasColorArrowSide) rt.settings.colorArrowSide = update.colorArrowSide;
    if (update.hasColorArrowRear) rt.settings.colorArrowRear = update.colorArrowRear;
    if (update.hasColorBandL) rt.settings.colorBandL = update.colorBandL;
    if (update.hasColorBandKa) rt.settings.colorBandKa = update.colorBandKa;
    if (update.hasColorBandK) rt.settings.colorBandK = update.colorBandK;
    if (update.hasColorBandX) rt.settings.colorBandX = update.colorBandX;
    if (update.hasColorBandPhoto) rt.settings.colorBandPhoto = update.colorBandPhoto;
    if (update.hasColorWiFiIcon) rt.settings.colorWiFiIcon = update.colorWiFiIcon;
    if (update.hasColorWiFiConnected) rt.settings.colorWiFiConnected = update.colorWiFiConnected;
    if (update.hasColorBleConnected) rt.settings.colorBleConnected = update.colorBleConnected;
    if (update.hasColorBleDisconnected) rt.settings.colorBleDisconnected = update.colorBleDisconnected;
    if (update.hasColorBar1) rt.settings.colorBar1 = update.colorBar1;
    if (update.hasColorBar2) rt.settings.colorBar2 = update.colorBar2;
    if (update.hasColorBar3) rt.settings.colorBar3 = update.colorBar3;
    if (update.hasColorBar4) rt.settings.colorBar4 = update.colorBar4;
    if (update.hasColorBar5) rt.settings.colorBar5 = update.colorBar5;
    if (update.hasColorBar6) rt.settings.colorBar6 = update.colorBar6;
    if (update.hasColorMuted) rt.settings.colorMuted = update.colorMuted;
    if (update.hasColorPersisted) rt.settings.colorPersisted = update.colorPersisted;
    if (update.hasColorVolumeMain) rt.settings.colorVolumeMain = update.colorVolumeMain;
    if (update.hasColorVolumeMute) rt.settings.colorVolumeMute = update.colorVolumeMute;
    if (update.hasColorRssiV1) rt.settings.colorRssiV1 = update.colorRssiV1;
    if (update.hasColorRssiProxy) rt.settings.colorRssiProxy = update.colorRssiProxy;
    if (update.hasColorObd) rt.settings.colorObd = update.colorObd;
    if (update.hasFreqUseBandColor) rt.settings.freqUseBandColor = update.freqUseBandColor;
    if (update.hasHideWifiIcon) rt.settings.hideWifiIcon = update.hideWifiIcon;
    if (update.hasHideProfileIndicator) rt.settings.hideProfileIndicator = update.hideProfileIndicator;
    if (update.hasHideBatteryIcon) rt.settings.hideBatteryIcon = update.hideBatteryIcon;
    if (update.hasShowBatteryPercent) rt.settings.showBatteryPercent = update.showBatteryPercent;
    if (update.hasHideBleIcon) rt.settings.hideBleIcon = update.hideBleIcon;
    if (update.hasHideVolumeIndicator) rt.settings.hideVolumeIndicator = update.hideVolumeIndicator;
    if (update.hasHideRssiIndicator) rt.settings.hideRssiIndicator = update.hideRssiIndicator;
    if (update.hasBrightness) rt.settings.brightness = update.brightness;
    if (update.hasDisplayStyle) rt.settings.displayStyle = update.displayStyle;
}

static void resetDisplaySettingsForTest(FakeRuntime& rt) {
    rt.saveDeferredBackupCalls++;
    rt.settings.colorBogey = 0xF800;
    rt.settings.colorFrequency = 0xF800;
    rt.settings.colorArrowFront = 0xF800;
    rt.settings.colorArrowSide = 0xF800;
    rt.settings.colorArrowRear = 0xF800;
    rt.settings.colorBandL = 0x001F;
    rt.settings.colorBandKa = 0xF800;
    rt.settings.colorBandK = 0x001F;
    rt.settings.colorBandX = 0x07E0;
    rt.settings.colorBandPhoto = 0x780F;
    rt.settings.colorWiFiIcon = 0x07FF;
    rt.settings.colorWiFiConnected = 0x07E0;
    rt.settings.colorBleConnected = 0x07E0;
    rt.settings.colorBleDisconnected = 0x001F;
    rt.settings.colorBar1 = 0x07E0;
    rt.settings.colorBar2 = 0x07E0;
    rt.settings.colorBar3 = 0xFFE0;
    rt.settings.colorBar4 = 0xFFE0;
    rt.settings.colorBar5 = 0xF800;
    rt.settings.colorBar6 = 0xF800;
    rt.settings.colorMuted = 0x3186;
    rt.settings.colorPersisted = 0x18C3;
    rt.settings.colorVolumeMain = 0x001F;
    rt.settings.colorVolumeMute = 0xFFE0;
    rt.settings.colorRssiV1 = 0x07E0;
    rt.settings.colorRssiProxy = 0x001F;
    rt.settings.colorObd = 0x001F;
    rt.settings.freqUseBandColor = false;
}

static WifiDisplayColorsApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiDisplayColorsApiService::Runtime{
        [](void* ctx) -> const V1Settings& {
            return static_cast<FakeRuntime*>(ctx)->settings;
        }, &rt,
        [](const DisplaySettingsUpdate& update, void* ctx) {
            applyDisplaySettingsUpdateForTest(*static_cast<FakeRuntime*>(ctx), update);
        }, &rt,
        [](void* ctx) {
            resetDisplaySettingsForTest(*static_cast<FakeRuntime*>(ctx));
        }, &rt,
        [](uint8_t brightness, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setDisplayBrightnessCalls++;
            rtp->lastDisplayBrightness = brightness;
        }, &rt,
        [](void* ctx) {
            static_cast<FakeRuntime*>(ctx)->forceDisplayRedrawCalls++;
        }, &rt,
        [](uint32_t holdMs, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->requestColorPreviewHoldCalls++;
            rtp->lastPreviewHoldMs = holdMs;
        }, &rt,
        [](void* ctx) {
            return static_cast<FakeRuntime*>(ctx)->isColorPreviewRunning;
        }, &rt,
        [](void* ctx) {
            static_cast<FakeRuntime*>(ctx)->cancelColorPreviewCalls++;
        }, &rt,
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
    rt.settings.colorObd = 789;
    rt.settings.hideWifiIcon = true;
    rt.settings.brightness = 67;
    rt.settings.displayStyle = DISPLAY_STYLE_SERPENTINE;
    rt.settings.voiceVolume = 55;

    WifiDisplayColorsApiService::handleApiGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"bogey\":123"));
    TEST_ASSERT_TRUE(responseContains(server, "\"bandKa\":456"));
    TEST_ASSERT_TRUE(responseContains(server, "\"obd\":789"));
    TEST_ASSERT_TRUE(responseContains(server, "\"hideWifiIcon\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"brightness\":67"));
    TEST_ASSERT_TRUE(responseContains(server, "\"displayStyle\":3"));
    TEST_ASSERT_FALSE(responseContains(server, "\"voiceVolume\":"));
}

void test_save_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("brightness", "100");

    WifiDisplayColorsApiService::handleApiSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.setDisplayBrightnessCalls);
}

void test_save_updates_display_settings_and_calls_side_effects() {
    WebServer server(80);
    FakeRuntime rt;

    server.setArg("bogey", "321");
    server.setArg("wifiConnected", "987");
    server.setArg("obd", "654");
    server.setArg("hideWifiIcon", "true");
    server.setArg("brightness", "111");
    server.setArg("displayStyle", "3");

    WifiDisplayColorsApiService::handleApiSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_UINT16(321, rt.settings.colorBogey);
    TEST_ASSERT_EQUAL_UINT16(987, rt.settings.colorWiFiConnected);
    TEST_ASSERT_EQUAL_UINT16(654, rt.settings.colorObd);
    TEST_ASSERT_TRUE(rt.settings.hideWifiIcon);
    TEST_ASSERT_EQUAL_UINT8(111, rt.settings.brightness);
    TEST_ASSERT_EQUAL_INT(1, rt.setDisplayBrightnessCalls);
    TEST_ASSERT_EQUAL_UINT8(111, rt.lastDisplayBrightness);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DISPLAY_STYLE_SERPENTINE),
                          static_cast<int>(rt.settings.displayStyle));
    TEST_ASSERT_EQUAL_INT(1, rt.forceDisplayRedrawCalls);
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
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.requestColorPreviewHoldCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
}

void test_save_ignores_non_display_args() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.voiceVolume = 67;

    server.setArg("voiceVolume", "71");

    WifiDisplayColorsApiService::handleApiSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_UINT8(67, rt.settings.voiceVolume);
    TEST_ASSERT_EQUAL_INT(0, rt.setDisplayBrightnessCalls);
}

void test_reset_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.colorBogey = 123;

    WifiDisplayColorsApiService::handleApiReset(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return false; }, nullptr);

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
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_UINT16(0xF800, rt.settings.colorBogey);
    TEST_ASSERT_EQUAL_UINT16(0x001F, rt.settings.colorBandL);
    TEST_ASSERT_EQUAL_UINT16(0x001F, rt.settings.colorObd);
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
        [](void* /*ctx*/) { return false; }, nullptr);

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
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"active\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.requestColorPreviewHoldCalls);
}

void test_preview_toggles_off_when_running() {
    WebServer server(80);
    FakeRuntime rt;
    rt.isColorPreviewRunning = true;

    WifiDisplayColorsApiService::handleApiPreview(server, makeRuntime(rt), nullptr, nullptr);

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

    WifiDisplayColorsApiService::handleApiPreview(server, makeRuntime(rt), nullptr, nullptr);

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
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.cancelColorPreviewCalls);
}

void test_api_clear_delegates_when_allowed() {
    WebServer server(80);
    FakeRuntime rt;

    WifiDisplayColorsApiService::handleApiClear(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"active\":false"));
    TEST_ASSERT_EQUAL_INT(1, rt.cancelColorPreviewCalls);
}

void test_clear_cancels_preview_and_returns_inactive() {
    WebServer server(80);
    FakeRuntime rt;

    WifiDisplayColorsApiService::handleApiClear(server, makeRuntime(rt), nullptr, nullptr);

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

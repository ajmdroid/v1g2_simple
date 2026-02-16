#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_settings_api_service.h"
#include "../../src/modules/wifi/wifi_settings_api_service.cpp"  // Pull implementation for UNIT_TEST.

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

    int updateApCredentialsCalls = 0;
    String lastApSsid;
    String lastApPassword;

    int updateBrightnessCalls = 0;
    uint8_t lastBrightness = 0;

    int updateDisplayStyleCalls = 0;
    DisplayStyle lastDisplayStyle = DISPLAY_STYLE_CLASSIC;
    int forceDisplayRedrawCalls = 0;

    int setObdVwDataEnabledCalls = 0;
    bool lastObdVwDataEnabled = false;

    int stopObdScanCalls = 0;
    int disconnectObdCalls = 0;

    int setGpsRuntimeEnabledCalls = 0;
    bool lastGpsRuntimeEnabled = false;

    int setSpeedSourceGpsEnabledCalls = 0;
    bool lastSpeedSourceGpsEnabled = false;

    int setCameraRuntimeEnabledCalls = 0;
    bool lastCameraRuntimeEnabled = false;

    int setLockoutKaLearningEnabledCalls = 0;
    bool lastLockoutKaLearningEnabled = false;

    int saveCalls = 0;
};

static WifiSettingsApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiSettingsApiService::Runtime{
        [&rt]() -> const V1Settings& {
            return rt.settings;
        },
        [&rt]() -> V1Settings& {
            return rt.settings;
        },
        [&rt](const String& ssid, const String& password) {
            rt.updateApCredentialsCalls++;
            rt.lastApSsid = ssid;
            rt.lastApPassword = password;
            rt.settings.apSSID = ssid;
            rt.settings.apPassword = password;
        },
        [&rt](uint8_t brightness) {
            rt.updateBrightnessCalls++;
            rt.lastBrightness = brightness;
            rt.settings.brightness = brightness;
        },
        [&rt](DisplayStyle style) {
            rt.updateDisplayStyleCalls++;
            rt.lastDisplayStyle = style;
            rt.settings.displayStyle = style;
        },
        [&rt]() {
            rt.forceDisplayRedrawCalls++;
        },
        [&rt](bool enabled) {
            rt.setObdVwDataEnabledCalls++;
            rt.lastObdVwDataEnabled = enabled;
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
        [&rt](bool enabled) {
            rt.setLockoutKaLearningEnabledCalls++;
            rt.lastLockoutKaLearningEnabled = enabled;
        },
        [&rt]() {
            rt.saveCalls++;
        },
    };
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_settings_get_serializes_expected_payload() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.apSSID = "V1-Test";
    rt.settings.apPassword = "custom-pass";
    rt.settings.proxyBLE = false;
    rt.settings.proxyName = "Proxy-Test";
    rt.settings.obdEnabled = false;
    rt.settings.gpsEnabled = true;
    rt.settings.cameraEnabled = true;
    rt.settings.gpsLockoutMode = LOCKOUT_RUNTIME_ADVISORY;
    rt.settings.autoPowerOffMinutes = 12;
    rt.settings.apTimeoutMinutes = 25;
    rt.settings.enableWifiAtBoot = true;

    WifiSettingsApiService::handleSettingsGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ap_ssid\":\"V1-Test\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ap_password\":\"********\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"isDefaultPassword\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"proxy_ble\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"proxy_name\":\"Proxy-Test\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"gpsLockoutModeName\":\"advisory\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"enableWifiAtBoot\":true"));
}

void test_settings_get_returns_500_without_runtime() {
    WebServer server(80);
    WifiSettingsApiService::Runtime runtime{};

    WifiSettingsApiService::handleSettingsGet(server, runtime);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Settings unavailable\""));
}

void test_settings_save_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("brightness", "20");

    WifiSettingsApiService::handleSettingsSave(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.updateBrightnessCalls);
}

void test_legacy_settings_save_rate_limited_short_circuits_on_route_guard() {
    WebServer server(80);
    FakeRuntime rt;
    int rateLimitCalls = 0;
    int deprecatedHeaderCalls = 0;
    int legacyWarnCalls = 0;

    WifiSettingsApiService::handleLegacySettingsSave(
        server,
        makeRuntime(rt),
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        },
        [&deprecatedHeaderCalls]() { deprecatedHeaderCalls++; },
        [&legacyWarnCalls]() { legacyWarnCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, deprecatedHeaderCalls);
    TEST_ASSERT_EQUAL_INT(0, legacyWarnCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_legacy_settings_save_preserves_double_rate_limit_behavior() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("brightness", "11");
    int rateLimitCalls = 0;
    int deprecatedHeaderCalls = 0;
    int legacyWarnCalls = 0;

    WifiSettingsApiService::handleLegacySettingsSave(
        server,
        makeRuntime(rt),
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&deprecatedHeaderCalls]() { deprecatedHeaderCalls++; },
        [&legacyWarnCalls]() { legacyWarnCalls++; });

    TEST_ASSERT_EQUAL_INT(2, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, deprecatedHeaderCalls);
    TEST_ASSERT_EQUAL_INT(1, legacyWarnCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.updateBrightnessCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
}

void test_settings_save_rejects_invalid_ap_credentials() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("ap_ssid", "MyAP");
    server.setArg("ap_password", "short");

    WifiSettingsApiService::handleSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "AP SSID required and password must be at least 8 characters"));
    TEST_ASSERT_EQUAL_INT(0, rt.updateApCredentialsCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_settings_save_uses_existing_password_placeholder() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.apPassword = "existing123";
    server.setArg("ap_ssid", "RenamedAP");
    server.setArg("ap_password", "********");

    WifiSettingsApiService::handleSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.updateApCredentialsCalls);
    TEST_ASSERT_EQUAL_STRING("RenamedAP", rt.lastApSsid.c_str());
    TEST_ASSERT_EQUAL_STRING("existing123", rt.lastApPassword.c_str());
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
}

void test_settings_save_updates_runtime_dependencies() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("obdVwDataEnabled", "1");
    server.setArg("obdEnabled", "false");
    server.setArg("gpsEnabled", "true");
    server.setArg("cameraEnabled", "true");
    server.setArg("gpsLockoutMode", "enforce");
    server.setArg("gpsLockoutMaxQueueDrops", "70000");
    server.setArg("gpsLockoutKaLearningEnabled", "true");

    WifiSettingsApiService::handleSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.setObdVwDataEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastObdVwDataEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.stopObdScanCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.disconnectObdCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.setGpsRuntimeEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastGpsRuntimeEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.setSpeedSourceGpsEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastSpeedSourceGpsEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.setCameraRuntimeEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastCameraRuntimeEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.setLockoutKaLearningEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastLockoutKaLearningEnabled);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LOCKOUT_RUNTIME_ENFORCE),
                          static_cast<int>(rt.settings.gpsLockoutMode));
    TEST_ASSERT_EQUAL_UINT16(65535, rt.settings.gpsLockoutMaxQueueDrops);
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
}

void test_settings_save_updates_brightness_and_display_style() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("brightness", "42");
    server.setArg("displayStyle", "3");

    WifiSettingsApiService::handleSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.updateBrightnessCalls);
    TEST_ASSERT_EQUAL_UINT8(42, rt.lastBrightness);
    TEST_ASSERT_EQUAL_INT(1, rt.updateDisplayStyleCalls);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DISPLAY_STYLE_SERPENTINE),
                          static_cast<int>(rt.lastDisplayStyle));
    TEST_ASSERT_EQUAL_INT(1, rt.forceDisplayRedrawCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_settings_get_serializes_expected_payload);
    RUN_TEST(test_settings_get_returns_500_without_runtime);
    RUN_TEST(test_settings_save_rate_limited_short_circuits);
    RUN_TEST(test_legacy_settings_save_rate_limited_short_circuits_on_route_guard);
    RUN_TEST(test_legacy_settings_save_preserves_double_rate_limit_behavior);
    RUN_TEST(test_settings_save_rejects_invalid_ap_credentials);
    RUN_TEST(test_settings_save_uses_existing_password_placeholder);
    RUN_TEST(test_settings_save_updates_runtime_dependencies);
    RUN_TEST(test_settings_save_updates_brightness_and_display_style);
    return UNITY_END();
}

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

    int setGpsRuntimeEnabledCalls = 0;
    bool lastGpsRuntimeEnabled = false;

    int setSpeedSourceGpsEnabledCalls = 0;
    bool lastSpeedSourceGpsEnabled = false;

    int setLockoutKaLearningEnabledCalls = 0;
    bool lastLockoutKaLearningEnabled = false;

    int setObdRuntimeEnabledCalls = 0;
    bool lastObdRuntimeEnabled = false;

    int setObdRuntimeMinRssiCalls = 0;
    int8_t lastObdRuntimeMinRssi = 0;

    int setSpeedSourceObdEnabledCalls = 0;
    bool lastSpeedSourceObdEnabled = false;

    int saveDeferredBackupCalls = 0;
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
        [&rt](bool enabled) {
            rt.setGpsRuntimeEnabledCalls++;
            rt.lastGpsRuntimeEnabled = enabled;
        },
        [&rt](bool enabled) {
            rt.setSpeedSourceGpsEnabledCalls++;
            rt.lastSpeedSourceGpsEnabled = enabled;
        },
        [&rt](bool enabled) {
            rt.setLockoutKaLearningEnabledCalls++;
            rt.lastLockoutKaLearningEnabled = enabled;
        },
        [](bool) {},  // setLockoutKLearningEnabled
        [](bool) {},  // setLockoutXLearningEnabled
        [&rt](bool enabled) {
            rt.setObdRuntimeEnabledCalls++;
            rt.lastObdRuntimeEnabled = enabled;
        },
        [&rt](int8_t minRssi) {
            rt.setObdRuntimeMinRssiCalls++;
            rt.lastObdRuntimeMinRssi = minRssi;
        },
        [&rt](bool enabled) {
            rt.setSpeedSourceObdEnabledCalls++;
            rt.lastSpeedSourceObdEnabled = enabled;
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

void test_device_settings_get_serializes_expected_payload() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.apSSID = "V1-Test";
    rt.settings.apPassword = "custom-pass";
    rt.settings.proxyBLE = false;
    rt.settings.proxyName = "Proxy-Test";
    rt.settings.autoPowerOffMinutes = 12;
    rt.settings.apTimeoutMinutes = 25;
    rt.settings.enableWifiAtBoot = true;
    rt.settings.enableSignalTraceLogging = false;

    WifiSettingsApiService::handleApiDeviceSettingsGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ap_ssid\":\"V1-Test\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ap_password\":\"********\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"isDefaultPassword\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"proxy_ble\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"proxy_name\":\"Proxy-Test\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"autoPowerOffMinutes\":12"));
    TEST_ASSERT_TRUE(responseContains(server, "\"apTimeoutMinutes\":25"));
    TEST_ASSERT_TRUE(responseContains(server, "\"enableWifiAtBoot\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"enableSignalTraceLogging\":false"));
}

void test_settings_save_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("gpsEnabled", "true");

    WifiSettingsApiService::handleApiSettingsSave(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.setGpsRuntimeEnabledCalls);
}

void test_device_settings_save_rejects_invalid_ap_credentials() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("ap_ssid", "MyAP");
    server.setArg("ap_password", "short");

    WifiSettingsApiService::handleApiDeviceSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "AP SSID required and password must be at least 8 characters"));
    TEST_ASSERT_EQUAL_INT(0, rt.updateApCredentialsCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.saveDeferredBackupCalls);
}

void test_device_settings_save_uses_existing_password_placeholder() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.apPassword = "existing123";
    server.setArg("ap_ssid", "RenamedAP");
    server.setArg("ap_password", "********");

    WifiSettingsApiService::handleApiDeviceSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.updateApCredentialsCalls);
    TEST_ASSERT_EQUAL_STRING("RenamedAP", rt.lastApSsid.c_str());
    TEST_ASSERT_EQUAL_STRING("existing123", rt.lastApPassword.c_str());
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
}

void test_device_settings_save_updates_device_toggles() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("proxy_ble", "0");
    server.setArg("proxy_name", "Garage Unit");
    server.setArg("autoPowerOffMinutes", "19");
    server.setArg("apTimeoutMinutes", "14");
    server.setArg("enableWifiAtBoot", "true");
    server.setArg("enableSignalTraceLogging", "false");

    WifiSettingsApiService::handleApiDeviceSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_FALSE(rt.settings.proxyBLE);
    TEST_ASSERT_EQUAL_STRING("Garage Unit", rt.settings.proxyName.c_str());
    TEST_ASSERT_EQUAL_UINT8(19, rt.settings.autoPowerOffMinutes);
    TEST_ASSERT_EQUAL_UINT8(14, rt.settings.apTimeoutMinutes);
    TEST_ASSERT_TRUE(rt.settings.enableWifiAtBoot);
    TEST_ASSERT_FALSE(rt.settings.enableSignalTraceLogging);
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
}

void test_settings_save_updates_runtime_dependencies() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("gpsEnabled", "true");
    server.setArg("gpsLockoutMode", "enforce");
    server.setArg("gpsLockoutMaxQueueDrops", "70000");
    server.setArg("gpsLockoutKaLearningEnabled", "true");

    WifiSettingsApiService::handleApiSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.setGpsRuntimeEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastGpsRuntimeEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.setSpeedSourceGpsEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastSpeedSourceGpsEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.setLockoutKaLearningEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastLockoutKaLearningEnabled);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LOCKOUT_RUNTIME_ENFORCE),
                          static_cast<int>(rt.settings.gpsLockoutMode));
    TEST_ASSERT_EQUAL_UINT16(65535, rt.settings.gpsLockoutMaxQueueDrops);
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
}

void test_settings_save_ignores_display_args() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.brightness = 77;
    rt.settings.displayStyle = DISPLAY_STYLE_SERPENTINE;
    server.setArg("brightness", "42");
    server.setArg("displayStyle", "3");

    WifiSettingsApiService::handleApiSettingsSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_UINT8(77, rt.settings.brightness);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DISPLAY_STYLE_SERPENTINE),
                          static_cast<int>(rt.settings.displayStyle));
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_device_settings_get_serializes_expected_payload);
    RUN_TEST(test_settings_save_rate_limited_short_circuits);
    RUN_TEST(test_device_settings_save_rejects_invalid_ap_credentials);
    RUN_TEST(test_device_settings_save_uses_existing_password_placeholder);
    RUN_TEST(test_device_settings_save_updates_device_toggles);
    RUN_TEST(test_settings_save_updates_runtime_dependencies);
    RUN_TEST(test_settings_save_ignores_display_args);
    return UNITY_END();
}

#include <unity.h>
#include <cstring>

#include "../mocks/settings.h"
#include "../../src/modules/obd/obd_runtime_module.h"
#include "../../src/modules/obd/obd_elm327_parser.cpp"
#include "../../src/modules/obd/obd_runtime_module.cpp"
#include "../../src/modules/obd/obd_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 1000;
unsigned long mockMicros = 1000000;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

static void resetRuntime() {
    obdRuntimeModule = ObdRuntimeModule();
}

void setUp() {
    resetRuntime();
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_config_get_returns_persisted_settings() {
    WebServer server(80);
    SettingsManager settingsManager;
    settingsManager.settings.obdEnabled = true;
    settingsManager.settings.obdMinRssi = -62;

    ObdApiService::handleApiConfigGet(server, settingsManager, []() {});

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"minRssi\":-62"));
}

void test_config_updates_runtime_settings_and_speed_source_callback() {
    WebServer server(80);
    SettingsManager settingsManager;
    bool speedSourceEnabled = false;
    int speedSourceCallbackCalls = 0;

    obdRuntimeModule.begin(false, "", 0, -80);
    server.setArg("plain", "{\"enabled\":true,\"minRssi\":-55}");

    ObdApiService::handleApiConfig(server,
                                   obdRuntimeModule,
                                   settingsManager,
                                   [&](bool enabled) {
                                       speedSourceEnabled = enabled;
                                       speedSourceCallbackCalls++;
                                   },
                                   []() { return true; },
                                   []() {});

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(settingsManager.settings.obdEnabled);
    TEST_ASSERT_EQUAL_INT8(-55, settingsManager.settings.obdMinRssi);
    TEST_ASSERT_TRUE(obdRuntimeModule.isEnabled());
    TEST_ASSERT_TRUE(speedSourceEnabled);
    TEST_ASSERT_EQUAL_INT(1, speedSourceCallbackCalls);
    TEST_ASSERT_EQUAL_INT(1, settingsManager.saveCalls);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(2000, true, true, true);
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -60);
    obdRuntimeModule.update(3000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_forget_clears_saved_address_and_persists_setting() {
    WebServer server(80);
    SettingsManager settingsManager;
    settingsManager.settings.obdSavedAddress = "A4:C1:38:00:11:22";
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);

    ObdApiService::handleApiForget(server,
                                   obdRuntimeModule,
                                   settingsManager,
                                   []() { return true; },
                                   []() {});

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_EQUAL_STRING("", settingsManager.settings.obdSavedAddress.c_str());
    TEST_ASSERT_EQUAL_INT(1, settingsManager.saveCalls);

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(mockMillis);
    TEST_ASSERT_FALSE(status.savedAddressValid);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_config_rejects_missing_json_body() {
    WebServer server(80);
    SettingsManager settingsManager;
    obdRuntimeModule.begin(false, "", 0, -80);

    ObdApiService::handleApiConfig(server,
                                   obdRuntimeModule,
                                   settingsManager,
                                   [](bool) {},
                                   []() { return true; },
                                   []() {});

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Missing JSON body"));
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
}

void test_scan_rejects_when_obd_is_disabled() {
    WebServer server(80);
    obdRuntimeModule.begin(false, "", 0, -80);

    ObdApiService::handleApiScan(server,
                                 obdRuntimeModule,
                                 []() { return true; },
                                 []() {});

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"OBD is disabled\""));
    TEST_ASSERT_FALSE(obdRuntimeModule.snapshot(mockMillis).scanInProgress);
}

void test_scan_reports_requested_when_obd_is_enabled() {
    WebServer server(80);
    obdRuntimeModule.begin(true, "", 0, -80);

    ObdApiService::handleApiScan(server,
                                 obdRuntimeModule,
                                 []() { return true; },
                                 []() {});

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"requested\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"scanInProgress\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"OBD scan requested\""));
}

void test_scan_rejects_when_request_already_pending() {
    WebServer server(80);
    obdRuntimeModule.begin(true, "", 0, -80);
    TEST_ASSERT_TRUE(obdRuntimeModule.startScan());

    ObdApiService::handleApiScan(server,
                                 obdRuntimeModule,
                                 []() { return true; },
                                 []() {});

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"OBD scan already requested or in progress\""));
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_config_get_returns_persisted_settings);
    RUN_TEST(test_config_updates_runtime_settings_and_speed_source_callback);
    RUN_TEST(test_forget_clears_saved_address_and_persists_setting);
    RUN_TEST(test_config_rejects_missing_json_body);
    RUN_TEST(test_scan_rejects_when_obd_is_disabled);
    RUN_TEST(test_scan_reports_requested_when_obd_is_enabled);
    RUN_TEST(test_scan_rejects_when_request_already_pending);

    return UNITY_END();
}

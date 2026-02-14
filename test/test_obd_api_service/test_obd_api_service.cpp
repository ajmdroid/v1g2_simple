#include <unity.h>
#include <cstring>

#include "../../src/modules/obd/obd_api_service.h"
#include "../../src/modules/obd/obd_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_parse_connect_request_from_query_args() {
    WebServer server(80);
    server.setArg("address", "AA:BB:CC");
    server.setArg("name", "TestAdapter");
    server.setArg("pin", "1234");
    server.setArg("remember", "1");
    server.setArg("autoConnect", "true");

    ObdApiService::ConnectRequest request;
    String errorMessage;
    TEST_ASSERT_TRUE(ObdApiService::parseConnectRequest(server, request, errorMessage));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC", request.address.c_str());
    TEST_ASSERT_EQUAL_STRING("TestAdapter", request.name.c_str());
    TEST_ASSERT_EQUAL_STRING("1234", request.pin.c_str());
    TEST_ASSERT_TRUE(request.remember);
    TEST_ASSERT_TRUE(request.autoConnect);
}

void test_parse_connect_request_missing_address() {
    WebServer server(80);
    ObdApiService::ConnectRequest request;
    String errorMessage;
    TEST_ASSERT_FALSE(ObdApiService::parseConnectRequest(server, request, errorMessage));
    TEST_ASSERT_EQUAL_STRING("Missing address", errorMessage.c_str());
}

void test_handle_scan_requires_v1_connection() {
    WebServer server(80);
    OBDHandler obdHandler;
    V1BLEClient bleClient;
    bleClient.setConnected(false);

    ObdApiService::handleScan(server, obdHandler, bleClient);
    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Connect V1 before OBD scan"));
    TEST_ASSERT_EQUAL_INT(0, obdHandler.startScanCalls);
}

void test_handle_scan_starts_scan_when_connected() {
    WebServer server(80);
    OBDHandler obdHandler;
    V1BLEClient bleClient;
    bleClient.setConnected(true);

    ObdApiService::handleScan(server, obdHandler, bleClient);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, obdHandler.startScanCalls);
}

void test_handle_connect_queues_request() {
    WebServer server(80);
    OBDHandler obdHandler;
    V1BLEClient bleClient;
    bleClient.setConnected(true);
    server.setArg("address", "11:22:33");
    server.setArg("name", "OBD");
    server.setArg("pin", "0000");
    server.setArg("remember", "true");
    server.setArg("autoConnect", "1");

    ObdApiService::handleConnect(server, obdHandler, bleClient);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "OBD connect queued"));
    TEST_ASSERT_EQUAL_INT(1, obdHandler.connectCalls);
    TEST_ASSERT_EQUAL_STRING("11:22:33", obdHandler.lastConnectAddress.c_str());
    TEST_ASSERT_EQUAL_STRING("OBD", obdHandler.lastConnectName.c_str());
    TEST_ASSERT_EQUAL_STRING("0000", obdHandler.lastConnectPin.c_str());
    TEST_ASSERT_TRUE(obdHandler.lastConnectRemember);
    TEST_ASSERT_TRUE(obdHandler.lastConnectAutoConnect);
}

void test_handle_connect_reports_queue_failure() {
    WebServer server(80);
    OBDHandler obdHandler;
    obdHandler.connectReturn = false;
    V1BLEClient bleClient;
    bleClient.setConnected(true);
    server.setArg("address", "11:22:33");

    ObdApiService::handleConnect(server, obdHandler, bleClient);
    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Failed to queue OBD connect"));
}

void test_handle_config_applies_vw_data_enabled() {
    WebServer server(80);
    OBDHandler obdHandler;
    SettingsManager settingsManagerLocal;
    settingsManagerLocal.settings.obdVwDataEnabled = true;
    server.setArg("vwDataEnabled", "off");

    ObdApiService::handleConfig(server, obdHandler, settingsManagerLocal);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_FALSE(settingsManagerLocal.settings.obdVwDataEnabled);
    TEST_ASSERT_EQUAL_INT(1, obdHandler.setVwDataEnabledCalls);
    TEST_ASSERT_FALSE(obdHandler.lastVwDataEnabled);
    TEST_ASSERT_TRUE(responseContains(server, "\"vwDataEnabled\":false"));
}

void test_handle_remembered_autoconnect_updates_flag() {
    WebServer server(80);
    OBDHandler obdHandler;
    server.setArg("plain", "{\"address\":\"AA:BB\",\"enabled\":true}");

    ObdApiService::handleRememberedAutoConnect(server, obdHandler);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, obdHandler.setRememberedAutoConnectCalls);
    TEST_ASSERT_EQUAL_STRING("AA:BB", obdHandler.lastRememberedAutoConnectAddress.c_str());
    TEST_ASSERT_TRUE(obdHandler.lastRememberedAutoConnectEnabled);
}

void test_handle_forget_reports_missing_address() {
    WebServer server(80);
    OBDHandler obdHandler;

    ObdApiService::handleForget(server, obdHandler);
    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Missing address"));
}

void test_send_status_reports_core_obd_fields() {
    WebServer server(80);
    OBDHandler obdHandler;
    V1BLEClient bleClient;
    bleClient.setConnected(true);
    obdHandler.setStateString("POLLING");
    obdHandler.setConnected(true);
    obdHandler.setScanActive(false);
    obdHandler.setConnectedDeviceName("Adapter");
    obdHandler.setConnectedDeviceAddress("AA:BB:CC");
    obdHandler.setValidData(true);

    OBDData data;
    data.speed_mph = 42.5f;
    data.speed_kph = 68.4f;
    data.rpm = 3100;
    data.voltage = 13.8f;
    data.valid = true;
    data.timestamp_ms = 900;
    data.oil_temp_c = 100;
    data.dsg_temp_c = -128;
    data.intake_air_temp_c = 22;
    obdHandler.setData(data);

    std::vector<OBDRememberedDevice> remembered = {
        {"AA:BB:CC", "Adapter", "", true, 100},
        {"11:22:33", "Spare", "1234", false, 200},
    };
    obdHandler.setRememberedDevices(remembered);

    V1Settings settings;
    settings.obdVwDataEnabled = true;

    ObdApiService::sendStatus(server, obdHandler, bleClient, settings);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"POLLING\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"sampleAgeMs\":100"));
    TEST_ASSERT_TRUE(responseContains(server, "\"rememberedCount\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"autoConnectCount\":1"));
}

void test_send_status_uses_single_data_snapshot_for_validity() {
    WebServer server(80);
    OBDHandler obdHandler;
    V1BLEClient bleClient;

    // Deliberately disagree with getData() to verify sendStatus ignores
    // secondary validity calls and uses one consistent snapshot.
    obdHandler.setValidData(true);

    OBDData data;
    data.speed_mph = 41.0f;
    data.speed_kph = 66.0f;
    data.valid = false;
    data.timestamp_ms = 900;
    obdHandler.setData(data);

    V1Settings settings;
    ObdApiService::sendStatus(server, obdHandler, bleClient, settings);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"hasValidData\":false"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_connect_request_from_query_args);
    RUN_TEST(test_parse_connect_request_missing_address);
    RUN_TEST(test_handle_scan_requires_v1_connection);
    RUN_TEST(test_handle_scan_starts_scan_when_connected);
    RUN_TEST(test_handle_connect_queues_request);
    RUN_TEST(test_handle_connect_reports_queue_failure);
    RUN_TEST(test_handle_config_applies_vw_data_enabled);
    RUN_TEST(test_handle_remembered_autoconnect_updates_flag);
    RUN_TEST(test_handle_forget_reports_missing_address);
    RUN_TEST(test_send_status_reports_core_obd_fields);
    RUN_TEST(test_send_status_uses_single_data_snapshot_for_validity);
    return UNITY_END();
}

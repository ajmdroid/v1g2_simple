#include <unity.h>
#include <cstring>
#include <vector>

#include "../../src/modules/wifi/wifi_client_api_service.h"
#include "../../src/modules/wifi/wifi_client_api_service.cpp"  // Pull implementation for UNIT_TEST.

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

void test_parse_connect_request_missing_body() {
    WebServer server(80);
    String ssid;
    String password;
    const char* errorMessage = nullptr;

    TEST_ASSERT_FALSE(WifiClientApiService::parseConnectRequest(
        server, ssid, password, errorMessage));
    TEST_ASSERT_EQUAL_STRING("Missing request body", errorMessage);
}

void test_parse_connect_request_invalid_json() {
    WebServer server(80);
    server.setArg("plain", "{bad");
    String ssid;
    String password;
    const char* errorMessage = nullptr;

    TEST_ASSERT_FALSE(WifiClientApiService::parseConnectRequest(
        server, ssid, password, errorMessage));
    TEST_ASSERT_EQUAL_STRING("Invalid JSON", errorMessage);
}

void test_parse_connect_request_missing_ssid() {
    WebServer server(80);
    server.setArg("plain", "{\"password\":\"pw\"}");
    String ssid;
    String password;
    const char* errorMessage = nullptr;

    TEST_ASSERT_FALSE(WifiClientApiService::parseConnectRequest(
        server, ssid, password, errorMessage));
    TEST_ASSERT_EQUAL_STRING("SSID required", errorMessage);
}

void test_parse_connect_request_success() {
    WebServer server(80);
    server.setArg("plain", "{\"ssid\":\"GarageWiFi\",\"password\":\"secret\"}");
    String ssid;
    String password;
    const char* errorMessage = nullptr;

    TEST_ASSERT_TRUE(WifiClientApiService::parseConnectRequest(
        server, ssid, password, errorMessage));
    TEST_ASSERT_EQUAL_STRING("GarageWiFi", ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("secret", password.c_str());
    TEST_ASSERT_NULL(errorMessage);
}

void test_send_status_connected_includes_network_fields() {
    WebServer server(80);
    WifiClientApiService::StatusPayload payload;
    payload.enabled = true;
    payload.savedSsid = "SavedNet";
    payload.state = "connected";
    payload.scanRunning = false;
    payload.includeConnectedFields = true;
    payload.connectedSsid = "LiveNet";
    payload.ip = "192.168.1.42";
    payload.rssi = -61;

    WifiClientApiService::sendStatus(server, payload);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"savedSSID\":\"SavedNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"connected\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"connectedSSID\":\"LiveNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ip\":\"192.168.1.42\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"rssi\":-61"));
    TEST_ASSERT_TRUE(responseContains(server, "\"scanRunning\":false"));
}

void test_send_status_disconnected_omits_connected_fields() {
    WebServer server(80);
    WifiClientApiService::StatusPayload payload;
    payload.enabled = false;
    payload.savedSsid = "";
    payload.state = "disabled";
    payload.scanRunning = true;
    payload.includeConnectedFields = false;

    WifiClientApiService::sendStatus(server, payload);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"disabled\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"scanRunning\":true"));
    TEST_ASSERT_FALSE(responseContains(server, "\"connectedSSID\""));
    TEST_ASSERT_FALSE(responseContains(server, "\"ip\""));
}

void test_send_scan_results_includes_networks() {
    WebServer server(80);
    std::vector<WifiClientApiService::ScannedNetworkPayload> networks;

    WifiClientApiService::ScannedNetworkPayload first;
    first.ssid = "OpenNet";
    first.rssi = -42;
    first.secure = false;
    networks.push_back(first);

    WifiClientApiService::ScannedNetworkPayload second;
    second.ssid = "SecureNet";
    second.rssi = -70;
    second.secure = true;
    networks.push_back(second);

    WifiClientApiService::sendScanResults(server, networks);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"OpenNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"secure\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"SecureNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"secure\":true"));
}

void test_parse_enable_request_requires_boolean_field() {
    WebServer server(80);
    server.setArg("plain", "{\"enabled\":\"true\"}");
    bool enabled = false;

    TEST_ASSERT_FALSE(WifiClientApiService::parseEnableRequest(server, enabled));
}

void test_parse_enable_request_accepts_boolean_field() {
    WebServer server(80);
    server.setArg("plain", "{\"enabled\":true}");
    bool enabled = false;

    TEST_ASSERT_TRUE(WifiClientApiService::parseEnableRequest(server, enabled));
    TEST_ASSERT_TRUE(enabled);
}

void test_send_enable_parse_error_uses_expected_payload() {
    WebServer server(80);
    WifiClientApiService::sendEnableParseError(server);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing enabled field\""));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_connect_request_missing_body);
    RUN_TEST(test_parse_connect_request_invalid_json);
    RUN_TEST(test_parse_connect_request_missing_ssid);
    RUN_TEST(test_parse_connect_request_success);
    RUN_TEST(test_send_status_connected_includes_network_fields);
    RUN_TEST(test_send_status_disconnected_omits_connected_fields);
    RUN_TEST(test_send_scan_results_includes_networks);
    RUN_TEST(test_parse_enable_request_requires_boolean_field);
    RUN_TEST(test_parse_enable_request_accepts_boolean_field);
    RUN_TEST(test_send_enable_parse_error_uses_expected_payload);
    return UNITY_END();
}

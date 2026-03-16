#include <unity.h>
#include <cstring>
#include <vector>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/modules/wifi/wifi_json_document.h"
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
    mock_reset_heap_caps();
}

void tearDown() {}

struct FakeRuntime {
    bool enabled = false;
    String savedSsid;
    const char* stateName = "disabled";
    bool scanRunning = false;
    bool connected = false;
    WifiClientApiService::ConnectedNetworkPayload connectedNetwork;

    bool scanInProgress = false;
    bool hasCompletedResults = false;
    std::vector<WifiClientApiService::ScannedNetworkPayload> scannedNetworks;
    bool startScanReturn = false;

    bool connectReturn = true;
    int connectCalls = 0;
    String lastConnectSsid;
    String lastConnectPassword;

    int disconnectCalls = 0;
    int clearCredentialsCalls = 0;
    int setEnabledCalls = 0;
    bool lastSetEnabled = false;
    String savedPassword;
    int setStateDisabledCalls = 0;
    int setStateDisconnectedCalls = 0;
    int setApModeCalls = 0;
    int startScanCalls = 0;
};

static WifiClientApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiClientApiService::Runtime{
        [&rt]() { return rt.enabled; },
        [&rt]() { return rt.savedSsid; },
        [&rt]() { return rt.stateName; },
        [&rt]() { return rt.scanRunning; },
        [&rt]() { return rt.connected; },
        [&rt]() { return rt.connectedNetwork; },
        [&rt]() { return rt.scanInProgress; },
        [&rt]() { return rt.hasCompletedResults; },
        [&rt]() { return rt.scannedNetworks; },
        [&rt]() {
            rt.startScanCalls++;
            return rt.startScanReturn;
        },
        [&rt](const String& ssid, const String& password) {
            rt.connectCalls++;
            rt.lastConnectSsid = ssid;
            rt.lastConnectPassword = password;
            return rt.connectReturn;
        },
        [&rt]() { rt.disconnectCalls++; },
        [&rt]() { rt.clearCredentialsCalls++; },
        [&rt](bool enabled) {
            rt.setEnabledCalls++;
            rt.lastSetEnabled = enabled;
        },
        [&rt]() { return rt.savedPassword; },
        [&rt]() { rt.setStateDisabledCalls++; },
        [&rt]() { rt.setStateDisconnectedCalls++; },
        [&rt]() { rt.setApModeCalls++; },
    };
}

void test_handle_connect_missing_body_returns_400() {
    WebServer server(80);
    FakeRuntime rt;

    WifiClientApiService::handleApiConnect(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"Missing request body\""));
    TEST_ASSERT_EQUAL_INT(0, rt.connectCalls);
}

void test_handle_connect_invalid_json_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{bad");

    WifiClientApiService::handleApiConnect(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"Invalid JSON\""));
    TEST_ASSERT_EQUAL_INT(0, rt.connectCalls);
}

void test_handle_connect_missing_ssid_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"password\":\"pw\"}");

    WifiClientApiService::handleApiConnect(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"SSID required\""));
    TEST_ASSERT_EQUAL_INT(0, rt.connectCalls);
}

void test_handle_connect_valid_payload_returns_200() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"ssid\":\"GarageWiFi\",\"password\":\"secret\"}");

    WifiClientApiService::handleApiConnect(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"Connecting...\""));
    TEST_ASSERT_EQUAL_INT(1, rt.connectCalls);
    TEST_ASSERT_EQUAL_STRING("GarageWiFi", rt.lastConnectSsid.c_str());
    TEST_ASSERT_EQUAL_STRING("secret", rt.lastConnectPassword.c_str());
}

void test_handle_status_connected_includes_network_fields() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enabled = true;
    rt.savedSsid = "SavedNet";
    rt.stateName = "connected";
    rt.scanRunning = false;
    rt.connected = true;
    rt.connectedNetwork.ssid = "LiveNet";
    rt.connectedNetwork.ip = "192.168.1.42";
    rt.connectedNetwork.rssi = -61;

    WifiClientApiService::handleApiStatus(server, makeRuntime(rt), nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"savedSSID\":\"SavedNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"connected\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"connectedSSID\":\"LiveNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ip\":\"192.168.1.42\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"rssi\":-61"));
    TEST_ASSERT_TRUE(responseContains(server, "\"scanRunning\":false"));
}

void test_handle_status_disconnected_omits_connected_fields() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enabled = false;
    rt.savedSsid = "";
    rt.stateName = "disabled";
    rt.scanRunning = true;
    rt.connected = false;

    WifiClientApiService::handleApiStatus(server, makeRuntime(rt), nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"disabled\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"scanRunning\":true"));
    TEST_ASSERT_FALSE(responseContains(server, "\"connectedSSID\""));
    TEST_ASSERT_FALSE(responseContains(server, "\"ip\""));
}

void test_handle_status_repeated_requests_release_wifi_json_allocations() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enabled = true;
    rt.savedSsid = "SavedNet";
    rt.stateName = "connected";
    rt.connected = true;
    rt.connectedNetwork.ssid = "LiveNet";
    rt.connectedNetwork.ip = "192.168.4.10";
    rt.connectedNetwork.rssi = -55;

    mock_reset_heap_caps_tracking();

    for (int i = 0; i < 5; ++i) {
        WifiClientApiService::handleApiStatus(server, makeRuntime(rt), nullptr);
    }

    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

void test_handle_scan_completed_includes_networks() {
    WebServer server(80);
    FakeRuntime rt;
    rt.scanRunning = true;
    rt.scanInProgress = false;
    rt.hasCompletedResults = true;

    WifiClientApiService::ScannedNetworkPayload first;
    first.ssid = "OpenNet";
    first.rssi = -42;
    first.secure = false;
    rt.scannedNetworks.push_back(first);

    WifiClientApiService::ScannedNetworkPayload second;
    second.ssid = "SecureNet";
    second.rssi = -70;
    second.secure = true;
    rt.scannedNetworks.push_back(second);

    WifiClientApiService::handleApiScan(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"OpenNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"secure\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"SecureNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"secure\":true"));
}

void test_handle_enable_rejects_non_boolean_enabled() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"enabled\":\"true\"}");

    mock_reset_heap_caps_tracking();

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing enabled field\""));
    TEST_ASSERT_EQUAL_INT(0, rt.setEnabledCalls);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

void test_handle_enable_accepts_boolean_enabled() {
    WebServer server(80);
    FakeRuntime rt;
    rt.savedSsid = "";
    server.setArg("plain", "{\"enabled\":true}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"WiFi client enabled\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastSetEnabled);
}

void test_handle_enable_missing_field_uses_expected_payload() {
    WebServer server(80);
    FakeRuntime rt;

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing enabled field\""));
    TEST_ASSERT_EQUAL_INT(0, rt.setEnabledCalls);
}

void test_handle_status_connected_uses_runtime_payload() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enabled = true;
    rt.savedSsid = "SavedNet";
    rt.stateName = "connected";
    rt.scanRunning = false;
    rt.connected = true;
    rt.connectedNetwork.ssid = "LiveNet";
    rt.connectedNetwork.ip = "192.168.4.10";
    rt.connectedNetwork.rssi = -55;

    WifiClientApiService::handleApiStatus(server, makeRuntime(rt), nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"savedSSID\":\"SavedNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"connected\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"connectedSSID\":\"LiveNet\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"ip\":\"192.168.4.10\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"rssi\":-55"));
    TEST_ASSERT_TRUE(responseContains(server, "\"scanRunning\":false"));
}

void test_handle_scan_in_progress_returns_scanning_true() {
    WebServer server(80);
    FakeRuntime rt;
    rt.scanRunning = true;
    rt.scanInProgress = true;

    WifiClientApiService::handleApiScan(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":true"));
    TEST_ASSERT_EQUAL_INT(0, rt.startScanCalls);
}

void test_handle_scan_completed_returns_networks() {
    WebServer server(80);
    FakeRuntime rt;
    rt.scanRunning = true;
    rt.scanInProgress = false;
    rt.hasCompletedResults = true;

    WifiClientApiService::ScannedNetworkPayload net;
    net.ssid = "Garage";
    net.rssi = -48;
    net.secure = true;
    rt.scannedNetworks.push_back(net);

    WifiClientApiService::handleApiScan(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"ssid\":\"Garage\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"secure\":true"));
    TEST_ASSERT_EQUAL_INT(0, rt.startScanCalls);
}

void test_handle_scan_starts_new_scan_when_no_results() {
    WebServer server(80);
    FakeRuntime rt;
    rt.scanRunning = false;
    rt.scanInProgress = false;
    rt.hasCompletedResults = false;
    rt.startScanReturn = true;

    WifiClientApiService::handleApiScan(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"scanning\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.startScanCalls);
}

void test_handle_connect_parse_error_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{bad");

    mock_reset_heap_caps_tracking();

    WifiClientApiService::handleApiConnect(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"Invalid JSON\""));
    TEST_ASSERT_EQUAL_INT(0, rt.connectCalls);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

void test_handle_connect_starts_connection() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"ssid\":\"GarageWiFi\",\"password\":\"secret\"}");

    WifiClientApiService::handleApiConnect(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"Connecting...\""));
    TEST_ASSERT_EQUAL_INT(1, rt.connectCalls);
    TEST_ASSERT_EQUAL_STRING("GarageWiFi", rt.lastConnectSsid.c_str());
    TEST_ASSERT_EQUAL_STRING("secret", rt.lastConnectPassword.c_str());
}

void test_handle_forget_clears_credentials_and_disables_sta() {
    WebServer server(80);
    FakeRuntime rt;

    WifiClientApiService::handleApiForget(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"WiFi credentials forgotten\""));
    TEST_ASSERT_EQUAL_INT(1, rt.disconnectCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.clearCredentialsCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.setStateDisabledCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.setApModeCalls);
}

void test_handle_enable_true_with_saved_credentials_starts_connect() {
    WebServer server(80);
    FakeRuntime rt;
    rt.savedSsid = "HomeNet";
    rt.savedPassword = "pw123";
    server.setArg("plain", "{\"enabled\":true}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"WiFi client enabled\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastSetEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.connectCalls);
    TEST_ASSERT_EQUAL_STRING("HomeNet", rt.lastConnectSsid.c_str());
    TEST_ASSERT_EQUAL_STRING("pw123", rt.lastConnectPassword.c_str());
    TEST_ASSERT_EQUAL_INT(0, rt.setStateDisconnectedCalls);
}

void test_handle_enable_true_with_saved_credentials_returns_500_when_connect_fails() {
    WebServer server(80);
    FakeRuntime rt;
    rt.savedSsid = "HomeNet";
    rt.savedPassword = "pw123";
    rt.connectReturn = false;
    server.setArg("plain", "{\"enabled\":true}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"Failed to start connection\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastSetEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.connectCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.setStateDisconnectedCalls);
}

void test_handle_enable_true_without_saved_credentials_sets_disconnected_state() {
    WebServer server(80);
    FakeRuntime rt;
    rt.savedSsid = "";
    server.setArg("plain", "{\"enabled\":true}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"WiFi client enabled\""));
    TEST_ASSERT_EQUAL_INT(1, rt.setEnabledCalls);
    TEST_ASSERT_TRUE(rt.lastSetEnabled);
    TEST_ASSERT_EQUAL_INT(0, rt.connectCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.setStateDisconnectedCalls);
}

void test_handle_enable_false_disables_sta_mode() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"enabled\":false}");

    WifiClientApiService::handleApiEnable(server, makeRuntime(rt), nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"WiFi client disabled\""));
    TEST_ASSERT_EQUAL_INT(1, rt.disconnectCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.setEnabledCalls);
    TEST_ASSERT_FALSE(rt.lastSetEnabled);
    TEST_ASSERT_EQUAL_INT(1, rt.setStateDisabledCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.setApModeCalls);
}

void test_handle_api_status_marks_ui_activity_and_delegates() {
    WebServer server(80);
    FakeRuntime rt;
    rt.enabled = true;
    rt.stateName = "connected";
    rt.connected = false;
    int uiActivityCalls = 0;

    WifiClientApiService::handleApiStatus(
        server,
        makeRuntime(rt),
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"state\":\"connected\""));
}

void test_handle_api_scan_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    WifiClientApiService::handleApiScan(
        server,
        makeRuntime(rt),
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.startScanCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_connect_delegates_when_allowed() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"ssid\":\"GarageWiFi\",\"password\":\"secret\"}");
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    WifiClientApiService::handleApiConnect(
        server,
        makeRuntime(rt),
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.connectCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
}

void test_handle_api_disconnect_delegates_when_allowed() {
    WebServer server(80);
    FakeRuntime rt;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    WifiClientApiService::handleApiDisconnect(
        server,
        makeRuntime(rt),
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.disconnectCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
}

void test_handle_api_forget_delegates_when_allowed() {
    WebServer server(80);
    FakeRuntime rt;
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    WifiClientApiService::handleApiForget(
        server,
        makeRuntime(rt),
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.disconnectCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.clearCredentialsCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
}

void test_handle_api_enable_delegates_when_allowed() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"enabled\":false}");
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    WifiClientApiService::handleApiEnable(
        server,
        makeRuntime(rt),
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.setEnabledCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_connect_missing_body_returns_400);
    RUN_TEST(test_handle_connect_invalid_json_returns_400);
    RUN_TEST(test_handle_connect_missing_ssid_returns_400);
    RUN_TEST(test_handle_connect_valid_payload_returns_200);
    RUN_TEST(test_handle_status_connected_includes_network_fields);
    RUN_TEST(test_handle_status_disconnected_omits_connected_fields);
    RUN_TEST(test_handle_status_repeated_requests_release_wifi_json_allocations);
    RUN_TEST(test_handle_scan_completed_includes_networks);
    RUN_TEST(test_handle_enable_rejects_non_boolean_enabled);
    RUN_TEST(test_handle_enable_accepts_boolean_enabled);
    RUN_TEST(test_handle_enable_missing_field_uses_expected_payload);
    RUN_TEST(test_handle_status_connected_uses_runtime_payload);
    RUN_TEST(test_handle_scan_in_progress_returns_scanning_true);
    RUN_TEST(test_handle_scan_completed_returns_networks);
    RUN_TEST(test_handle_scan_starts_new_scan_when_no_results);
    RUN_TEST(test_handle_connect_parse_error_returns_400);
    RUN_TEST(test_handle_connect_starts_connection);
    RUN_TEST(test_handle_forget_clears_credentials_and_disables_sta);
    RUN_TEST(test_handle_enable_true_with_saved_credentials_starts_connect);
    RUN_TEST(test_handle_enable_true_with_saved_credentials_returns_500_when_connect_fails);
    RUN_TEST(test_handle_enable_true_without_saved_credentials_sets_disconnected_state);
    RUN_TEST(test_handle_enable_false_disables_sta_mode);
    RUN_TEST(test_handle_api_status_marks_ui_activity_and_delegates);
    RUN_TEST(test_handle_api_scan_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_connect_delegates_when_allowed);
    RUN_TEST(test_handle_api_disconnect_delegates_when_allowed);
    RUN_TEST(test_handle_api_forget_delegates_when_allowed);
    RUN_TEST(test_handle_api_enable_delegates_when_allowed);
    return UNITY_END();
}

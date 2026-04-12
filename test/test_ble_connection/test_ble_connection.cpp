#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

std::string extractFunctionBody(const std::string& text, const std::string& signature) {
    const size_t sigPos = text.find(signature);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sigPos);

    const size_t braceStart = text.find('{', sigPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, braceStart);

    int depth = 0;
    for (size_t i = braceStart; i < text.size(); ++i) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            depth--;
            if (depth == 0) {
                return text.substr(braceStart, i - braceStart + 1);
            }
        }
    }

    TEST_FAIL_MESSAGE("Failed to locate function body end");
    return {};
}

static std::string projectRoot() {
    return std::string(PROJECT_DIR);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_async_connect_does_not_delete_bond() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "bool V1BLEClient::startAsyncConnect()");

    TEST_ASSERT_EQUAL(std::string::npos, body.find("NimBLEDevice::deleteBond("));
}

void test_disconnect_callback_still_defers_bond_heal() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body =
        extractFunctionBody(text, "void V1BLEClient::ClientCallbacks::onDisconnect");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("pendingDeleteBondAddr_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("pendingDeleteBond_ = true"));
}

void test_disconnect_callback_no_longer_stops_proxy_advertising_inline() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body =
        extractFunctionBody(text, "void V1BLEClient::ClientCallbacks::onDisconnect");

    TEST_ASSERT_EQUAL(std::string::npos, body.find("NimBLEDevice::stopAdvertising("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("enqueueProxyCallbackEvent"));
}

void test_manual_obd_preempt_disconnects_proxy_from_main_loop() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "void V1BLEClient::process()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("PREEMPT_PROXY_FOR_MANUAL_SCAN"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("stopProxyAdvertisingFromMainLoop("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("pServer_->disconnect("));
}

void test_scan_stopping_uses_instance_owned_results_cleared_state() {
    const std::filesystem::path runtimeSource =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::filesystem::path clientSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::string runtimeText = readFile(runtimeSource);
    const std::string clientText = readFile(clientSource);
    const std::string processBody = extractFunctionBody(runtimeText, "void V1BLEClient::process()");
    const std::string setStateBody = extractFunctionBody(clientText, "void V1BLEClient::setBLEState");
    const std::string cleanupBody = extractFunctionBody(clientText, "void V1BLEClient::cleanupConnection()");

    TEST_ASSERT_EQUAL(std::string::npos, processBody.find("static bool resultsCleared"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, processBody.find("scanStopResultsCleared_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, setStateBody.find("scanStopResultsCleared_ = false"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, cleanupBody.find("scanStopResultsCleared_ = false"));
}

void test_destructor_clears_instance_ptr_only_for_active_instance() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "V1BLEClient::~V1BLEClient()");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("if (instancePtr == this)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("instancePtr = nullptr"));
}

void test_connect_to_server_removes_unused_addr_type_local() {
    const std::filesystem::path source =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "bool V1BLEClient::connectToServer()");

    TEST_ASSERT_EQUAL(std::string::npos, body.find("addrType"));
}

void test_ble_mutex_trylocks_use_semaphore_guard_in_runtime_and_callbacks() {
    const std::filesystem::path runtimeSource =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::filesystem::path connectionSource =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::string runtimeText = readFile(runtimeSource);
    const std::string connectionText = readFile(connectionSource);

    TEST_ASSERT_EQUAL(std::string::npos, runtimeText.find("xSemaphoreTake(bleMutex_"));
    TEST_ASSERT_EQUAL(std::string::npos, runtimeText.find("xSemaphoreGive(bleMutex_"));
    TEST_ASSERT_EQUAL(std::string::npos, connectionText.find("xSemaphoreTake(instancePtr->bleMutex_"));
    TEST_ASSERT_EQUAL(std::string::npos, connectionText.find("xSemaphoreGive(instancePtr->bleMutex_"));
    TEST_ASSERT_EQUAL(std::string::npos, connectionText.find("xSemaphoreTake(bleClient->bleMutex_"));
    TEST_ASSERT_EQUAL(std::string::npos, connectionText.find("xSemaphoreGive(bleClient->bleMutex_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeText.find("SemaphoreGuard lock(bleMutex_, 0)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectionText.find("SemaphoreGuard lock(instancePtr->bleMutex_, 0)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectionText.find("SemaphoreGuard lock(bleClient->bleMutex_, 0)"));
}

void test_connected_flag_uses_explicit_atomic_load_store() {
    const std::filesystem::path clientSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::filesystem::path runtimeSource =
        std::filesystem::path(projectRoot() + "/src/ble_runtime.cpp");
    const std::filesystem::path connectionSource =
        std::filesystem::path(projectRoot() + "/src/ble_connection.cpp");
    const std::filesystem::path proxySource =
        std::filesystem::path(projectRoot() + "/src/ble_proxy.cpp");
    const std::string clientText = readFile(clientSource);
    const std::string runtimeText = readFile(runtimeSource);
    const std::string connectionText = readFile(connectionSource);
    const std::string proxyText = readFile(proxySource);

    TEST_ASSERT_NOT_EQUAL(std::string::npos, clientText.find("connected_.load(std::memory_order_acquire)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeText.find("connected_.store(false, std::memory_order_relaxed)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, runtimeText.find("connected_.store(true, std::memory_order_relaxed)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectionText.find("connected_.store(true, std::memory_order_relaxed)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectionText.find("connected_.store(false, std::memory_order_release)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, proxyText.find("connected_.load(std::memory_order_relaxed)"));
}

void test_ble_timing_state_and_rssi_caches_use_uint32() {
    const std::filesystem::path headerSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.h");
    const std::filesystem::path clientSource =
        std::filesystem::path(projectRoot() + "/src/ble_client.cpp");
    const std::string headerText = readFile(headerSource);
    const std::string clientText = readFile(clientSource);

    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t lastScanStart_;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t stateEnteredMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t scanStopRequestedMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t connectStartMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t nextConnectAllowedMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t proxyAdvertisingStartMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t proxyAdvertisingWindowStartMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t proxyAdvertisingRetryAtMs_ = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, headerText.find("uint32_t proxyNoClientDeadlineMs_ = 0;"));
    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("unsigned long lastScanStart_"));
    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("unsigned long stateEnteredMs_"));
    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("unsigned long connectStartMs_"));
    TEST_ASSERT_EQUAL(std::string::npos, headerText.find("unsigned long nextConnectAllowedMs_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, clientText.find("static uint32_t s_lastV1RssiQueryMs = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, clientText.find("static constexpr uint32_t RSSI_QUERY_INTERVAL_MS = 2000;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, clientText.find("static uint32_t s_lastProxyRssiQueryMs = 0;"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_async_connect_does_not_delete_bond);
    RUN_TEST(test_disconnect_callback_still_defers_bond_heal);
    RUN_TEST(test_disconnect_callback_no_longer_stops_proxy_advertising_inline);
    RUN_TEST(test_manual_obd_preempt_disconnects_proxy_from_main_loop);
    RUN_TEST(test_scan_stopping_uses_instance_owned_results_cleared_state);
    RUN_TEST(test_destructor_clears_instance_ptr_only_for_active_instance);
    RUN_TEST(test_connect_to_server_removes_unused_addr_type_local);
    RUN_TEST(test_ble_mutex_trylocks_use_semaphore_guard_in_runtime_and_callbacks);
    RUN_TEST(test_connected_flag_uses_explicit_atomic_load_store);
    RUN_TEST(test_ble_timing_state_and_rssi_caches_use_uint32);
    return UNITY_END();
}

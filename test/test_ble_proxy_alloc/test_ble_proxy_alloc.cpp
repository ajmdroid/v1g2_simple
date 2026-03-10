#include <unity.h>

#include "../mocks/Arduino.h"
#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../mocks/freertos/FreeRTOS.h"
#include "../mocks/freertos/task.h"
#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"

#ifndef configASSERT
#define configASSERT(expr) do { if (!(expr)) { TEST_FAIL_MESSAGE("configASSERT failed"); } } while (0)
#endif

#define private public
#include "../../src/ble_client.h"
#undef private

#include "../../src/perf_metrics.h"

PerfCounters perfCounters;

void perfRecordNotifyToProxyMs(uint32_t) {}
void perfRecordProxyAdvertisingTransition(bool, uint8_t, uint32_t) {}

portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE proxyCmdMux = portMUX_INITIALIZER_UNLOCKED;
V1BLEClient* instancePtr = nullptr;

V1BLEClient::V1BLEClient()
    : pClient(nullptr)
    , pRemoteService(nullptr)
    , pDisplayDataChar(nullptr)
    , pCommandChar(nullptr)
    , pCommandCharLong(nullptr)
    , pServer(nullptr)
    , pProxyService(nullptr)
    , pProxyNotifyChar(nullptr)
    , pProxyNotifyLongChar(nullptr)
    , pProxyWriteChar(nullptr)
    , proxyEnabled(false)
    , proxyServerInitialized(false)
    , proxyName_("V1-Proxy")
    , proxyQueue(nullptr)
    , phone2v1Queue(nullptr)
    , proxyQueuesInPsram(false)
    , dataCallback(nullptr)
    , connectCallback(nullptr)
    , hasTargetDevice(false)
    , targetAddress()
    , lastScanStart(0)
    , freshFlashBoot(false)
    , pScanCallbacks(nullptr)
    , pClientCallbacks(nullptr)
    , pProxyServerCallbacks(nullptr)
    , pProxyWriteCallbacks(nullptr) {
    instancePtr = this;
}

V1BLEClient::~V1BLEClient() {
    releaseProxyQueues();
}

void V1BLEClient::setProxyClientConnected(bool connectedState) {
    proxyClientConnected = connectedState;
}

SendResult V1BLEClient::sendCommandWithResult(const uint8_t*, size_t) {
    return SendResult::SENT;
}

#include "../../src/ble_proxy.cpp"

void setUp() {
    mock_reset_heap_caps();
    mock_reset_nimble_state();
    mockMillis = 0;
    mockMicros = 0;
}

void tearDown() {}

void test_allocateProxyQueues_prefers_psram_for_both_buffers() {
    V1BLEClient client;

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    TEST_ASSERT_NOT_NULL(client.proxyQueue);
    TEST_ASSERT_NOT_NULL(client.phone2v1Queue);
    TEST_ASSERT_TRUE(client.proxyQueuesInPsram);
    TEST_ASSERT_EQUAL_UINT32(2, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyQueueCount);
    TEST_ASSERT_EQUAL_UINT32(0, client.phone2v1QueueCount);
}

void test_allocateProxyQueues_falls_back_to_internal_when_psram_misses() {
    V1BLEClient client;
    g_mock_heap_caps_fail_on_call = 2u;

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    TEST_ASSERT_NOT_NULL(client.proxyQueue);
    TEST_ASSERT_NOT_NULL(client.phone2v1Queue);
    TEST_ASSERT_FALSE(client.proxyQueuesInPsram);
    TEST_ASSERT_EQUAL_UINT32(3, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL, g_mock_heap_caps_last_malloc_caps);
}

void test_initProxyServer_full_allocation_failure_disables_proxy_before_server_creation() {
    V1BLEClient client;
    client.proxyEnabled = true;
    g_mock_heap_caps_fail_call_mask = 0x0Fu;

    TEST_ASSERT_FALSE(client.initProxyServer("Proxy"));
    TEST_ASSERT_FALSE(client.proxyEnabled);
    TEST_ASSERT_NULL(client.proxyQueue);
    TEST_ASSERT_NULL(client.phone2v1Queue);
    TEST_ASSERT_FALSE(client.proxyQueuesInPsram);
    TEST_ASSERT_EQUAL_UINT32(4, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_nimble_state.createServerCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_nimble_state.createServiceCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_nimble_state.createCharacteristicCalls);
}

void test_initProxyServer_partial_failure_frees_partial_allocation_and_resets_state() {
    V1BLEClient client;
    client.proxyEnabled = true;
    g_mock_heap_caps_fail_call_mask = (1u << 1) | (1u << 2);

    TEST_ASSERT_FALSE(client.initProxyServer("Proxy"));
    TEST_ASSERT_FALSE(client.proxyEnabled);
    TEST_ASSERT_NULL(client.proxyQueue);
    TEST_ASSERT_NULL(client.phone2v1Queue);
    TEST_ASSERT_FALSE(client.proxyQueuesInPsram);
    TEST_ASSERT_EQUAL_UINT32(3, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(0, client.proxyQueueCount);
    TEST_ASSERT_EQUAL_UINT32(0, client.phone2v1QueueCount);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_nimble_state.createServerCalls);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_allocateProxyQueues_prefers_psram_for_both_buffers);
    RUN_TEST(test_allocateProxyQueues_falls_back_to_internal_when_psram_misses);
    RUN_TEST(test_initProxyServer_full_allocation_failure_disables_proxy_before_server_creation);
    RUN_TEST(test_initProxyServer_partial_failure_frees_partial_allocation_and_resets_state);

    return UNITY_END();
}

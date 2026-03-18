#include <unity.h>
#include <vector>

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
PerfExtendedMetrics perfExtended;

void perfRecordNotifyToProxyMs(uint32_t) {}
void perfRecordBleProxyStartUs(uint32_t) {}
void perfRecordProxyAdvertisingTransition(bool, uint8_t, uint32_t) {}

portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE proxyCmdMux = portMUX_INITIALIZER_UNLOCKED;
V1BLEClient* instancePtr = nullptr;

namespace {

SendResult g_sendCommandResult = SendResult::SENT;
std::vector<uint8_t> g_lastSentCommand;
std::vector<uint8_t> g_sentCommandHistory;

void resetPhoneCommandSendState() {
    g_sendCommandResult = SendResult::SENT;
    g_lastSentCommand.clear();
    g_sentCommandHistory.clear();
}

void assertPhoneCmdDropMetrics(const V1BLEClient& client,
                               uint32_t overflow,
                               uint32_t invalid,
                               uint32_t bleFail,
                               uint32_t lockBusy) {
    const PhoneCmdDropMetricsSnapshot snapshot = perfPhoneCmdDropMetricsSnapshot();
    JsonDocument doc;
    perfAppendPhoneCmdDropMetrics(doc, snapshot);

    TEST_ASSERT_EQUAL_UINT32(overflow, client.getPhoneCmdDropsOverflow());
    TEST_ASSERT_EQUAL_UINT32(invalid, client.getPhoneCmdDropsInvalid());
    TEST_ASSERT_EQUAL_UINT32(bleFail, client.getPhoneCmdDropsBleFail());
    TEST_ASSERT_EQUAL_UINT32(lockBusy, client.getPhoneCmdDropsLockBusy());
    TEST_ASSERT_EQUAL_UINT32(overflow, doc["phoneCmdDropsOverflow"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(invalid, doc["phoneCmdDropsInvalid"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(bleFail, doc["phoneCmdDropsBleFail"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(lockBusy, doc["phoneCmdDropsLockBusy"].as<uint32_t>());
}

}  // namespace

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
    , connectImmediateCallback(nullptr)
    , connectStableCallback(nullptr)
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

SendResult V1BLEClient::sendCommandWithResult(const uint8_t* data, size_t length) {
    if (data && length > 0) {
        g_lastSentCommand.assign(data, data + length);
        g_sentCommandHistory.push_back(data[0]);
    } else {
        g_lastSentCommand.clear();
    }
    return g_sendCommandResult;
}

#include "../../src/ble_proxy.cpp"

void setUp() {
    mock_reset_heap_caps();
    mock_reset_nimble_state();
    mockMillis = 0;
    mockMicros = 0;
    perfCounters.reset();
    perfExtended.reset();
    resetPhoneCommandSendState();
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

void test_phone_command_invalid_drop_updates_getters_and_metrics_payload() {
    V1BLEClient client;

    TEST_ASSERT_FALSE(client.enqueuePhoneCommand(nullptr, 0, 0xB2CE));

    assertPhoneCmdDropMetrics(client, 0, 1, 0, 0);
}

void test_phone_command_lock_busy_drop_updates_getters_and_metrics_payload() {
    V1BLEClient client;
    const uint8_t cmd[] = {0x11};

    client.phoneCmdMutex = nullptr;
    TEST_ASSERT_FALSE(client.enqueuePhoneCommand(cmd, sizeof(cmd), 0xB2CE));

    assertPhoneCmdDropMetrics(client, 0, 0, 0, 1);
}

void test_phone_command_overflow_drops_oldest_and_keeps_newest() {
    V1BLEClient client;

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.phoneCmdMutex = xSemaphoreCreateMutex();
    client.connected = true;

    for (uint8_t i = 1; i <= V1BLEClient::PHONE_CMD_QUEUE_SIZE + 1; ++i) {
        TEST_ASSERT_TRUE(client.enqueuePhoneCommand(&i, 1, 0xB2CE));
    }

    assertPhoneCmdDropMetrics(client, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(V1BLEClient::PHONE_CMD_QUEUE_SIZE, client.phone2v1QueueCount);

    while (client.processPhoneCommandQueue() == 1) {
    }

    TEST_ASSERT_EQUAL_UINT32(V1BLEClient::PHONE_CMD_QUEUE_SIZE, g_sentCommandHistory.size());
    TEST_ASSERT_EQUAL_UINT8(2, g_sentCommandHistory.front());
    TEST_ASSERT_EQUAL_UINT8(V1BLEClient::PHONE_CMD_QUEUE_SIZE + 1, g_sentCommandHistory.back());
}

void test_phone_command_ble_failure_updates_getters_and_metrics_payload() {
    V1BLEClient client;
    const uint8_t cmd[] = {0x22};

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.phoneCmdMutex = xSemaphoreCreateMutex();
    client.connected = true;
    TEST_ASSERT_TRUE(client.enqueuePhoneCommand(cmd, sizeof(cmd), 0xB2CE));

    g_sendCommandResult = SendResult::FAILED;
    TEST_ASSERT_EQUAL_INT(0, client.processPhoneCommandQueue());

    assertPhoneCmdDropMetrics(client, 0, 0, 1, 0);
}

void test_phone_command_drop_metrics_reset_zeroes_all_observable_surfaces() {
    V1BLEClient client;
    const uint8_t cmd[] = {0x33};

    TEST_ASSERT_TRUE(client.allocateProxyQueues());
    client.phoneCmdMutex = xSemaphoreCreateMutex();
    client.connected = true;

    TEST_ASSERT_FALSE(client.enqueuePhoneCommand(nullptr, 0, 0xB2CE));
    TEST_ASSERT_TRUE(client.enqueuePhoneCommand(cmd, sizeof(cmd), 0xB2CE));
    g_sendCommandResult = SendResult::FAILED;
    TEST_ASSERT_EQUAL_INT(0, client.processPhoneCommandQueue());
    assertPhoneCmdDropMetrics(client, 0, 1, 1, 0);

    perfCounters.reset();
    assertPhoneCmdDropMetrics(client, 0, 0, 0, 0);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_allocateProxyQueues_prefers_psram_for_both_buffers);
    RUN_TEST(test_allocateProxyQueues_falls_back_to_internal_when_psram_misses);
    RUN_TEST(test_initProxyServer_full_allocation_failure_disables_proxy_before_server_creation);
    RUN_TEST(test_initProxyServer_partial_failure_frees_partial_allocation_and_resets_state);
    RUN_TEST(test_phone_command_invalid_drop_updates_getters_and_metrics_payload);
    RUN_TEST(test_phone_command_lock_busy_drop_updates_getters_and_metrics_payload);
    RUN_TEST(test_phone_command_overflow_drops_oldest_and_keeps_newest);
    RUN_TEST(test_phone_command_ble_failure_updates_getters_and_metrics_payload);
    RUN_TEST(test_phone_command_drop_metrics_reset_zeroes_all_observable_surfaces);

    return UNITY_END();
}

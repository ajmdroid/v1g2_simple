#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/FS.h"
#include "../mocks/NimBLEDevice.h"
#include "../mocks/storage_manager.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#define private public
#include "../../src/ble_client.h"
#undef private

portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE proxyCmdMux = portMUX_INITIALIZER_UNLOCKED;
V1BLEClient* instancePtr = nullptr;

namespace {

fs::FS g_sdFs(std::filesystem::temp_directory_path() / "v1g2_ble_deferred_bond_backup");
int g_tryBackupCalls = 0;
int g_tryBackupResult = 1;
uint32_t g_lastAlertFollowupUs = 0;
uint32_t g_lastVersionFollowupUs = 0;
uint32_t g_lastStableCallbackUs = 0;

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

V1BLEClient::~V1BLEClient() {}

bool V1BLEClient::requestAlertData() { return true; }
bool V1BLEClient::requestVersion() { return true; }
bool V1BLEClient::isConnected() { return connected.load(std::memory_order_relaxed); }
int V1BLEClient::processPhoneCommandQueue() { return 0; }
void V1BLEClient::setBLEState(BLEState newState, const char*) { bleState = newState; }
void V1BLEClient::processConnectingWait() {}
void V1BLEClient::processDiscovering() {}
void V1BLEClient::processSubscribing() {}
void V1BLEClient::processSubscribeYield() {}
void V1BLEClient::releaseProxyQueues() {}
void V1BLEClient::startProxyAdvertising(uint8_t, bool) {}

void perfRecordBleFollowupRequestAlertUs(uint32_t us) { g_lastAlertFollowupUs = us; }
void perfRecordBleFollowupRequestVersionUs(uint32_t us) { g_lastVersionFollowupUs = us; }
void perfRecordBleConnectStableCallbackUs(uint32_t us) { g_lastStableCallbackUs = us; }

int V1BLEClient::tryBackupBondsToSD() {
    g_tryBackupCalls++;
    StorageManager::SDTryLock lock(storageManager.getSDMutex(), false);
    if (!lock) {
        return -1;
    }
    return g_tryBackupResult;
}

#include "../../src/ble_connected_followup.cpp"

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    g_tryBackupCalls = 0;
    g_tryBackupResult = 1;
    g_lastAlertFollowupUs = 0;
    g_lastVersionFollowupUs = 0;
    g_lastStableCallbackUs = 0;
    mock_reset_nimble_state();
    StorageManager::resetMockSdLockState();
    storageManager.reset();
    storageManager.setFilesystem(&g_sdFs, true);
}

void tearDown() {}

void test_followup_backup_step_marks_pending_without_inline_write() {
    V1BLEClient client;
    client.connectedFollowupStep = V1BLEClient::ConnectedFollowupStep::BACKUP_BONDS;
    client.lastBondBackupCount = 1;
    g_mock_nimble_state.bondCount = 2;

    client.processConnectedFollowup();

    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1BLEClient::ConnectedFollowupStep::NONE),
                          static_cast<int>(client.connectedFollowupStep));
    TEST_ASSERT_TRUE(client.pendingBondBackup);
    TEST_ASSERT_EQUAL_UINT8(2, client.pendingBondBackupCount);
    TEST_ASSERT_EQUAL_UINT32(0, client.pendingBondBackupRetryAtMs);
    TEST_ASSERT_EQUAL(0, g_tryBackupCalls);
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.tryAcquireCalls);
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.blockingAcquireCalls);
}

void test_followup_backup_step_noops_when_bond_count_is_already_backed_up() {
    V1BLEClient client;
    client.connectedFollowupStep = V1BLEClient::ConnectedFollowupStep::BACKUP_BONDS;
    client.lastBondBackupCount = 3;
    g_mock_nimble_state.bondCount = 3;

    client.processConnectedFollowup();

    TEST_ASSERT_FALSE(client.pendingBondBackup);
    TEST_ASSERT_EQUAL_UINT8(0xFF, client.pendingBondBackupCount);
    TEST_ASSERT_EQUAL(0, g_tryBackupCalls);
}

void test_service_deferred_bond_backup_retries_after_trylock_busy() {
    V1BLEClient client;
    client.pendingBondBackup = true;
    client.pendingBondBackupCount = 4;
    client.lastBondBackupCount = 3;
    StorageManager::mockSdLockState.failNextTryLockCount = 1;

    client.serviceDeferredBondBackup(1000);

    TEST_ASSERT_TRUE(client.pendingBondBackup);
    TEST_ASSERT_EQUAL_UINT8(3, client.lastBondBackupCount);
    TEST_ASSERT_EQUAL_UINT32(2000, client.pendingBondBackupRetryAtMs);
    TEST_ASSERT_EQUAL(1, g_tryBackupCalls);
    TEST_ASSERT_EQUAL_UINT32(1, StorageManager::mockSdLockState.tryAcquireCalls);

    client.serviceDeferredBondBackup(1500);
    TEST_ASSERT_EQUAL(1, g_tryBackupCalls);
    TEST_ASSERT_TRUE(client.pendingBondBackup);
}

void test_service_deferred_bond_backup_success_clears_pending_and_updates_count() {
    V1BLEClient client;
    client.pendingBondBackup = true;
    client.pendingBondBackupCount = 5;
    client.lastBondBackupCount = 2;
    g_tryBackupResult = 2;

    client.serviceDeferredBondBackup(5000);

    TEST_ASSERT_FALSE(client.pendingBondBackup);
    TEST_ASSERT_EQUAL_UINT8(5, client.lastBondBackupCount);
    TEST_ASSERT_EQUAL_UINT32(0, client.pendingBondBackupRetryAtMs);
    TEST_ASSERT_EQUAL(1, g_tryBackupCalls);
    TEST_ASSERT_EQUAL_UINT32(1, StorageManager::mockSdLockState.tryAcquireCalls);
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.blockingAcquireCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_followup_backup_step_marks_pending_without_inline_write);
    RUN_TEST(test_followup_backup_step_noops_when_bond_count_is_already_backed_up);
    RUN_TEST(test_service_deferred_bond_backup_retries_after_trylock_busy);
    RUN_TEST(test_service_deferred_bond_backup_success_clears_pending_and_updates_count);
    return UNITY_END();
}

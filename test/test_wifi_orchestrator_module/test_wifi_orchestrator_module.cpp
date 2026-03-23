#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/WiFi.h"
#include "../mocks/display.h"
#include "../mocks/ble_client.h"
#include "../mocks/packet_parser.h"
#include "../mocks/settings.h"
#include "../mocks/storage_manager.h"
#include "wifi_manager_test_stub.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/lockout/lockout_runtime_mute_controller.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/wifi/wifi_orchestrator_module.cpp"

String AutoPushModule::getStatusJson() const {
    return String("{}");
}

AutoPushModule::QueueResult AutoPushModule::queueSlotPush(int, bool, bool) {
    return QueueResult::QUEUED;
}

AutoPushModule::QueueResult AutoPushModule::queuePushNow(const PushNowRequest&) {
    return QueueResult::QUEUED;
}

namespace {

WiFiManager wifiManagerMock;
V1BLEClient bleClientMock;
PacketParser parserMock;
SettingsManager settingsManagerMock;
StorageManager storageManagerMock;
AutoPushModule autoPushModuleMock;
QuietCoordinatorModule quietCoordinatorMock;
WifiOrchestrator* orchestrator = nullptr;

void expectCallbacksBoundOnce() {
    TEST_ASSERT_EQUAL(1, wifiManagerMock.statusCallbackCalls);
    TEST_ASSERT_EQUAL(1, wifiManagerMock.alertCallbackCalls);
    TEST_ASSERT_EQUAL(1, wifiManagerMock.commandCallbackCalls);
    TEST_ASSERT_EQUAL(1, wifiManagerMock.filesystemCallbackCalls);
    TEST_ASSERT_EQUAL(1, wifiManagerMock.profilePushCallbackCalls);
    TEST_ASSERT_EQUAL(1, wifiManagerMock.pushStatusCallbackCalls);
    TEST_ASSERT_EQUAL(1, wifiManagerMock.pushNowCallbackCalls);
    TEST_ASSERT_EQUAL(1, wifiManagerMock.v1ConnectedCallbackCalls);
}

}  // namespace

void setUp() {
    wifiManagerMock.reset();
    bleClientMock.reset();
    parserMock.reset();
    settingsManagerMock = SettingsManager{};
    storageManagerMock.reset();
    autoPushModuleMock = AutoPushModule{};
    quietCoordinatorMock.begin(&bleClientMock, &parserMock);
    orchestrator = new WifiOrchestrator(
        wifiManagerMock,
        bleClientMock,
        parserMock,
        settingsManagerMock,
        storageManagerMock,
        autoPushModuleMock,
        quietCoordinatorMock);
}

void tearDown() {
    delete orchestrator;
    orchestrator = nullptr;
}

void test_ensure_callbacks_configured_binds_callbacks_when_wifi_was_started_elsewhere() {
    wifiManagerMock.setupModeActive = true;

    orchestrator->ensureCallbacksConfigured();

    expectCallbacksBoundOnce();
    TEST_ASSERT_EQUAL(0, wifiManagerMock.startSetupModeCalls);
}

void test_ensure_callbacks_configured_is_idempotent_without_starting_wifi() {
    orchestrator->ensureCallbacksConfigured();
    orchestrator->ensureCallbacksConfigured();

    expectCallbacksBoundOnce();
    TEST_ASSERT_EQUAL(0, wifiManagerMock.startSetupModeCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ensure_callbacks_configured_binds_callbacks_when_wifi_was_started_elsewhere);
    RUN_TEST(test_ensure_callbacks_configured_is_idempotent_without_starting_wifi);
    return UNITY_END();
}

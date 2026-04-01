#include <unity.h>

#include <filesystem>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"
#include "../mocks/nvs.h"
#include "../mocks/storage_manager.h"
#include "../../src/settings.h"
#include "../../src/v1_profiles.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace ArduinoJson {

inline void convertFromJson(JsonVariantConst src, ::String& dst) {
    const char* raw = src.as<const char*>();
    dst = ::String(raw ? raw : "");
}

inline bool canConvertFromJson(JsonVariantConst src, const ::String&) {
    return src.is<const char*>();
}

}  // namespace ArduinoJson

#include "../../src/v1_profiles.cpp"
#include "../../src/backup_payload_builder.cpp"
#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/settings.cpp"
#include "../../src/settings_nvs.cpp"
#include "../../src/settings_backup.cpp"
#include "../../src/settings_deferred_backup.cpp"
#include "../../src/settings_restore.cpp"

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
    return std::filesystem::temp_directory_path() /
           ("settings_deferred_backup_" + std::to_string(++g_tempRootIndex));
}

String activeNamespaceOrEmpty() {
    return mock_preferences::getString(SETTINGS_NS_META, "active", "");
}

bool loadJsonFile(fs::FS& fs, const char* path, JsonDocument& doc) {
    File file = fs.open(path, FILE_READ);
    if (!file) {
        return false;
    }
    const DeserializationError err = deserializeJson(doc, file);
    file.close();
    return !err;
}

void resetRuntimeState() {
    mock_preferences::reset();
    mock_nvs::reset();
    mock_reset_heap_caps();
    mock_reset_queue_create_state();
    mock_reset_task_create_state();
    storageManager.reset();
    StorageManager::resetMockSdLockState();
    resetDeferredSettingsBackupStateForTest();
    v1ProfileManager = V1ProfileManager();
    settingsManager = SettingsManager();
    mockMillis = 1000;
    mockMicros = 1000000;
}

}  // namespace

void setUp() {
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
    resetRuntimeState();
}

void tearDown() {
    std::filesystem::remove_all(g_tempRoot);
    resetDeferredSettingsBackupStateForTest();
}

void test_save_deferred_backup_persists_nvs_and_writes_snapshot_via_writer() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "DeferredSave";
    manager.mutableSettings().brightness = 88;

    manager.saveDeferredBackup();

    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());
    TEST_ASSERT_FALSE(manager.deferredBackupRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(0u, manager.deferredBackupNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), "apSSID"));

    manager.serviceDeferredBackup(1000);

    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_queue_create_state.staticCalls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_task_create_state.capsCalls);

    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("DeferredSave", backupDoc["apSSID"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(88, backupDoc["brightness"].as<int>());
}

void test_service_deferred_backup_retries_after_sd_trylock_busy() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "RetryPath";
    manager.requestDeferredBackupFromCurrentState();

    StorageManager::mockSdLockState.failNextTryLockCount = 1;
    manager.serviceDeferredBackup(1000);

    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());
    TEST_ASSERT_TRUE(manager.deferredBackupRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(1250u, manager.deferredBackupNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_EQUAL_UINT32(1u, StorageManager::mockSdLockState.tryAcquireCalls);

    manager.serviceDeferredBackup(1200);
    TEST_ASSERT_EQUAL_UINT32(1u, StorageManager::mockSdLockState.tryAcquireCalls);
    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());
    TEST_ASSERT_TRUE(manager.deferredBackupRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(1250u, manager.deferredBackupNextAttemptAtMs());

    manager.serviceDeferredBackup(1250);
    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_FALSE(manager.deferredBackupPending());
    TEST_ASSERT_FALSE(manager.deferredBackupRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(0u, manager.deferredBackupNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_EQUAL_UINT32(2u, StorageManager::mockSdLockState.tryAcquireCalls);
}

void test_repeated_requests_coalesce_to_latest_snapshot() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "OldValue";
    manager.requestDeferredBackupFromCurrentState();
    manager.mutableSettings().apSSID = "NewValue";
    manager.requestDeferredBackupFromCurrentState();

    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("NewValue", backupDoc["apSSID"].as<const char*>());
}

void test_repeated_save_deferred_backup_calls_coalesce_to_latest_snapshot() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "FirstValue";
    manager.saveDeferredBackup();
    manager.mutableSettings().apSSID = "FinalValue";
    manager.saveDeferredBackup();

    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT32(3u, manager.backupRevision());

    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("FinalValue", backupDoc["apSSID"].as<const char*>());
}

void test_service_deferred_backup_keeps_pending_when_writer_setup_fails() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "QueueFail";
    manager.requestDeferredBackupFromCurrentState();

    g_mock_heap_caps_fail_malloc = true;
    g_mock_queue_create_state.failDynamic = true;

    manager.serviceDeferredBackup(1000);

    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());
}

void test_writer_failure_requeues_backup_request_for_rebuild() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "Initial";
    manager.requestDeferredBackupFromCurrentState();
    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());

    storageManager.reset();
    StorageManager::resetMockSdLockState();
    TEST_ASSERT_FALSE(runDeferredSettingsBackupWriterOnceForTest());
    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());

    storageManager.setFilesystem(&fs, true);
    manager.mutableSettings().apSSID = "Recovered";
    manager.serviceDeferredBackup(2000);

    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("Recovered", backupDoc["apSSID"].as<const char*>());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_save_deferred_backup_persists_nvs_and_writes_snapshot_via_writer);
    RUN_TEST(test_service_deferred_backup_retries_after_sd_trylock_busy);
    RUN_TEST(test_repeated_requests_coalesce_to_latest_snapshot);
    RUN_TEST(test_repeated_save_deferred_backup_calls_coalesce_to_latest_snapshot);
    RUN_TEST(test_service_deferred_backup_keeps_pending_when_writer_setup_fails);
    RUN_TEST(test_writer_failure_requeues_backup_request_for_rebuild);
    return UNITY_END();
}

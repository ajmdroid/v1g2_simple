#include <unity.h>

#include <cstring>

#include "../../src/modules/wifi/backup_api_service.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace BackupPayloadBuilder {

bool isRecognizedBackupType(const char* type) {
    return type != nullptr &&
           (std::strcmp(type, "v1simple_backup") == 0 ||
            std::strcmp(type, "v1simple_sd_backup") == 0);
}

}  // namespace BackupPayloadBuilder

namespace BackupApiService {

bool sendCachedBackupSnapshot(WebServer& server,
                              BackupSnapshotCache& /*cache*/,
                              uint32_t /*settingsRevision*/,
                              uint32_t /*profileRevision*/,
                              BackupSnapshotBuildFn /*buildSnapshot*/,
                              void* /*buildCtx*/,
                              uint32_t (*/*millisFn*/)(void* ctx),
                              void* /*millisCtx*/) {
    server.send(200, "application/json", "{\"cached\":true}");
    return true;
}

}  // namespace BackupApiService

#include "../../src/modules/wifi/backup_api_service.cpp"

namespace {

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeRuntime {
    bool applyBackupReturn = true;
    int applyBackupCalls = 0;
    int syncAfterRestoreCalls = 0;
    int profilesRestored = 0;
    bool lastFullRestore = false;
    String lastBackupType;
};

BackupApiService::BackupRuntime makeRuntime(FakeRuntime& runtime) {
    BackupApiService::BackupRuntime apiRuntime;
    apiRuntime.ctx = &runtime;
    apiRuntime.applyBackup = [](const JsonDocument& doc,
                                bool fullRestore,
                                int& profilesRestored,
                                void* ctx) {
        auto* fakeRuntime = static_cast<FakeRuntime*>(ctx);
        fakeRuntime->applyBackupCalls++;
        fakeRuntime->lastFullRestore = fullRestore;
        fakeRuntime->lastBackupType = doc["_type"].as<const char*>() ?
                                          doc["_type"].as<const char*>() :
                                          "";
        profilesRestored = fakeRuntime->profilesRestored;
        return fakeRuntime->applyBackupReturn;
    };
    apiRuntime.syncAfterRestore = [](void* ctx) {
        static_cast<FakeRuntime*>(ctx)->syncAfterRestoreCalls++;
    };
    return apiRuntime;
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_restore_missing_body_returns_400_without_apply_or_sync() {
    WebServer server(80);
    FakeRuntime runtime;

    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "No JSON body provided"));
    TEST_ASSERT_EQUAL_INT(0, runtime.applyBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, runtime.syncAfterRestoreCalls);
}

void test_restore_invalid_json_returns_400_without_apply_or_sync() {
    WebServer server(80);
    FakeRuntime runtime;
    server.setArg("plain", "{bad");

    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Invalid JSON"));
    TEST_ASSERT_EQUAL_INT(0, runtime.applyBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, runtime.syncAfterRestoreCalls);
}

void test_restore_invalid_backup_type_returns_400_without_apply_or_sync() {
    WebServer server(80);
    FakeRuntime runtime;
    server.setArg("plain", "{\"_type\":\"unsupported\",\"brightness\":77}");

    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Invalid backup format"));
    TEST_ASSERT_EQUAL_INT(0, runtime.applyBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, runtime.syncAfterRestoreCalls);
}

void test_restore_apply_failure_returns_500_and_skips_sync() {
    WebServer server(80);
    FakeRuntime runtime;
    runtime.applyBackupReturn = false;
    server.setArg("plain", "{\"_type\":\"v1simple_backup\",\"brightness\":77}");

    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Failed to persist restored settings"));
    TEST_ASSERT_EQUAL_INT(1, runtime.applyBackupCalls);
    TEST_ASSERT_TRUE(runtime.lastFullRestore);
    TEST_ASSERT_EQUAL_STRING("v1simple_backup", runtime.lastBackupType.c_str());
    TEST_ASSERT_EQUAL_INT(0, runtime.syncAfterRestoreCalls);
}

void test_restore_success_syncs_runtime_and_reports_profiles_restored() {
    WebServer server(80);
    FakeRuntime runtime;
    runtime.profilesRestored = 2;
    server.setArg("plain", "{\"_type\":\"v1simple_backup\",\"brightness\":77}");

    BackupApiService::handleApiRestore(server,
                                       makeRuntime(runtime),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "Settings restored successfully (2 profiles)"));
    TEST_ASSERT_EQUAL_INT(1, runtime.applyBackupCalls);
    TEST_ASSERT_TRUE(runtime.lastFullRestore);
    TEST_ASSERT_EQUAL_STRING("v1simple_backup", runtime.lastBackupType.c_str());
    TEST_ASSERT_EQUAL_INT(1, runtime.syncAfterRestoreCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_restore_missing_body_returns_400_without_apply_or_sync);
    RUN_TEST(test_restore_invalid_json_returns_400_without_apply_or_sync);
    RUN_TEST(test_restore_invalid_backup_type_returns_400_without_apply_or_sync);
    RUN_TEST(test_restore_apply_failure_returns_500_and_skips_sync);
    RUN_TEST(test_restore_success_syncs_runtime_and_reports_profiles_restored);
    return UNITY_END();
}

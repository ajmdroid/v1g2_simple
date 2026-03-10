#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/backup_api_service.h"
#include "../support/wrappers/backup_api_service_wrappers.cpp"  // Pull wrappers for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

int sendBackupCalls = 0;
int handleBackupNowCalls = 0;
int handleRestoreCalls = 0;

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

}  // namespace

namespace BackupApiService {

void sendBackup(WebServer& server,
                BackupSnapshotCache& /*cachedSnapshot*/,
                const std::function<uint32_t()>& /*millisFn*/) {
    sendBackupCalls++;
    server.sendHeader("Content-Disposition", "attachment; filename=\"v1simple_backup.json\"");
    server.send(200, "application/json", "{\"route\":\"backup\"}");
}

void handleBackupNow(WebServer& server) {
    handleBackupNowCalls++;
    server.send(200, "application/json", "{\"route\":\"backup-now\"}");
}

void handleRestore(WebServer& server) {
    handleRestoreCalls++;
    server.send(200, "application/json", "{\"route\":\"restore\"}");
}

}  // namespace BackupApiService

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    sendBackupCalls = 0;
    handleBackupNowCalls = 0;
    handleRestoreCalls = 0;
}

void tearDown() {}

void test_handle_api_backup_marks_ui_activity_and_delegates() {
    WebServer server(80);
    BackupApiService::BackupSnapshotCache cache;
    int uiActivityCalls = 0;

    BackupApiService::handleApiBackup(
        server,
        cache,
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, sendBackupCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"backup\""));
    TEST_ASSERT_EQUAL_STRING("attachment; filename=\"v1simple_backup.json\"",
                             server.sentHeader("Content-Disposition").c_str());
}

void test_handle_api_restore_rate_limited_short_circuits() {
    WebServer server(80);
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    BackupApiService::handleApiRestore(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, handleRestoreCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_backup_now_rate_limited_short_circuits() {
    WebServer server(80);
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    BackupApiService::handleApiBackupNow(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return false;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(0, handleBackupNowCalls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_backup_now_marks_ui_activity_and_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    BackupApiService::handleApiBackupNow(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handleBackupNowCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"backup-now\""));
}

void test_handle_api_restore_marks_ui_activity_and_delegates_when_allowed() {
    WebServer server(80);
    int rateLimitCalls = 0;
    int uiActivityCalls = 0;

    BackupApiService::handleApiRestore(
        server,
        [&rateLimitCalls]() {
            rateLimitCalls++;
            return true;
        },
        [&uiActivityCalls]() { uiActivityCalls++; });

    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);
    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(1, handleRestoreCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"restore\""));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_api_backup_marks_ui_activity_and_delegates);
    RUN_TEST(test_handle_api_restore_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_backup_now_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_backup_now_marks_ui_activity_and_delegates_when_allowed);
    RUN_TEST(test_handle_api_restore_marks_ui_activity_and_delegates_when_allowed);
    return UNITY_END();
}

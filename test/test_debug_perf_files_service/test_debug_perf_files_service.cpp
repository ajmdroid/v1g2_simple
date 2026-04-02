#include <unity.h>

#include <cstring>
#include <filesystem>

#include "../mocks/Arduino.h"
#include "../mocks/storage_manager.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 1000;
unsigned long mockMicros = 1000000;

#define private public
#include "../../src/perf_sd_logger.h"
#undef private

#include "../../src/modules/debug/debug_perf_files_service.h"
#include "../../src/modules/debug/debug_perf_files_service.cpp"

PerfSdLogger perfSdLogger;

namespace {

const std::filesystem::path kFsRoot =
    std::filesystem::temp_directory_path() / "debug_perf_files_service_tests";
fs::FS g_sdFs(kFsRoot);

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

void resetPerfLoggerState() {
    perfSdLogger.enabled_ = false;
    std::memset(perfSdLogger.csvPathBuf_, 0, sizeof(perfSdLogger.csvPathBuf_));
}

DebugPerfFilesService::PerfFilesRuntime makeTestRuntime() {
    return DebugPerfFilesService::PerfFilesRuntime{
        [](void*) -> bool { return storageManager.isReady(); },
        [](void*) -> bool { return storageManager.isSDCard(); },
        [](void*) -> void* { return storageManager.getSDMutex(); },
        [](void*) -> void* { return storageManager.getFilesystem(); },
        [](void*) -> bool { return perfSdLogger.isEnabled(); },
        [](void*) -> const char* { return perfSdLogger.csvPath(); },
        nullptr,
    };
}

void clearPerfFileCache() {
    storageManager.reset();
    WebServer server(80);
    DebugPerfFilesService::handleApiPerfFilesList(server, makeTestRuntime(), [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr);
}

void writePerfFile(const char* name, const char* content) {
    File file = g_sdFs.open(String("/perf/") + name, FILE_WRITE);
    TEST_ASSERT_TRUE(static_cast<bool>(file));
    TEST_ASSERT_EQUAL(strlen(content), file.write(reinterpret_cast<const uint8_t*>(content),
                                                  std::strlen(content)));
    file.close();
}

}  // namespace

void setUp() {
    std::error_code ec;
    std::filesystem::remove_all(kFsRoot, ec);
    std::filesystem::create_directories(kFsRoot, ec);
    mockMillis = 1000;
    mockMicros = 1000000;
    clearPerfFileCache();
    storageManager.reset();
    StorageManager::resetMockSdLockState();
    storageManager.setFilesystem(&g_sdFs, true);
    fs::mock_reset_fs_rename_state();
    resetPerfLoggerState();
    mock_reset_heap_caps();
}

void tearDown() {}

void test_perf_files_list_returns_rows_when_lock_is_free() {
    writePerfFile("20260316_020000_perf_7.csv", "ts,val\n1,2\n");

    WebServer server(80);
    DebugPerfFilesService::handleApiPerfFilesList(server, makeTestRuntime(), [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"20260316_020000_perf_7.csv\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"count\":1"));
    TEST_ASSERT_TRUE(responseContains(server, "\"downloadAllowed\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"deleteAllowed\":true"));
    TEST_ASSERT_EQUAL_UINT32(1, StorageManager::mockSdLockState.tryAcquireCalls);
}

void test_perf_files_list_marks_file_ops_blocked_while_logging_active() {
    writePerfFile("20260316_020000_perf_7.csv", "ts,val\n1,2\n");
    writePerfFile("20260316_010000_perf_6.csv", "ts,val\n3,4\n");
    perfSdLogger.enabled_ = true;
    std::strncpy(perfSdLogger.csvPathBuf_,
                 "/perf/20260316_020000_perf_7.csv",
                 sizeof(perfSdLogger.csvPathBuf_) - 1);

    WebServer server(80);
    DebugPerfFilesService::handleApiPerfFilesList(server, makeTestRuntime(), [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"loggingActive\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"fileOpsBlocked\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"fileOpsBlockedReasonCode\":\"perf_logging_active\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"activeFile\":\"20260316_020000_perf_7.csv\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"downloadAllowed\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"deleteAllowed\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"deleteAllowed\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"blockedReasonCode\":\"perf_logging_active\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"deleteBlockedReason\":\"Active perf log in use\""));
}

void test_perf_files_list_returns_503_when_sd_trylock_is_busy() {
    writePerfFile("20260316_020000_perf_7.csv", "ts,val\n1,2\n");
    StorageManager::mockSdLockState.failNextTryLockCount = 1;

    WebServer server(80);
    DebugPerfFilesService::handleApiPerfFilesList(server, makeTestRuntime(), [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr);

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"SD busy\""));
    TEST_ASSERT_EQUAL_UINT32(1, StorageManager::mockSdLockState.tryAcquireCalls);
}

void test_perf_file_download_returns_503_while_perf_logging_active() {
    writePerfFile("20260316_020000_perf_7.csv", "ts,val\n1,2\n");
    perfSdLogger.enabled_ = true;
    std::strncpy(perfSdLogger.csvPathBuf_,
                 "/perf/20260316_020000_perf_7.csv",
                 sizeof(perfSdLogger.csvPathBuf_) - 1);

    WebServer server(80);
    server.setArg("name", "20260316_020000_perf_7.csv");
    DebugPerfFilesService::handleApiPerfFilesDownload(server, makeTestRuntime(), [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr);

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Perf logging active"));
    TEST_ASSERT_TRUE(responseContains(server, "\"reasonCode\":\"perf_logging_active\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"operation\":\"download\""));
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.tryAcquireCalls);
}

void test_perf_file_download_streams_csv_when_idle() {
    writePerfFile("20260316_020000_perf_7.csv", "ts,val\n1,2\n");

    WebServer server(80);
    server.setArg("name", "20260316_020000_perf_7.csv");
    DebugPerfFilesService::handleApiPerfFilesDownload(server, makeTestRuntime(), [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/csv", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_UINT32(11, server.lastContentLength);
    TEST_ASSERT_EQUAL_STRING("attachment; filename=\"20260316_020000_perf_7.csv\"",
                             server.sentHeader("Content-Disposition").c_str());
    TEST_ASSERT_EQUAL_STRING("ts,val\n1,2\n", server.lastBody.c_str());
}

void test_perf_file_delete_returns_503_when_sd_trylock_is_busy() {
    writePerfFile("20260316_020000_perf_7.csv", "ts,val\n1,2\n");
    StorageManager::mockSdLockState.failNextTryLockCount = 1;

    WebServer server(80);
    server.setArg("name", "20260316_020000_perf_7.csv");
    DebugPerfFilesService::handleApiPerfFilesDelete(server, makeTestRuntime(), [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr);

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"SD busy\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"reasonCode\":\"sd_busy\""));
    TEST_ASSERT_TRUE(g_sdFs.exists("/perf/20260316_020000_perf_7.csv"));
}

void test_perf_file_delete_returns_503_while_perf_logging_active() {
    writePerfFile("20260316_020000_perf_7.csv", "ts,val\n1,2\n");
    perfSdLogger.enabled_ = true;
    std::strncpy(perfSdLogger.csvPathBuf_,
                 "/perf/20260316_020000_perf_7.csv",
                 sizeof(perfSdLogger.csvPathBuf_) - 1);

    WebServer server(80);
    server.setArg("name", "20260316_020000_perf_7.csv");
    DebugPerfFilesService::handleApiPerfFilesDelete(server, makeTestRuntime(), [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr);

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Active perf log in use"));
    TEST_ASSERT_TRUE(responseContains(server, "\"reasonCode\":\"perf_logging_active\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"operation\":\"delete\""));
    TEST_ASSERT_TRUE(g_sdFs.exists("/perf/20260316_020000_perf_7.csv"));
    TEST_ASSERT_EQUAL_UINT32(0, StorageManager::mockSdLockState.tryAcquireCalls);
}

void test_perf_file_delete_allows_inactive_file_while_perf_logging_active() {
    writePerfFile("20260316_020000_perf_7.csv", "ts,val\n1,2\n");
    writePerfFile("20260316_010000_perf_6.csv", "ts,val\n3,4\n");
    perfSdLogger.enabled_ = true;
    std::strncpy(perfSdLogger.csvPathBuf_,
                 "/perf/20260316_020000_perf_7.csv",
                 sizeof(perfSdLogger.csvPathBuf_) - 1);

    WebServer server(80);
    server.setArg("name", "20260316_010000_perf_6.csv");
    DebugPerfFilesService::handleApiPerfFilesDelete(server, makeTestRuntime(), [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_FALSE(g_sdFs.exists("/perf/20260316_010000_perf_6.csv"));
    TEST_ASSERT_TRUE(g_sdFs.exists("/perf/20260316_020000_perf_7.csv"));
    TEST_ASSERT_EQUAL_UINT32(1, StorageManager::mockSdLockState.tryAcquireCalls);
}

void test_perf_file_delete_removes_file_when_lock_is_free() {
    writePerfFile("20260316_020000_perf_7.csv", "ts,val\n1,2\n");

    WebServer server(80);
    server.setArg("name", "20260316_020000_perf_7.csv");
    DebugPerfFilesService::handleApiPerfFilesDelete(server, makeTestRuntime(), [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_FALSE(g_sdFs.exists("/perf/20260316_020000_perf_7.csv"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_perf_files_list_returns_rows_when_lock_is_free);
    RUN_TEST(test_perf_files_list_marks_file_ops_blocked_while_logging_active);
    RUN_TEST(test_perf_files_list_returns_503_when_sd_trylock_is_busy);
    RUN_TEST(test_perf_file_download_returns_503_while_perf_logging_active);
    RUN_TEST(test_perf_file_download_streams_csv_when_idle);
    RUN_TEST(test_perf_file_delete_returns_503_when_sd_trylock_is_busy);
    RUN_TEST(test_perf_file_delete_returns_503_while_perf_logging_active);
    RUN_TEST(test_perf_file_delete_allows_inactive_file_while_perf_logging_active);
    RUN_TEST(test_perf_file_delete_removes_file_when_lock_is_free);
    return UNITY_END();
}

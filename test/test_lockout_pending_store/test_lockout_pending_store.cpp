#include <unity.h>

#include <filesystem>
#include <fstream>

#include "../mocks/Arduino.h"
#include "../mocks/storage_manager.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/storage_json_rollback.cpp"
#include "../../src/modules/lockout/lockout_pending_store.cpp"

namespace {

std::filesystem::path makeFsRoot(const char* testName) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("v1g2_pending_store_" + std::string(testName));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

std::filesystem::path resolveFsPath(const std::filesystem::path& root, const char* logicalPath) {
    std::filesystem::path relative = logicalPath ? std::filesystem::path(logicalPath) : std::filesystem::path();
    if (relative.is_absolute()) {
        relative = relative.relative_path();
    }
    return root / relative;
}

void writeText(const std::filesystem::path& path, const char* text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    TEST_ASSERT_TRUE(stream.is_open());
    stream << (text ? text : "");
    TEST_ASSERT_TRUE(stream.good());
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_pending_store_prefers_valid_live_over_prev() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);
    writeText(resolveFsPath(root, lockout_pending_store::kPendingLearnerPath),
              "{\"candidates\":[{\"freq\":24148}],\"live\":true}");
    writeText(resolveFsPath(root,
                            StorageManager::rollbackPathFor(
                                lockout_pending_store::kPendingLearnerPath).c_str()),
              "{\"candidates\":[{\"freq\":10525}],\"live\":false}");

    JsonDocument doc;
    String loadedPath;
    const JsonRollbackLoadResult result =
        lockout_pending_store::loadPendingLearnerJsonDocument(fs, doc, nullptr, &loadedPath);

    TEST_ASSERT_EQUAL(static_cast<int>(JsonRollbackLoadResult::LoadedLive), static_cast<int>(result));
    TEST_ASSERT_EQUAL_STRING(lockout_pending_store::kPendingLearnerPath, loadedPath.c_str());
    TEST_ASSERT_TRUE(doc["live"].as<bool>());
    TEST_ASSERT_EQUAL(24148, doc["candidates"][0]["freq"].as<int>());
}

void test_pending_store_falls_back_to_prev_when_live_missing() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);
    const String prevPath = StorageManager::rollbackPathFor(lockout_pending_store::kPendingLearnerPath);
    writeText(resolveFsPath(root, prevPath.c_str()), "{\"candidates\":[{\"freq\":10525}]}");

    JsonDocument doc;
    String loadedPath;
    const JsonRollbackLoadResult result =
        lockout_pending_store::loadPendingLearnerJsonDocument(fs, doc, nullptr, &loadedPath);

    TEST_ASSERT_EQUAL(static_cast<int>(JsonRollbackLoadResult::LoadedRollback), static_cast<int>(result));
    TEST_ASSERT_EQUAL_STRING(prevPath.c_str(), loadedPath.c_str());
    TEST_ASSERT_EQUAL(10525, doc["candidates"][0]["freq"].as<int>());
}

void test_pending_store_falls_back_to_prev_when_live_is_corrupt() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);
    writeText(resolveFsPath(root, lockout_pending_store::kPendingLearnerPath), "{bad json");
    const String prevPath = StorageManager::rollbackPathFor(lockout_pending_store::kPendingLearnerPath);
    writeText(resolveFsPath(root, prevPath.c_str()), "{\"candidates\":[{\"freq\":10525}]}");

    JsonDocument doc;
    String loadedPath;
    const JsonRollbackLoadResult result =
        lockout_pending_store::loadPendingLearnerJsonDocument(fs, doc, nullptr, &loadedPath);

    TEST_ASSERT_EQUAL(static_cast<int>(JsonRollbackLoadResult::LoadedRollback), static_cast<int>(result));
    TEST_ASSERT_EQUAL_STRING(prevPath.c_str(), loadedPath.c_str());
    TEST_ASSERT_EQUAL(10525, doc["candidates"][0]["freq"].as<int>());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_pending_store_prefers_valid_live_over_prev);
    RUN_TEST(test_pending_store_falls_back_to_prev_when_live_missing);
    RUN_TEST(test_pending_store_falls_back_to_prev_when_live_is_corrupt);
    return UNITY_END();
}

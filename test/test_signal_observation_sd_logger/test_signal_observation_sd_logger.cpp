#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../mocks/Arduino.h"
#include "../mocks/mock_heap_caps_state.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/perf_metrics.h"
PerfCounters perfCounters;

#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/storage_manager.cpp"
#include "../../src/modules/lockout/signal_observation_sd_logger.cpp"

namespace {

constexpr size_t kRotateThresholdBytes = 2u * 1024u * 1024u;

std::filesystem::path makeFsRoot(const char* testName) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("v1g2_signal_observation_sd_logger_" + std::string(testName));
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

void createSizedFile(const std::filesystem::path& path, size_t size) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    TEST_ASSERT_TRUE(stream.is_open());
    stream.seekp(static_cast<std::streamoff>(size - 1));
    const char marker = '\n';
    stream.write(&marker, 1);
    TEST_ASSERT_TRUE(stream.good());
}

}  // namespace

void setUp() {
    mock_reset_heap_caps();
    fs::mock_reset_fs_rename_state();
    perfCounters.reset();
}

void tearDown() {}

void test_rotate_if_needed_keeps_live_file_when_rename_fails() {
    SignalObservationSdLogger logger;
    logger.setBootId(77);

    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS testFs(root);
    const char* livePath = logger.csvPath();
    const String prevPath = String(livePath) + ".prev";
    createSizedFile(resolveFsPath(root, livePath), kRotateThresholdBytes);

    fs::mock_fail_next_rename();

    TEST_ASSERT_TRUE(logger.rotateIfNeededForTest(testFs));
    TEST_ASSERT_TRUE(testFs.exists(livePath));
    TEST_ASSERT_FALSE(testFs.exists(prevPath));
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().rotations);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_rotate_if_needed_keeps_live_file_when_rename_fails);
    return UNITY_END();
}

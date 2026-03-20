#include <unity.h>

#include <fstream>
#include <string>

namespace {

std::string readTextFile(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_metrics_handler_keeps_runtime_snapshot_off_stack() {
    const std::string source = readTextFile("src/modules/debug/debug_api_service.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(),
                              "failed to read src/modules/debug/debug_api_service.cpp");
    TEST_ASSERT_EQUAL(std::string::npos,
                      source.find("PerfRuntimeMetricsSnapshot snapshot{};"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("class MetricsSnapshotScratch"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("MetricsSnapshotScratch snapshotScratch;"));
}

void test_full_metrics_builder_is_marked_noinline() {
    const std::string source = readTextFile("src/modules/debug/debug_api_service.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(),
                              "failed to read src/modules/debug/debug_api_service.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("DEBUG_API_NOINLINE void appendFullMetricsDoc("));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_metrics_handler_keeps_runtime_snapshot_off_stack);
    RUN_TEST(test_full_metrics_builder_is_marked_noinline);
    return UNITY_END();
}

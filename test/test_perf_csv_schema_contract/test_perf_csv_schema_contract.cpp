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

void test_perf_csv_schema_version_matches_alpr_only_header() {
    const std::string source = readTextFile("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("static constexpr uint32_t PERF_CSV_SCHEMA_VERSION = 12;"));
}

void test_perf_csv_header_drops_camera_voice_columns() {
    const std::string source = readTextFile("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, source.find("cameraVoiceQueued"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("cameraVoiceStarted"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("obdStaleCount\\n\";")); 
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_perf_csv_schema_version_matches_alpr_only_header);
    RUN_TEST(test_perf_csv_header_drops_camera_voice_columns);
    return UNITY_END();
}

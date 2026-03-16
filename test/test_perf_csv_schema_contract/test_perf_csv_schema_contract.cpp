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

void test_perf_csv_schema_version_matches_current_header() {
    const std::string source = readTextFile("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("static constexpr uint32_t PERF_CSV_SCHEMA_VERSION = 16;"));
}

void test_perf_csv_header_drops_camera_voice_columns() {
    const std::string source = readTextFile("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, source.find("cameraVoiceQueued"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("cameraVoiceStarted"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("displayLiveFallbackToUsable,obdMax_us,obdPollErrors,obdStaleCount,obdVinDetected,obdVehicleFamily,obdEotValid,obdEotC_x10,obdEotAgeMs,obdEotProfileId,obdEotProbeFailures,perfDrop,eventBusDrops"));
}

void test_perf_csv_header_appends_drive_gate_columns() {
    const std::string source = readTextFile("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find(
            "perfDrop,eventBusDrops,freeDmaMin,largestDmaMin,bleState,subscribeStep,connectInProgress,asyncConnectPending,pendingDisconnectCleanup,proxyAdvertising,proxyAdvertisingLastTransitionReason,wifiPriorityMode\\n\";"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_perf_csv_schema_version_matches_current_header);
    RUN_TEST(test_perf_csv_header_drops_camera_voice_columns);
    RUN_TEST(test_perf_csv_header_appends_drive_gate_columns);
    return UNITY_END();
}

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
    source.find("static constexpr uint32_t PERF_CSV_SCHEMA_VERSION = 27;"));
}

void test_perf_csv_header_drops_camera_voice_columns() {
    const std::string source = readTextFile("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, source.find("cameraVoiceQueued"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("cameraVoiceStarted"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdVinDetected"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdVehicleFamily"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdEotValid"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdEotC_x10"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdEotAgeMs"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdEotProfileId"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdEotProbeFailures"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("displayLiveFallbackToUsable,obdMax_us,obdConnectCallMax_us,obdSecurityStartCallMax_us,obdDiscoveryCallMax_us,obdSubscribeCallMax_us,obdWriteCallMax_us,obdRssiCallMax_us,obdPollErrors,obdStaleCount,perfDrop,eventBusDrops"));
}

void test_perf_csv_header_appends_drive_gate_columns() {
    const std::string source = readTextFile("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find(
            "perfDrop,eventBusDrops,wifiHandleClientMax_us,wifiMaintenanceMax_us,wifiStatusCheckMax_us,wifiTimeoutCheckMax_us,wifiHeapGuardMax_us,wifiApStaPollMax_us,wifiStopHttpServerMax_us,wifiStopStaDisconnectMax_us,wifiStopApDisableMax_us,wifiStopModeOffMax_us,wifiStartPreflightMax_us,wifiStartApBringupMax_us,freeDmaMin,largestDmaMin,bleState,subscribeStep,connectInProgress,asyncConnectPending,pendingDisconnectCleanup,proxyAdvertising,proxyAdvertisingLastTransitionReason,wifiPriorityMode,speedSourceSelected,speedSourceValid,speedSelectedMph_x10,speedSelectedAgeMs,speedSourceSwitches,speedNoSourceSelections"));
}

void test_perf_csv_header_includes_connect_burst_attribution_columns() {
    const std::string source = readTextFile("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find(
            "bleFirstRxMs,bleFollowupRequestAlertMax_us,bleFollowupRequestVersionMax_us,bleConnectStableCallbackMax_us,bleProxyStartMax_us,displayVoiceMax_us,displayGapRecoverMax_us,displayFullRenderCount"));
}

void test_perf_csv_header_includes_display_attribution_columns() {
    const std::string source = readTextFile("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find(
            "displayFullFlushCount,displayPartialFlushCount,displayFlushBatchCount,displayPartialFlushAreaPeakPx,displayPartialFlushAreaTotalPx,displayFlushEquivalentAreaTotalPx,displayFlushMaxAreaPx,displayBaseFrameMax_us,displayStatusStripMax_us,displayFrequencyMax_us,displayBandsBarsMax_us,displayArrowsIconsMax_us,displayCardsMax_us,displayFlushSubphaseMax_us,displayLiveRenderMax_us,displayRestingRenderMax_us,displayPersistedRenderMax_us,displayPreviewRenderMax_us,displayRestoreRenderMax_us,displayPreviewFirstRenderMax_us,displayPreviewSteadyRenderMax_us,alertPersistStarts"));
}

void test_perf_metrics_exports_render_and_connect_burst_sources() {
    const std::string source = readTextFile("src/perf_metrics.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_metrics.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.dispMaxUs = perfExtended.displayRenderMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.bleProxyStartMaxUs = perfExtended.bleProxyStartMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.displayVoiceMaxUs = perfExtended.displayVoiceMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.displayGapRecoverMaxUs = perfExtended.displayGapRecoverMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.displayPartialFlushAreaPeakPx = perfExtended.displayPartialFlushAreaPeakPx;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.displayFlushSubphaseMaxUs = perfExtended.displayFlushSubphaseMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.displayPreviewFirstRenderMaxUs = perfExtended.displayPreviewFirstRenderMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.speedSourceSelected = static_cast<uint8_t>(ctx.speedStatus.selectedSource);"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.speedSelectedMph_x10 ="));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.speedSourceSwitches = ctx.speedStatus.sourceSwitches;"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_perf_csv_schema_version_matches_current_header);
    RUN_TEST(test_perf_csv_header_drops_camera_voice_columns);
    RUN_TEST(test_perf_csv_header_appends_drive_gate_columns);
    RUN_TEST(test_perf_csv_header_includes_connect_burst_attribution_columns);
    RUN_TEST(test_perf_csv_header_includes_display_attribution_columns);
    RUN_TEST(test_perf_metrics_exports_render_and_connect_burst_sources);
    return UNITY_END();
}

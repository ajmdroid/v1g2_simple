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

void test_removed_camera_label_helper_is_no_longer_declared_or_defined() {
    const std::string displayHeader = readTextFile("src/display.h");
    const std::string displayFrequency = readTextFile("src/display_frequency.cpp");

    TEST_ASSERT_FALSE_MESSAGE(displayHeader.empty(), "failed to read src/display.h");
    TEST_ASSERT_FALSE_MESSAGE(displayFrequency.empty(), "failed to read src/display_frequency.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("drawCameraLabel"));
    TEST_ASSERT_EQUAL(std::string::npos, displayFrequency.find("drawCameraLabel"));
}

void test_wifi_toggle_setup_mode_stays_a_compatibility_wrapper() {
    const std::string header = readTextFile("src/wifi_manager.h");
    const std::string source = readTextFile("src/wifi_manager_lifecycle.cpp");

    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "failed to read src/wifi_manager.h");
    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/wifi_manager_lifecycle.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          header.find("Compatibility-retained wrapper for older callers."));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("return stopSetupMode(manual, manual ? \"manual\" : \"toggle\");"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("return startSetupMode(false);"));
}

void test_alert_persistence_update_stays_a_safe_no_op_hook() {
    const std::string header = readTextFile("src/modules/alert_persistence/alert_persistence_module.h");
    const std::string source = readTextFile("src/modules/alert_persistence/alert_persistence_module.cpp");

    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "failed to read alert_persistence_module.h");
    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read alert_persistence_module.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, header.find("Main update - call from loop()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          header.find("Compatibility-retained no-op hook."));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("if (!initialized) return;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("Compatibility-retained no-op: production no longer needs loop work here."));
    TEST_ASSERT_EQUAL(std::string::npos,
                      source.find("Future: could handle periodic tasks here"));
}

void test_perf_display_screen_keeps_camera_reserved_and_unemitted() {
    const std::string perfHeader = readTextFile("src/perf_metrics.h");
    const std::string displayScreens = readTextFile("src/display_screens.cpp");
    const std::string displayUpdate = readTextFile("src/display_update.cpp");

    TEST_ASSERT_FALSE_MESSAGE(perfHeader.empty(), "failed to read src/perf_metrics.h");
    TEST_ASSERT_FALSE_MESSAGE(displayScreens.empty(), "failed to read src/display_screens.cpp");
    TEST_ASSERT_FALSE_MESSAGE(displayUpdate.empty(), "failed to read src/display_update.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Camera = 6"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          perfHeader.find("Current producers emit only Unknown,"));
    TEST_ASSERT_EQUAL(std::string::npos, displayScreens.find("PerfDisplayScreen::Camera"));
    TEST_ASSERT_EQUAL(std::string::npos, displayUpdate.find("PerfDisplayScreen::Camera"));
    TEST_ASSERT_EQUAL(std::string::npos, displayScreens.find("PerfDisplayScreen::Disconnected"));
    TEST_ASSERT_EQUAL(std::string::npos, displayUpdate.find("PerfDisplayScreen::Disconnected"));
}

void test_manual_and_audio_comments_mark_bogey_breakdown_as_legacy_only() {
    const std::string manual = readTextFile("docs/MANUAL.md");
    const std::string audioHeader = readTextFile("src/audio_beep.h");
    const std::string audioVoice = readTextFile("src/audio_voice.cpp");

    TEST_ASSERT_FALSE_MESSAGE(manual.empty(), "failed to read docs/MANUAL.md");
    TEST_ASSERT_FALSE_MESSAGE(audioHeader.empty(), "failed to read src/audio_beep.h");
    TEST_ASSERT_FALSE_MESSAGE(audioVoice.empty(), "failed to read src/audio_voice.cpp");
    TEST_ASSERT_EQUAL(std::string::npos,
                      manual.find("void play_bogey_breakdown(uint8_t total, uint8_t ahead, uint8_t behind, uint8_t side);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          manual.find("Compatibility note: play_bogey_breakdown(...) is retained for legacy/tests but is not used by the current production voice flow."));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          audioHeader.find("Compatibility-retained legacy helper for older/tests callers."));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          audioVoice.find("Compatibility-retained legacy helper: older/tests callers may still use this,"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_removed_camera_label_helper_is_no_longer_declared_or_defined);
    RUN_TEST(test_wifi_toggle_setup_mode_stays_a_compatibility_wrapper);
    RUN_TEST(test_alert_persistence_update_stays_a_safe_no_op_hook);
    RUN_TEST(test_perf_display_screen_keeps_camera_reserved_and_unemitted);
    RUN_TEST(test_manual_and_audio_comments_mark_bogey_breakdown_as_legacy_only);
    return UNITY_END();
}

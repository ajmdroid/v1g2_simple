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

void test_perf_display_screen_uses_explicit_mapping_and_keeps_retired_values_unemitted() {
    const std::string perfHeader = readTextFile("src/perf_metrics.h");
    const std::string displayHeader = readTextFile("src/display.h");
    const std::string displayCore = readTextFile("src/display.cpp");
    const std::string displayScreens = readTextFile("src/display_screens.cpp");
    const std::string displayUpdate = readTextFile("src/display_update.cpp");

    TEST_ASSERT_FALSE_MESSAGE(perfHeader.empty(), "failed to read src/perf_metrics.h");
    TEST_ASSERT_FALSE_MESSAGE(displayHeader.empty(), "failed to read src/display.h");
    TEST_ASSERT_FALSE_MESSAGE(displayCore.empty(), "failed to read src/display.cpp");
    TEST_ASSERT_FALSE_MESSAGE(displayScreens.empty(), "failed to read src/display_screens.cpp");
    TEST_ASSERT_FALSE_MESSAGE(displayUpdate.empty(), "failed to read src/display_update.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Camera = 6"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          perfHeader.find("Current producers emit only Unknown,"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          displayHeader.find("static PerfDisplayScreen perfScreenForMode(ScreenMode mode);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          displayCore.find("PerfDisplayScreen V1Display::perfScreenForMode(ScreenMode mode)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          displayCore.find("case ScreenMode::Disconnected:"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          displayCore.find("return PerfDisplayScreen::Unknown;"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      displayScreens.find("static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen))"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      displayUpdate.find("static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen))"));
    TEST_ASSERT_EQUAL(std::string::npos, displayScreens.find("PerfDisplayScreen::Camera"));
    TEST_ASSERT_EQUAL(std::string::npos, displayUpdate.find("PerfDisplayScreen::Camera"));
    TEST_ASSERT_EQUAL(std::string::npos, displayScreens.find("PerfDisplayScreen::Disconnected"));
    TEST_ASSERT_EQUAL(std::string::npos, displayUpdate.find("PerfDisplayScreen::Disconnected"));
}

void test_bogey_breakdown_has_been_fully_retired() {
    const std::string manual = readTextFile("docs/MANUAL.md");
    const std::string audioHeader = readTextFile("src/audio_beep.h");
    const std::string audioVoice = readTextFile("src/audio_voice.cpp");

    TEST_ASSERT_FALSE_MESSAGE(manual.empty(), "failed to read docs/MANUAL.md");
    TEST_ASSERT_FALSE_MESSAGE(audioHeader.empty(), "failed to read src/audio_beep.h");
    TEST_ASSERT_FALSE_MESSAGE(audioVoice.empty(), "failed to read src/audio_voice.cpp");
    TEST_ASSERT_EQUAL(std::string::npos,
                      manual.find("play_bogey_breakdown"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      audioHeader.find("play_bogey_breakdown"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      audioVoice.find("play_bogey_breakdown"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_removed_camera_label_helper_is_no_longer_declared_or_defined);
    RUN_TEST(test_wifi_toggle_setup_mode_stays_a_compatibility_wrapper);
    RUN_TEST(test_alert_persistence_update_stays_a_safe_no_op_hook);
    RUN_TEST(test_perf_display_screen_uses_explicit_mapping_and_keeps_retired_values_unemitted);
    RUN_TEST(test_bogey_breakdown_has_been_fully_retired);
    return UNITY_END();
}

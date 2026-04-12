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

void test_legacy_display_dirty_region_tracking_is_fully_removed() {
    const std::string displayHeader = readTextFile("src/display.h");
    const std::string displayFrequency = readTextFile("src/display_frequency.cpp");
    const std::string displayCards = readTextFile("src/display_cards.cpp");

    TEST_ASSERT_FALSE_MESSAGE(displayHeader.empty(), "failed to read src/display.h");
    TEST_ASSERT_FALSE_MESSAGE(displayFrequency.empty(), "failed to read src/display_frequency.cpp");
    TEST_ASSERT_FALSE_MESSAGE(displayCards.empty(), "failed to read src/display_cards.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("markFrequencyDirtyRegion"));
    TEST_ASSERT_EQUAL(std::string::npos, displayFrequency.find("markFrequencyDirtyRegion"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyRenderDirty_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyDirtyValid_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyDirtyX_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyDirtyY_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyDirtyW_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("frequencyDirtyH_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayHeader.find("secondaryCardsRenderDirty_"));
    TEST_ASSERT_EQUAL(std::string::npos, displayFrequency.find("Legacy dirty region tracking"));
    TEST_ASSERT_EQUAL(std::string::npos, displayCards.find("secondaryCardsRenderDirty_"));
}

void test_wifi_toggle_setup_mode_is_fully_removed() {
    const std::string header = readTextFile("src/wifi_manager.h");
    const std::string source = readTextFile("src/wifi_manager_lifecycle.cpp");

    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "failed to read src/wifi_manager.h");
    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/wifi_manager_lifecycle.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, header.find("toggleSetupMode"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("toggleSetupMode"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("bool startSetupMode("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("bool stopSetupMode("));
}

void test_alert_persistence_update_is_fully_removed() {
    const std::string header = readTextFile("src/modules/alert_persistence/alert_persistence_module.h");
    const std::string source = readTextFile("src/modules/alert_persistence/alert_persistence_module.cpp");

    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "failed to read alert_persistence_module.h");
    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read alert_persistence_module.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, header.find("void update("));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("AlertPersistenceModule::update("));
    TEST_ASSERT_EQUAL(std::string::npos, header.find("Compatibility-retained no-op hook."));
    TEST_ASSERT_EQUAL(std::string::npos,
                      source.find("Compatibility-retained no-op: production no longer needs loop work here."));
    TEST_ASSERT_EQUAL(std::string::npos, header.find("initialized = false"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("initialized = true;"));
}

void test_perf_display_screen_uses_explicit_mapping_and_removes_retired_values() {
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
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Unknown = 0"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Resting = 1"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Scanning = 2"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Live = 4"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, perfHeader.find("Persisted = 5"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          perfHeader.find("Current producers emit only Unknown,"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          perfHeader.find("retired Disconnected"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          perfHeader.find("retired Camera"));
    TEST_ASSERT_EQUAL(std::string::npos, perfHeader.find("Disconnected = 3"));
    TEST_ASSERT_EQUAL(std::string::npos, perfHeader.find("Camera = 6"));
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
    RUN_TEST(test_legacy_display_dirty_region_tracking_is_fully_removed);
    RUN_TEST(test_wifi_toggle_setup_mode_is_fully_removed);
    RUN_TEST(test_alert_persistence_update_is_fully_removed);
    RUN_TEST(test_perf_display_screen_uses_explicit_mapping_and_removes_retired_values);
    RUN_TEST(test_bogey_breakdown_has_been_fully_retired);
    return UNITY_END();
}

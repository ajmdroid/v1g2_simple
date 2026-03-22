#include <unity.h>

#include <fstream>
#include <string>

namespace {

std::string readFile(const char* path) {
    std::ifstream input(path);
    TEST_ASSERT_TRUE_MESSAGE(input.good(), "failed to open display_update.cpp");
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

size_t findMatchingBrace(const std::string& text, size_t openPos) {
    TEST_ASSERT_NOT_EQUAL(std::string::npos, openPos);
    int depth = 0;
    for (size_t i = openPos; i < text.size(); ++i) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            depth--;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string::npos;
}

std::string extractBlock(const std::string& text, const std::string& marker) {
    const size_t markerPos = text.find(marker);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, markerPos);
    const size_t openBrace = text.find('{', markerPos);
    const size_t closeBrace = findMatchingBrace(text, openBrace);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, closeBrace);
    return text.substr(openBrace, closeBrace - openBrace + 1);
}

void assertNoFunctionLocalStatic(const std::string& block, const char* label) {
    const std::string token = "\n    static ";
    const size_t staticPos = block.find(token);
    TEST_ASSERT_EQUAL_MESSAGE(std::string::npos, staticPos, label);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_scanning_early_return_does_not_clear_tracking_reset() {
    const std::string source = readFile("/Users/ajmedford/v1g2_simple/src/display_update.cpp");
    const std::string restingUpdate = extractBlock(source, "void V1Display::update(const DisplayState& state)");
    const std::string scanningBlock = extractBlock(restingUpdate, "if (currentScreen == ScreenMode::Scanning)");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, scanningBlock.find("return;"));
    TEST_ASSERT_EQUAL(std::string::npos, scanningBlock.find("dirty.resetTracking = false;"));
}

void test_resting_full_redraw_clears_tracking_reset_after_flush() {
    const std::string source = readFile("/Users/ajmedford/v1g2_simple/src/display_update.cpp");
    const std::string restingUpdate = extractBlock(source, "void V1Display::update(const DisplayState& state)");

    const size_t flushPos = restingUpdate.find("DISPLAY_FLUSH();");
    const size_t clearPos = restingUpdate.find("dirty.resetTracking = false;");
    const size_t screenPos = restingUpdate.find("currentScreen = ScreenMode::Resting;");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, flushPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, clearPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, screenPos);
    TEST_ASSERT_TRUE_MESSAGE(clearPos > flushPos, "resting path should clear resetTracking after flush");
    TEST_ASSERT_TRUE_MESSAGE(clearPos < screenPos, "resting path should clear resetTracking before final state commit");
}

void test_display_update_paths_no_longer_hide_persistent_state_in_function_locals() {
    const std::string source = readFile("/Users/ajmedford/v1g2_simple/src/display_update.cpp");

    assertNoFunctionLocalStatic(
        extractBlock(source, "void V1Display::drawStatusStrip"),
        "drawStatusStrip should use explicit render cache instead of function-local static state");
    assertNoFunctionLocalStatic(
        extractBlock(source, "void V1Display::update(const DisplayState& state)"),
        "resting update should use explicit render cache instead of function-local static state");
    assertNoFunctionLocalStatic(
        extractBlock(source, "void V1Display::refreshFrequencyOnly"),
        "refreshFrequencyOnly should use explicit render cache instead of function-local static state");
    assertNoFunctionLocalStatic(
        extractBlock(source, "void V1Display::update(const AlertData& priority"),
        "live update should use explicit render cache instead of function-local static state");
}

void test_stale_ble_policy_is_wired_into_display_sources() {
    const std::string updateSource = readFile("/Users/ajmedford/v1g2_simple/src/display_update.cpp");
    const std::string statusSource = readFile("/Users/ajmedford/v1g2_simple/src/display_status_bar.cpp");

    const std::string restingUpdate = extractBlock(updateSource, "void V1Display::update(const DisplayState& state)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, restingUpdate.find("const bool bleContextFresh = hasFreshBleContext(now);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, restingUpdate.find("volZeroWarn.reset();"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, restingUpdate.find("volZeroWarn.evaluate("));

    const std::string rssiBlock = extractBlock(statusSource, "void V1Display::drawRssiIndicator(int rssi)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, rssiBlock.find("if (!hasFreshBleContext(millis()))"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, rssiBlock.find("FILL_RECT(x, y, clearW, clearH, PALETTE_BG);"));

    const std::string bleBlock = extractBlock(statusSource, "void V1Display::drawBLEProxyIndicator()");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, bleBlock.find("const bool bleContextFresh = hasFreshBleContext(millis());"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, bleBlock.find("const bool receivingData = bleReceivingData && bleContextFresh;"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_scanning_early_return_does_not_clear_tracking_reset);
    RUN_TEST(test_resting_full_redraw_clears_tracking_reset_after_flush);
    RUN_TEST(test_display_update_paths_no_longer_hide_persistent_state_in_function_locals);
    RUN_TEST(test_stale_ble_policy_is_wired_into_display_sources);
    return UNITY_END();
}

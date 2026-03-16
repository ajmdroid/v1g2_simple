#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_audio_api_service.h"
#include "../../src/modules/wifi/wifi_audio_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeRuntime {
    V1Settings settings;
    int setAudioVolumeCalls = 0;
    uint8_t lastAudioVolume = 0;
    int saveDeferredBackupCalls = 0;
};

static WifiAudioApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiAudioApiService::Runtime{
        [&rt]() -> const V1Settings& {
            return rt.settings;
        },
        [&rt]() -> V1Settings& {
            return rt.settings;
        },
        [&rt](uint8_t volume) {
            rt.setAudioVolumeCalls++;
            rt.lastAudioVolume = volume;
        },
        [&rt]() {
            rt.saveDeferredBackupCalls++;
        },
    };
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_get_returns_500_when_runtime_missing() {
    WebServer server(80);
    WifiAudioApiService::Runtime runtime{};

    WifiAudioApiService::handleApiGet(server, runtime);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Settings unavailable\""));
}

void test_get_serializes_audio_payload() {
    WebServer server(80);
    FakeRuntime rt;
    rt.settings.voiceAlertMode = VOICE_MODE_FREQ_ONLY;
    rt.settings.voiceDirectionEnabled = false;
    rt.settings.voiceVolume = 61;
    rt.settings.alertVolumeFadeEnabled = true;
    rt.settings.alertVolumeFadeDelaySec = 4;

    WifiAudioApiService::handleApiGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"voiceAlertMode\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"voiceDirectionEnabled\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"voiceVolume\":61"));
    TEST_ASSERT_TRUE(responseContains(server, "\"alertVolumeFadeEnabled\":true"));
}

void test_save_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("voiceVolume", "55");

    WifiAudioApiService::handleApiSave(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.setAudioVolumeCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.saveDeferredBackupCalls);
}

void test_save_updates_audio_settings_and_calls_side_effects() {
    WebServer server(80);
    FakeRuntime rt;

    server.setArg("voiceAlertMode", "1");
    server.setArg("voiceDirectionEnabled", "false");
    server.setArg("announceBogeyCount", "false");
    server.setArg("muteVoiceIfVolZero", "true");
    server.setArg("voiceVolume", "71");
    server.setArg("announceSecondaryAlerts", "true");
    server.setArg("secondaryLaser", "false");
    server.setArg("secondaryKa", "false");
    server.setArg("secondaryK", "true");
    server.setArg("secondaryX", "true");
    server.setArg("alertVolumeFadeEnabled", "true");
    server.setArg("alertVolumeFadeDelaySec", "8");
    server.setArg("alertVolumeFadeVolume", "3");

    WifiAudioApiService::handleApiSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(VOICE_MODE_BAND_ONLY, static_cast<int>(rt.settings.voiceAlertMode));
    TEST_ASSERT_FALSE(rt.settings.voiceDirectionEnabled);
    TEST_ASSERT_FALSE(rt.settings.announceBogeyCount);
    TEST_ASSERT_TRUE(rt.settings.muteVoiceIfVolZero);
    TEST_ASSERT_EQUAL_UINT8(71, rt.settings.voiceVolume);
    TEST_ASSERT_TRUE(rt.settings.announceSecondaryAlerts);
    TEST_ASSERT_FALSE(rt.settings.secondaryLaser);
    TEST_ASSERT_FALSE(rt.settings.secondaryKa);
    TEST_ASSERT_TRUE(rt.settings.secondaryK);
    TEST_ASSERT_TRUE(rt.settings.secondaryX);
    TEST_ASSERT_TRUE(rt.settings.alertVolumeFadeEnabled);
    TEST_ASSERT_EQUAL_UINT8(8, rt.settings.alertVolumeFadeDelaySec);
    TEST_ASSERT_EQUAL_UINT8(3, rt.settings.alertVolumeFadeVolume);
    TEST_ASSERT_EQUAL_INT(1, rt.setAudioVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(71, rt.lastAudioVolume);
    TEST_ASSERT_EQUAL_INT(1, rt.saveDeferredBackupCalls);
}

void test_save_clamps_numeric_ranges() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("voiceAlertMode", "99");
    server.setArg("voiceVolume", "101");
    server.setArg("alertVolumeFadeDelaySec", "0");
    server.setArg("alertVolumeFadeVolume", "99");

    WifiAudioApiService::handleApiSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(rt.settings.voiceAlertMode));
    TEST_ASSERT_EQUAL_UINT8(100, rt.settings.voiceVolume);
    TEST_ASSERT_EQUAL_UINT8(1, rt.settings.alertVolumeFadeDelaySec);
    TEST_ASSERT_EQUAL_UINT8(9, rt.settings.alertVolumeFadeVolume);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_get_returns_500_when_runtime_missing);
    RUN_TEST(test_get_serializes_audio_payload);
    RUN_TEST(test_save_rate_limited_short_circuits);
    RUN_TEST(test_save_updates_audio_settings_and_calls_side_effects);
    RUN_TEST(test_save_clamps_numeric_ranges);
    return UNITY_END();
}

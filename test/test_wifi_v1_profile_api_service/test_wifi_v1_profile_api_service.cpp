#include <unity.h>
#include <cstring>
#include <vector>

#include "../../src/modules/wifi/wifi_v1_profile_api_service.h"
#include "../../src/modules/wifi/wifi_v1_profile_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct StoredProfile {
    String name;
    String description;
    bool displayOn = true;
    String profileJson;
};

struct FakeRuntime {
    std::vector<String> listNames;
    std::vector<StoredProfile> storedProfiles;

    bool parseSettingsOk = true;
    bool saveOk = true;
    String saveError = "";
    bool deleteResult = true;

    bool hasCurrent = false;
    String currentSettingsJson = "{}";
    bool connected = false;

    int listCalls = 0;
    int loadSummaryCalls = 0;
    int loadJsonCalls = 0;
    int parseSettingsCalls = 0;
    int saveCalls = 0;
    int deleteCalls = 0;
    int backupCalls = 0;

    String lastSaveName;
    String lastSaveDescription;
    bool lastSaveDisplayOn = true;
    uint8_t lastSaveBytes[6] = {0};
};

static bool fakeLoadSummary(FakeRuntime& rt, const String& name, WifiV1ProfileApiService::ProfileSummary& out) {
    rt.loadSummaryCalls++;
    for (const auto& p : rt.storedProfiles) {
        if (p.name == name) {
            out.name = p.name;
            out.description = p.description;
            out.displayOn = p.displayOn;
            return true;
        }
    }
    return false;
}

static bool fakeLoadJson(FakeRuntime& rt, const String& name, String& outJson) {
    rt.loadJsonCalls++;
    for (const auto& p : rt.storedProfiles) {
        if (p.name == name) {
            outJson = p.profileJson;
            return true;
        }
    }
    return false;
}

static WifiV1ProfileApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1ProfileApiService::Runtime{
        [&rt]() {
            rt.listCalls++;
            return rt.listNames;
        },
        [&rt](const String& name, WifiV1ProfileApiService::ProfileSummary& summary) {
            return fakeLoadSummary(rt, name, summary);
        },
        [&rt](const String& name, String& profileJson) {
            return fakeLoadJson(rt, name, profileJson);
        },
        [](const String&, uint8_t[6], bool&) { return false; },
        [&rt](const JsonObject& settingsObj, uint8_t outBytes[6]) {
            rt.parseSettingsCalls++;
            if (!rt.parseSettingsOk) {
                return false;
            }
            memset(outBytes, 0xFF, 6);
            if (settingsObj["byte0"].is<int>()) {
                outBytes[0] = static_cast<uint8_t>(settingsObj["byte0"].as<int>());
            }
            return true;
        },
        [&rt](const String& name,
              const String& description,
              bool displayOn,
              const uint8_t inBytes[6],
              String& error) {
            rt.saveCalls++;
            rt.lastSaveName = name;
            rt.lastSaveDescription = description;
            rt.lastSaveDisplayOn = displayOn;
            memcpy(rt.lastSaveBytes, inBytes, 6);
            if (!rt.saveOk) {
                error = rt.saveError;
                return false;
            }
            return true;
        },
        [&rt](const String& /*name*/) {
            rt.deleteCalls++;
            return rt.deleteResult;
        },
        []() { return false; },
        [](const uint8_t[6]) { return false; },
        [](bool) {},
        [&rt]() { return rt.hasCurrent; },
        [&rt]() { return rt.currentSettingsJson; },
        [&rt]() { return rt.connected; },
        [&rt]() { rt.backupCalls++; },
    };
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_profiles_list_includes_loaded_profiles() {
    WebServer server(80);
    FakeRuntime rt;
    rt.listNames = {"A", "B"};
    rt.storedProfiles.push_back({"A", "desc-a", true, "{\"name\":\"A\"}"});

    WifiV1ProfileApiService::handleProfilesList(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"profiles\":[{\"name\":\"A\""));
    TEST_ASSERT_FALSE(responseContains(server, "\"name\":\"B\""));
    TEST_ASSERT_EQUAL_INT(1, rt.listCalls);
    TEST_ASSERT_EQUAL_INT(2, rt.loadSummaryCalls);
}

void test_profile_get_missing_name_returns_400() {
    WebServer server(80);
    FakeRuntime rt;

    WifiV1ProfileApiService::handleProfileGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing profile name\""));
}

void test_profile_get_not_found_returns_404() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("name", "missing");

    WifiV1ProfileApiService::handleProfileGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Profile not found\""));
}

void test_profile_get_success_returns_profile_json() {
    WebServer server(80);
    FakeRuntime rt;
    rt.storedProfiles.push_back({"Commute", "", false, "{\"name\":\"Commute\",\"displayOn\":false}"});
    server.setArg("name", "Commute");

    WifiV1ProfileApiService::handleProfileGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"Commute\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"displayOn\":false"));
}

void test_profile_save_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"name\":\"X\",\"settings\":{\"byte0\":4}}");

    WifiV1ProfileApiService::handleProfileSave(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_invalid_json_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{bad json");

    WifiV1ProfileApiService::handleProfileSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Invalid JSON\""));
}

void test_profile_save_invalid_settings_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    rt.parseSettingsOk = false;
    server.setArg("plain", "{\"name\":\"X\",\"settings\":{\"byte0\":4}}");

    WifiV1ProfileApiService::handleProfileSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Invalid settings\""));
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_success_calls_save_and_backup() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"description\":\"desc\",\"displayOn\":false,\"settings\":{\"byte0\":7}}");

    WifiV1ProfileApiService::handleProfileSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.backupCalls);
    TEST_ASSERT_EQUAL_STRING("RoadTrip", rt.lastSaveName.c_str());
    TEST_ASSERT_EQUAL_STRING("desc", rt.lastSaveDescription.c_str());
    TEST_ASSERT_FALSE(rt.lastSaveDisplayOn);
    TEST_ASSERT_EQUAL_UINT8(7, rt.lastSaveBytes[0]);
}

void test_profile_save_failure_returns_500_with_error() {
    WebServer server(80);
    FakeRuntime rt;
    rt.saveOk = false;
    rt.saveError = "disk full";
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"byte0\":3}}");

    WifiV1ProfileApiService::handleProfileSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"disk full\""));
    TEST_ASSERT_EQUAL_INT(0, rt.backupCalls);
}

void test_profile_delete_success_calls_backup() {
    WebServer server(80);
    FakeRuntime rt;
    rt.deleteResult = true;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleProfileDelete(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.deleteCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.backupCalls);
}

void test_profile_delete_not_found_returns_404() {
    WebServer server(80);
    FakeRuntime rt;
    rt.deleteResult = false;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleProfileDelete(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Profile not found\""));
    TEST_ASSERT_EQUAL_INT(0, rt.backupCalls);
}

void test_current_settings_unavailable() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.hasCurrent = false;

    WifiV1ProfileApiService::handleCurrentSettings(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"connected\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"available\":false"));
}

void test_current_settings_available_embeds_settings_json() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = false;
    rt.hasCurrent = true;
    rt.currentSettingsJson = "{\"xBand\":true,\"bytes\":[1,2,3,4,5,6]}";

    WifiV1ProfileApiService::handleCurrentSettings(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"connected\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"available\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"settings\":{\"xBand\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"bytes\":[1,2,3,4,5,6]"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_profiles_list_includes_loaded_profiles);
    RUN_TEST(test_profile_get_missing_name_returns_400);
    RUN_TEST(test_profile_get_not_found_returns_404);
    RUN_TEST(test_profile_get_success_returns_profile_json);
    RUN_TEST(test_profile_save_rate_limited_short_circuits);
    RUN_TEST(test_profile_save_invalid_json_returns_400);
    RUN_TEST(test_profile_save_invalid_settings_returns_400);
    RUN_TEST(test_profile_save_success_calls_save_and_backup);
    RUN_TEST(test_profile_save_failure_returns_500_with_error);
    RUN_TEST(test_profile_delete_success_calls_backup);
    RUN_TEST(test_profile_delete_not_found_returns_404);
    RUN_TEST(test_current_settings_unavailable);
    RUN_TEST(test_current_settings_available_embeds_settings_json);
    return UNITY_END();
}

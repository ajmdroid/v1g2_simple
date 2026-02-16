#include <unity.h>
#include <cstring>

#include "../../src/modules/wifi/wifi_autopush_api_service.h"
#include "../../src/modules/wifi/wifi_autopush_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeRuntime {
    WifiAutoPushApiService::SlotsSnapshot snapshot;
    bool statusAvailable = false;
    String statusJson = "{}";
    uint8_t slotVolumes[3] = {4, 5, 6};
    uint8_t slotMuteVolumes[3] = {1, 2, 3};
    int activeSlot = 1;
    bool autoPushEnabled = false;
    WifiAutoPushApiService::PushNowQueueResult queueResult =
        WifiAutoPushApiService::PushNowQueueResult::QUEUED;
    WifiAutoPushApiService::PushNowRequest lastPushRequest;

    int loadSlotsCalls = 0;
    int loadStatusCalls = 0;
    int setSlotNameCalls = 0;
    int setSlotColorCalls = 0;
    int setSlotVolumesCalls = 0;
    int setSlotDarkModeCalls = 0;
    int setSlotMuteToZeroCalls = 0;
    int setSlotAlertPersistCalls = 0;
    int setSlotPriorityArrowOnlyCalls = 0;
    int setSlotProfileAndModeCalls = 0;
    int drawProfileIndicatorCalls = 0;
    int setActiveSlotCalls = 0;
    int setAutoPushEnabledCalls = 0;
    int queuePushNowCalls = 0;

    int lastSlotIndex = -1;
    String lastSlotName;
    uint16_t lastSlotColor = 0;
    uint8_t lastSlotVolume = 0;
    uint8_t lastSlotMuteVolume = 0;
    bool lastDarkMode = false;
    bool lastMuteToZero = false;
    uint8_t lastAlertPersist = 0;
    bool lastPriorityArrowOnly = false;
    String lastProfile;
    int lastMode = 0;
};

static WifiAutoPushApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiAutoPushApiService::Runtime{
        [&rt](WifiAutoPushApiService::SlotsSnapshot& out) {
            rt.loadSlotsCalls++;
            out = rt.snapshot;
        },
        [&rt](String& outJson) {
            rt.loadStatusCalls++;
            if (!rt.statusAvailable) {
                return false;
            }
            outJson = rt.statusJson;
            return true;
        },
        [&rt](int slot, const String& name) {
            rt.setSlotNameCalls++;
            rt.lastSlotIndex = slot;
            rt.lastSlotName = name;
        },
        [&rt](int slot, uint16_t color) {
            rt.setSlotColorCalls++;
            rt.lastSlotIndex = slot;
            rt.lastSlotColor = color;
        },
        [&rt](int slot) {
            return rt.slotVolumes[slot];
        },
        [&rt](int slot) {
            return rt.slotMuteVolumes[slot];
        },
        [&rt](int slot, uint8_t volume, uint8_t muteVolume) {
            rt.setSlotVolumesCalls++;
            rt.lastSlotIndex = slot;
            rt.lastSlotVolume = volume;
            rt.lastSlotMuteVolume = muteVolume;
            rt.slotVolumes[slot] = volume;
            rt.slotMuteVolumes[slot] = muteVolume;
        },
        [&rt](int slot, bool darkMode) {
            rt.setSlotDarkModeCalls++;
            rt.lastSlotIndex = slot;
            rt.lastDarkMode = darkMode;
        },
        [&rt](int slot, bool muteToZero) {
            rt.setSlotMuteToZeroCalls++;
            rt.lastSlotIndex = slot;
            rt.lastMuteToZero = muteToZero;
        },
        [&rt](int slot, uint8_t alertPersist) {
            rt.setSlotAlertPersistCalls++;
            rt.lastSlotIndex = slot;
            rt.lastAlertPersist = alertPersist;
        },
        [&rt](int slot, bool priorityArrowOnly) {
            rt.setSlotPriorityArrowOnlyCalls++;
            rt.lastSlotIndex = slot;
            rt.lastPriorityArrowOnly = priorityArrowOnly;
        },
        [&rt](int slot, const String& profile, int mode) {
            rt.setSlotProfileAndModeCalls++;
            rt.lastSlotIndex = slot;
            rt.lastProfile = profile;
            rt.lastMode = mode;
        },
        [&rt]() {
            return rt.activeSlot;
        },
        [&rt](int slot) {
            rt.drawProfileIndicatorCalls++;
            rt.lastSlotIndex = slot;
        },
        [&rt](int slot) {
            rt.setActiveSlotCalls++;
            rt.activeSlot = slot;
        },
        [&rt](bool enabled) {
            rt.setAutoPushEnabledCalls++;
            rt.autoPushEnabled = enabled;
        },
        [&rt](const WifiAutoPushApiService::PushNowRequest& request) {
            rt.queuePushNowCalls++;
            rt.lastPushRequest = request;
            return rt.queueResult;
        },
    };
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_slots_serializes_expected_payload() {
    WebServer server(80);
    FakeRuntime rt;
    rt.snapshot.enabled = true;
    rt.snapshot.activeSlot = 2;

    rt.snapshot.slots[0].name = "Default";
    rt.snapshot.slots[0].profile = "City";
    rt.snapshot.slots[0].mode = 1;
    rt.snapshot.slots[0].color = 31;
    rt.snapshot.slots[0].volume = 4;
    rt.snapshot.slots[0].muteVolume = 2;
    rt.snapshot.slots[0].darkMode = false;
    rt.snapshot.slots[0].muteToZero = true;
    rt.snapshot.slots[0].alertPersist = 3;
    rt.snapshot.slots[0].priorityArrowOnly = false;

    rt.snapshot.slots[1].name = "Highway";
    rt.snapshot.slots[1].profile = "Road";
    rt.snapshot.slots[1].mode = 2;
    rt.snapshot.slots[1].color = 63488;
    rt.snapshot.slots[1].volume = 6;
    rt.snapshot.slots[1].muteVolume = 3;
    rt.snapshot.slots[1].darkMode = true;
    rt.snapshot.slots[1].muteToZero = false;
    rt.snapshot.slots[1].alertPersist = 1;
    rt.snapshot.slots[1].priorityArrowOnly = true;

    rt.snapshot.slots[2].name = "Comfort";
    rt.snapshot.slots[2].profile = "Suburb";
    rt.snapshot.slots[2].mode = 0;
    rt.snapshot.slots[2].color = 2016;
    rt.snapshot.slots[2].volume = 5;
    rt.snapshot.slots[2].muteVolume = 1;
    rt.snapshot.slots[2].darkMode = false;
    rt.snapshot.slots[2].muteToZero = false;
    rt.snapshot.slots[2].alertPersist = 0;
    rt.snapshot.slots[2].priorityArrowOnly = false;

    WifiAutoPushApiService::handleApiSlots(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"activeSlot\":2"));
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"Default\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"profile\":\"Road\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"priorityArrowOnly\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"profile\":\"Suburb\""));
    TEST_ASSERT_EQUAL_INT(1, rt.loadSlotsCalls);
}

void test_status_returns_500_when_unavailable() {
    WebServer server(80);
    FakeRuntime rt;
    rt.statusAvailable = false;

    WifiAutoPushApiService::handleApiStatus(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Push status not available\""));
    TEST_ASSERT_EQUAL_INT(1, rt.loadStatusCalls);
}

void test_status_returns_500_when_callback_missing() {
    WebServer server(80);
    WifiAutoPushApiService::Runtime runtime{};

    WifiAutoPushApiService::handleApiStatus(server, runtime);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Push status not available\""));
}

void test_status_returns_json_when_available() {
    WebServer server(80);
    FakeRuntime rt;
    rt.statusAvailable = true;
    rt.statusJson = "{\"ok\":true,\"queueDepth\":1}";

    WifiAutoPushApiService::handleApiStatus(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"queueDepth\":1"));
    TEST_ASSERT_EQUAL_INT(1, rt.loadStatusCalls);
}

void test_slot_save_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("slot", "0");
    server.setArg("profile", "Road");
    server.setArg("mode", "1");

    WifiAutoPushApiService::handleApiSlotSave(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.setSlotProfileAndModeCalls);
}

void test_slot_save_missing_params_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("slot", "0");

    WifiAutoPushApiService::handleApiSlotSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing parameters\""));
}

void test_slot_save_invalid_slot_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("slot", "3");
    server.setArg("profile", "Road");
    server.setArg("mode", "1");

    WifiAutoPushApiService::handleApiSlotSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Invalid slot\""));
}

void test_slot_save_success_updates_slot_runtime() {
    WebServer server(80);
    FakeRuntime rt;
    rt.activeSlot = 2;
    server.setArg("slot", "2");
    server.setArg("profile", "Road");
    server.setArg("mode", "4");
    server.setArg("name", "Weekend");
    server.setArg("color", "1234");
    server.setArg("volume", "8");
    server.setArg("muteVol", "1");
    server.setArg("darkMode", "true");
    server.setArg("muteToZero", "true");
    server.setArg("alertPersist", "9");
    server.setArg("priorityArrowOnly", "true");

    WifiAutoPushApiService::handleApiSlotSave(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.setSlotNameCalls);
    TEST_ASSERT_EQUAL_STRING("Weekend", rt.lastSlotName.c_str());
    TEST_ASSERT_EQUAL_INT(1, rt.setSlotColorCalls);
    TEST_ASSERT_EQUAL_UINT16(1234, rt.lastSlotColor);
    TEST_ASSERT_EQUAL_INT(1, rt.setSlotVolumesCalls);
    TEST_ASSERT_EQUAL_UINT8(8, rt.lastSlotVolume);
    TEST_ASSERT_EQUAL_UINT8(1, rt.lastSlotMuteVolume);
    TEST_ASSERT_EQUAL_INT(1, rt.setSlotDarkModeCalls);
    TEST_ASSERT_TRUE(rt.lastDarkMode);
    TEST_ASSERT_EQUAL_INT(1, rt.setSlotMuteToZeroCalls);
    TEST_ASSERT_TRUE(rt.lastMuteToZero);
    TEST_ASSERT_EQUAL_INT(1, rt.setSlotAlertPersistCalls);
    TEST_ASSERT_EQUAL_UINT8(5, rt.lastAlertPersist);  // Clamped 0-5.
    TEST_ASSERT_EQUAL_INT(1, rt.setSlotPriorityArrowOnlyCalls);
    TEST_ASSERT_TRUE(rt.lastPriorityArrowOnly);
    TEST_ASSERT_EQUAL_INT(1, rt.setSlotProfileAndModeCalls);
    TEST_ASSERT_EQUAL_STRING("Road", rt.lastProfile.c_str());
    TEST_ASSERT_EQUAL_INT(4, rt.lastMode);
    TEST_ASSERT_EQUAL_INT(1, rt.drawProfileIndicatorCalls);
}

void test_activate_missing_slot_returns_400() {
    WebServer server(80);
    FakeRuntime rt;

    WifiAutoPushApiService::handleApiActivate(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing slot parameter\""));
}

void test_activate_invalid_slot_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("slot", "-1");

    WifiAutoPushApiService::handleApiActivate(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Invalid slot\""));
}

void test_activate_success_defaults_enable_true() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("slot", "1");

    WifiAutoPushApiService::handleApiActivate(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.setActiveSlotCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.activeSlot);
    TEST_ASSERT_EQUAL_INT(1, rt.setAutoPushEnabledCalls);
    TEST_ASSERT_TRUE(rt.autoPushEnabled);
}

void test_activate_success_enable_false() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("slot", "0");
    server.setArg("enable", "false");

    WifiAutoPushApiService::handleApiActivate(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_FALSE(rt.autoPushEnabled);
}

void test_push_now_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("slot", "1");

    WifiAutoPushApiService::handleApiPushNow(
        server,
        makeRuntime(rt),
        []() { return false; });

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.queuePushNowCalls);
}

void test_push_now_missing_slot_returns_400() {
    WebServer server(80);
    FakeRuntime rt;

    WifiAutoPushApiService::handleApiPushNow(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing slot parameter\""));
}

void test_push_now_invalid_slot_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("slot", "9");

    WifiAutoPushApiService::handleApiPushNow(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Invalid slot\""));
}

void test_push_now_maps_runtime_error_states() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("slot", "0");

    rt.queueResult = WifiAutoPushApiService::PushNowQueueResult::V1_NOT_CONNECTED;
    WifiAutoPushApiService::handleApiPushNow(server, makeRuntime(rt), []() { return true; });
    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"V1 not connected\""));

    rt.queueResult = WifiAutoPushApiService::PushNowQueueResult::ALREADY_IN_PROGRESS;
    WifiAutoPushApiService::handleApiPushNow(server, makeRuntime(rt), []() { return true; });
    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Push already in progress\""));

    rt.queueResult = WifiAutoPushApiService::PushNowQueueResult::NO_PROFILE_CONFIGURED;
    WifiAutoPushApiService::handleApiPushNow(server, makeRuntime(rt), []() { return true; });
    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"No profile configured for this slot\""));

    rt.queueResult = WifiAutoPushApiService::PushNowQueueResult::PROFILE_LOAD_FAILED;
    WifiAutoPushApiService::handleApiPushNow(server, makeRuntime(rt), []() { return true; });
    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Failed to load profile\""));
}

void test_push_now_success_with_profile_override() {
    WebServer server(80);
    FakeRuntime rt;
    rt.queueResult = WifiAutoPushApiService::PushNowQueueResult::QUEUED;
    server.setArg("slot", "2");
    server.setArg("profile", "Weekend");
    server.setArg("mode", "3");

    WifiAutoPushApiService::handleApiPushNow(
        server,
        makeRuntime(rt),
        []() { return true; });

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"queued\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.queuePushNowCalls);
    TEST_ASSERT_EQUAL_INT(2, rt.lastPushRequest.slot);
    TEST_ASSERT_TRUE(rt.lastPushRequest.hasProfileOverride);
    TEST_ASSERT_EQUAL_STRING("Weekend", rt.lastPushRequest.profileName.c_str());
    TEST_ASSERT_TRUE(rt.lastPushRequest.hasModeOverride);
    TEST_ASSERT_EQUAL_INT(3, rt.lastPushRequest.mode);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_slots_serializes_expected_payload);
    RUN_TEST(test_status_returns_500_when_unavailable);
    RUN_TEST(test_status_returns_500_when_callback_missing);
    RUN_TEST(test_status_returns_json_when_available);
    RUN_TEST(test_slot_save_rate_limited_short_circuits);
    RUN_TEST(test_slot_save_missing_params_returns_400);
    RUN_TEST(test_slot_save_invalid_slot_returns_400);
    RUN_TEST(test_slot_save_success_updates_slot_runtime);
    RUN_TEST(test_activate_missing_slot_returns_400);
    RUN_TEST(test_activate_invalid_slot_returns_400);
    RUN_TEST(test_activate_success_defaults_enable_true);
    RUN_TEST(test_activate_success_enable_false);
    RUN_TEST(test_push_now_rate_limited_short_circuits);
    RUN_TEST(test_push_now_missing_slot_returns_400);
    RUN_TEST(test_push_now_invalid_slot_returns_400);
    RUN_TEST(test_push_now_maps_runtime_error_states);
    RUN_TEST(test_push_now_success_with_profile_override);
    return UNITY_END();
}

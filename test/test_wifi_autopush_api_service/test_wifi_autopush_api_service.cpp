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

    int loadSlotsCalls = 0;
    int loadStatusCalls = 0;
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

    WifiAutoPushApiService::handleSlots(server, makeRuntime(rt));

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

    WifiAutoPushApiService::handleStatus(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Push status not available\""));
    TEST_ASSERT_EQUAL_INT(1, rt.loadStatusCalls);
}

void test_status_returns_500_when_callback_missing() {
    WebServer server(80);
    WifiAutoPushApiService::Runtime runtime{};

    WifiAutoPushApiService::handleStatus(server, runtime);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Push status not available\""));
}

void test_status_returns_json_when_available() {
    WebServer server(80);
    FakeRuntime rt;
    rt.statusAvailable = true;
    rt.statusJson = "{\"ok\":true,\"queueDepth\":1}";

    WifiAutoPushApiService::handleStatus(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"queueDepth\":1"));
    TEST_ASSERT_EQUAL_INT(1, rt.loadStatusCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_slots_serializes_expected_payload);
    RUN_TEST(test_status_returns_500_when_unavailable);
    RUN_TEST(test_status_returns_500_when_callback_missing);
    RUN_TEST(test_status_returns_json_when_available);
    return UNITY_END();
}

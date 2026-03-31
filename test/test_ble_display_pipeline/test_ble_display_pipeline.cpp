#include <unity.h>

// Override esp_timer_get_time with mockMicros BEFORE Arduino.h defines its default.
// Forward-declare the variable so the function body can reference it.
extern unsigned long mockMicros;
#define ESP_TIMER_GET_TIME_DEFINED
extern "C" int64_t esp_timer_get_time(void) {
    return static_cast<int64_t>(mockMicros);
}

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/v1_profiles.h"
#include "../mocks/modules/display/display_preview_module.h"
#include "../mocks/modules/power/power_module.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/perf_metrics.h"
PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
void perfRecordBleTimelineEvent(PerfBleTimelineEvent /*event*/, uint32_t /*nowMs*/) {}

// Use real parser implementation for BLE->parser integration coverage.
#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../../src/modules/system/system_event_bus.h"
#include "../../src/modules/ble/ble_queue_module.cpp"

#include <vector>

static V1BLEClient ble;
static PacketParser parser;
static V1ProfileManager profiles;
static DisplayPreviewModule preview;
static PowerModule power;
static SystemEventBus eventBus;
static BleQueueModule bleQueue;

static std::vector<uint8_t> makePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    packet.reserve(6 + payload.size());
    packet.push_back(ESP_PACKET_START);
    packet.push_back(0xDA);  // Destination (not validated)
    packet.push_back(0xE4);  // Origin (not validated)
    packet.push_back(packetId);
    packet.push_back(static_cast<uint8_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    packet.push_back(ESP_PACKET_END);
    return packet;
}

void setUp() {
    ble.reset();
    parser = PacketParser{};
    profiles.reset();
    preview = DisplayPreviewModule{};
    power.reset();
    eventBus.reset();
    perfCounters.reset();
    perfExtended.reset();
    bleQueue = BleQueueModule{};
    bleQueue.begin(&ble, &parser, &profiles, &preview, &power, &eventBus);
}

void tearDown() {}

void test_ble_queue_parses_display_packet_into_display_state() {
    // payload: bogey='5', led bars=3, K band+front arrow+mute, volume=5/2
    const std::vector<uint8_t> payload = {
        109,   // bogey counter byte ('5')
        0x00,  // image2 of bogey counter (unused)
        0x07,  // LED bar bitmap (3 bars)
        0x34,  // image1: K + mute + front arrow
        0x34,  // image2: steady same bits
        0x00,  // aux0
        0x00,  // aux1
        0x52   // aux2: main=5, mute=2
    };
    const std::vector<uint8_t> packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);

    mockMillis = 2500;
    mockMicros = 2500 * 1000UL;
    bleQueue.onNotify(packet.data(), packet.size(), 0xB2CE);
    bleQueue.process();

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(BAND_K, state.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_FRONT, state.arrows);
    TEST_ASSERT_EQUAL_UINT8(3, state.signalBars);
    TEST_ASSERT_TRUE(state.muted);
    TEST_ASSERT_EQUAL_UINT8(5, state.mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, state.muteVolume);

    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT32(2500, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_EQUAL(1, power.onV1DataReceivedCalls);
}

void test_ble_queue_publishes_parsed_event_to_bus() {
    const std::vector<uint8_t> payload = {
        0x3F, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x41
    };
    const std::vector<uint8_t> packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);

    mockMillis = 1234;
    mockMicros = 1234 * 1000UL;
    bleQueue.onNotify(packet.data(), packet.size(), 0xB2CE);
    bleQueue.process();

    SystemEvent event{};
    TEST_ASSERT_TRUE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SystemEventType::BLE_FRAME_PARSED),
                            static_cast<uint8_t>(event.type));
    TEST_ASSERT_EQUAL_UINT32(1234, event.tsMs);
    TEST_ASSERT_EQUAL_UINT16(PACKET_ID_DISPLAY_DATA, event.detail);
    TEST_ASSERT_TRUE(event.seq > 0);
}

void test_ble_queue_coalesces_parsed_events_per_process_cycle() {
    const std::vector<uint8_t> payloadA = {
        109, 0x00, 0x07, 0x34, 0x34, 0x00, 0x00, 0x52
    };
    const std::vector<uint8_t> payloadB = {
        0x3F, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x41
    };

    const std::vector<uint8_t> packetA = makePacket(PACKET_ID_DISPLAY_DATA, payloadA);
    const std::vector<uint8_t> packetB = makePacket(PACKET_ID_DISPLAY_DATA, payloadB);

    mockMillis = 7777;
    mockMicros = 7777 * 1000UL;
    bleQueue.onNotify(packetA.data(), packetA.size(), 0xB2CE);
    bleQueue.onNotify(packetB.data(), packetB.size(), 0xB2CE);
    bleQueue.process();

    SystemEvent event{};
    TEST_ASSERT_TRUE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
}

void test_ble_queue_resyncs_after_corrupt_prefix_in_single_notify() {
    const std::vector<uint8_t> payload = {
        109, 0x00, 0x07, 0x34, 0x34, 0x00, 0x00, 0x52
    };
    const std::vector<uint8_t> packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);
    std::vector<uint8_t> bytes = {0x00, 0x55, 0x99, 0xAB};
    bytes.insert(bytes.end(), packet.begin(), packet.end());

    mockMillis = 4321;
    mockMicros = 4321 * 1000UL;
    bleQueue.onNotify(bytes.data(), bytes.size(), 0xB2CE);
    bleQueue.process();

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(BAND_K, state.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_FRONT, state.arrows);
    TEST_ASSERT_EQUAL_UINT8(3, state.signalBars);
    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT32(4321, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_EQUAL(1, power.onV1DataReceivedCalls);

    SystemEvent event{};
    TEST_ASSERT_TRUE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_EQUAL_UINT32(4321, event.tsMs);
    TEST_ASSERT_EQUAL_UINT16(PACKET_ID_DISPLAY_DATA, event.detail);
}

void test_ble_queue_recovers_after_buffer_without_start_marker() {
    const std::vector<uint8_t> payload = {
        0x3F, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x41
    };
    const std::vector<uint8_t> packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);
    const std::vector<uint8_t> garbage = {0x01, 0x02, 0x03, 0x04, 0x05};

    mockMillis = 1500;
    mockMicros = 1500 * 1000UL;
    bleQueue.onNotify(garbage.data(), garbage.size(), 0xB2CE);
    bleQueue.process();

    SystemEvent event{};
    TEST_ASSERT_FALSE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_EQUAL_UINT32(0, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_EQUAL(0, power.onV1DataReceivedCalls);

    mockMillis = 2600;
    mockMicros = 2600 * 1000UL;
    bleQueue.onNotify(packet.data(), packet.size(), 0xB2CE);
    bleQueue.process();

    TEST_ASSERT_TRUE(bleQueue.consumeParsedFlag());
    TEST_ASSERT_EQUAL_UINT32(2600, bleQueue.getLastParsedTimestamp());
    TEST_ASSERT_EQUAL(1, power.onV1DataReceivedCalls);
    TEST_ASSERT_TRUE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_FALSE(eventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event));
    TEST_ASSERT_EQUAL_UINT32(2600, event.tsMs);
    TEST_ASSERT_EQUAL_UINT16(PACKET_ID_DISPLAY_DATA, event.detail);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ble_queue_parses_display_packet_into_display_state);
    RUN_TEST(test_ble_queue_publishes_parsed_event_to_bus);
    RUN_TEST(test_ble_queue_coalesces_parsed_events_per_process_cycle);
    RUN_TEST(test_ble_queue_resyncs_after_corrupt_prefix_in_single_notify);
    RUN_TEST(test_ble_queue_recovers_after_buffer_without_start_marker);
    return UNITY_END();
}

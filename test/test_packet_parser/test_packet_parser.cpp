#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// packet_parser.cpp pulls ../include/config.h. In native tests we only need the
// protocol constants, not the full display wiring config.
#ifndef CONFIG_H
#define CONFIG_H
#define ESP_PACKET_START 0xAA
#define ESP_PACKET_END 0xAB
#define PACKET_ID_DISPLAY_DATA 0x31
#define PACKET_ID_ALERT_DATA 0x43
#define PACKET_ID_WRITE_USER_BYTES 0x13
#define PACKET_ID_TURN_OFF_DISPLAY 0x32
#define PACKET_ID_TURN_ON_DISPLAY 0x33
#define PACKET_ID_MUTE_ON 0x34
#define PACKET_ID_MUTE_OFF 0x35
#define PACKET_ID_REQ_WRITE_VOLUME 0x39
#define PACKET_ID_RESP_USER_BYTES 0x12
#define PACKET_ID_VERSION 0x01
#endif

#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"

#include <vector>

namespace {

std::vector<uint8_t> makePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    packet.reserve(6 + payload.size());
    packet.push_back(ESP_PACKET_START);
    packet.push_back(0xDA);
    packet.push_back(0xE4);
    packet.push_back(packetId);
    packet.push_back(static_cast<uint8_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    packet.push_back(ESP_PACKET_END);
    return packet;
}

std::vector<uint8_t> makeDisplayPayload(uint8_t bogeyByte,
                                        uint8_t barBitmap,
                                        uint8_t image1,
                                        uint8_t image2,
                                        uint8_t aux0 = 0,
                                        uint8_t aux1 = 0,
                                        uint8_t aux2 = 0) {
    return std::vector<uint8_t>{bogeyByte, 0x00, barBitmap, image1, image2, aux0, aux1, aux2};
}

std::vector<uint8_t> makeVersionPayload(char major,
                                        char minor,
                                        char rev1,
                                        char rev2,
                                        char ctrl) {
    return std::vector<uint8_t>{0x00,
                                static_cast<uint8_t>('V'),
                                static_cast<uint8_t>(major),
                                static_cast<uint8_t>(minor),
                                static_cast<uint8_t>(rev1),
                                static_cast<uint8_t>(rev2),
                                static_cast<uint8_t>(ctrl)};
}

}  // namespace

void setUp() {
#ifndef ARDUINO
    mockMillis = 0;
    mockMicros = 0;
#endif
}

void tearDown() {}

void test_parse_display_packet_updates_render_state() {
    PacketParser parser;
    const auto payload = makeDisplayPayload(
        static_cast<uint8_t>(115 | 0x80),  // 'P' with decimal point
        0x03,                              // 2 bars
        0x52,                              // Ka + side + mute
        0x42,                              // steady side only
        0x00,
        0x04,                              // mode=A
        0x73);                             // main=7 mute=3
    const auto packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);

    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size()));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, state.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_SIDE, state.arrows);
    TEST_ASSERT_TRUE(state.muted);
    TEST_ASSERT_EQUAL_UINT8(2, state.signalBars);
    TEST_ASSERT_EQUAL('P', state.bogeyCounterChar);
    TEST_ASSERT_TRUE(state.bogeyCounterDot);
    TEST_ASSERT_TRUE(state.hasMode);
    TEST_ASSERT_EQUAL('A', state.modeChar);
    TEST_ASSERT_EQUAL_UINT8(7, state.mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, state.muteVolume);
    TEST_ASSERT_TRUE(state.hasVolumeData);
    TEST_ASSERT_EQUAL_UINT8(0x00, state.bandFlashBits);
    TEST_ASSERT_EQUAL_UINT8(0x00, state.flashBits);
}

void test_parse_display_packet_zero_volume_forces_muted() {
    PacketParser parser;
    const auto packet = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(63, 0x01, 0x20, 0x20, 0x00, 0x00, 0x00));

    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size()));
    TEST_ASSERT_TRUE(parser.getDisplayState().muted);
}

void test_parse_packet_rejects_six_byte_frame() {
    PacketParser parser;
    const uint8_t packet[] = {0xAA, 0xDA, 0xE4, 0x31, 0x00, 0xAB};
    TEST_ASSERT_FALSE(parser.validatePacketForTest(packet, sizeof(packet)));
    TEST_ASSERT_FALSE(parser.parse(packet, sizeof(packet)));
}

void test_parse_packet_rejects_seven_byte_frame() {
    PacketParser parser;
    const uint8_t packet[] = {0xAA, 0xDA, 0xE4, 0x31, 0x01, 0x00, 0xAB};
    TEST_ASSERT_FALSE(parser.validatePacketForTest(packet, sizeof(packet)));
    TEST_ASSERT_FALSE(parser.parse(packet, sizeof(packet)));
}

void test_parse_packet_rejects_bad_framing() {
    PacketParser parser;
    const auto packet = makePacket(PACKET_ID_DISPLAY_DATA, makeDisplayPayload(63, 0x01, 0x20, 0x20));

    std::vector<uint8_t> badStart = packet;
    badStart.front() = 0xBB;
    TEST_ASSERT_FALSE(parser.parse(badStart.data(), badStart.size()));

    std::vector<uint8_t> badEnd = packet;
    badEnd.back() = 0xAC;
    TEST_ASSERT_FALSE(parser.parse(badEnd.data(), badEnd.size()));
}

void test_parse_version_packet_records_supported_volume_version() {
    PacketParser parser;
    const auto packet = makePacket(PACKET_ID_VERSION, makeVersionPayload('4', '1', '0', '2', '8'));

    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size()));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_TRUE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(41028, state.v1FirmwareVersion);
    TEST_ASSERT_TRUE(state.supportsVolume());
}

void test_parse_display_ack_packets_toggle_display_state() {
    PacketParser parser;
    const auto darkPacket = makePacket(PACKET_ID_TURN_OFF_DISPLAY, {0x00, 0x00});
    const auto lightPacket = makePacket(PACKET_ID_TURN_ON_DISPLAY, {0x00});

    TEST_ASSERT_TRUE(parser.parse(darkPacket.data(), darkPacket.size()));
    TEST_ASSERT_FALSE(parser.getDisplayState().displayOn);
    TEST_ASSERT_TRUE(parser.getDisplayState().hasDisplayOn);

    TEST_ASSERT_TRUE(parser.parse(lightPacket.data(), lightPacket.size()));
    TEST_ASSERT_TRUE(parser.getDisplayState().displayOn);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_parse_display_packet_updates_render_state);
    RUN_TEST(test_parse_display_packet_zero_volume_forces_muted);
    RUN_TEST(test_parse_packet_rejects_six_byte_frame);
    RUN_TEST(test_parse_packet_rejects_seven_byte_frame);
    RUN_TEST(test_parse_packet_rejects_bad_framing);
    RUN_TEST(test_parse_version_packet_records_supported_volume_version);
    RUN_TEST(test_parse_display_ack_packets_toggle_display_state);
    return UNITY_END();
}

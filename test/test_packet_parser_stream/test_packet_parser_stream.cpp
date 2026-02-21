#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// packet_parser.cpp includes ../include/config.h for packet IDs.
// In native tests we only need protocol constants, not display driver wiring.
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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool strictAuditEnabled() {
#ifdef ARDUINO
    return false;
#else
    const char* raw = std::getenv("V1_STRICT_BACKUP_AUDIT");
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    return raw[0] != '0';
#endif
}

void requireStrictAuditEnabled() {
    if (!strictAuditEnabled()) {
        TEST_IGNORE_MESSAGE("Set V1_STRICT_BACKUP_AUDIT=1 to enable strict backup-feature audit checks");
    }
}

std::string readTextFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> makePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    packet.reserve(6 + payload.size());
    packet.push_back(ESP_PACKET_START);
    packet.push_back(0xDA);  // Dest (not validated by parser)
    packet.push_back(0xE4);  // Orig (not validated by parser)
    packet.push_back(packetId);
    packet.push_back(static_cast<uint8_t>(payload.size()));  // Length hint (not enforced)
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
    // payload[0]=bogey1, [1]=bogey2, [2]=bars, [3]=image1, [4]=image2, [5..7]=aux
    return std::vector<uint8_t>{bogeyByte, 0x00, barBitmap, image1, image2, aux0, aux1, aux2};
}

std::vector<uint8_t> makeAlertPayload(uint8_t index,
                                      uint8_t count,
                                      uint16_t freqMHz,
                                      uint8_t frontRaw,
                                      uint8_t rearRaw,
                                      uint8_t bandArrow,
                                      uint8_t aux0) {
    const uint8_t indexCount = static_cast<uint8_t>(((index & 0x0F) << 4) | (count & 0x0F));
    return std::vector<uint8_t>{
        indexCount,
        static_cast<uint8_t>((freqMHz >> 8) & 0xFF),
        static_cast<uint8_t>(freqMHz & 0xFF),
        frontRaw,
        rearRaw,
        bandArrow,
        aux0
    };
}

void assertContainsFrequencies(const PacketParser& parser, uint16_t a, uint16_t b) {
    const auto& alerts = parser.getAllAlerts();
    const size_t count = parser.getAlertCount();
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(count));

    bool foundA = false;
    bool foundB = false;
    for (size_t i = 0; i < count; ++i) {
        if (alerts[i].frequency == a) foundA = true;
        if (alerts[i].frequency == b) foundB = true;
    }
    TEST_ASSERT_TRUE(foundA);
    TEST_ASSERT_TRUE(foundB);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_display_stream_decodes_junk_counter_char() {
    PacketParser parser;

    // 30 = 'J', with decimal point bit set.
    const auto payload = makeDisplayPayload(static_cast<uint8_t>(30 | 0x80), 0x03, 0x24, 0x24);
    const auto packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);

    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size()));
    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL('J', state.bogeyCounterChar);
    TEST_ASSERT_TRUE(state.bogeyCounterDot);
}

void test_alert_stream_out_of_order_rows_completes_table() {
    PacketParser parser;

    // Count=2, send index 2 first then index 1.
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 34700, 0xB0, 0x00, 0x22, 0x80));

    TEST_ASSERT_TRUE(parser.parse(row2.data(), row2.size()));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    TEST_ASSERT_TRUE(parser.parse(row1.data(), row1.size()));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    TEST_ASSERT_TRUE(parser.hasAlerts());
    assertContainsFrequencies(parser, 24150, 34700);
}

void test_alert_stream_duplicate_index_replaces_prior_row() {
    PacketParser parser;

    const auto row1a = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    const auto row1b = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 24152, 0x92, 0x81, 0x84, 0x00));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 34700, 0xB0, 0x00, 0x22, 0x80));

    TEST_ASSERT_TRUE(parser.parse(row1a.data(), row1a.size()));
    TEST_ASSERT_TRUE(parser.parse(row1b.data(), row1b.size()));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));
    TEST_ASSERT_TRUE(parser.parse(row2.data(), row2.size()));

    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24152, 34700);
}

void test_alert_stream_zero_based_index_fallback_supported() {
    PacketParser parser;

    const auto row0 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(0, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 34700, 0xB0, 0x00, 0x22, 0x80));

    TEST_ASSERT_TRUE(parser.parse(row0.data(), row0.size()));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));
    TEST_ASSERT_TRUE(parser.parse(row1.data(), row1.size()));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24150, 34700);
}

void test_alert_stream_missing_row_keeps_previous_complete_table() {
    PacketParser parser;

    // Build an initial complete table (count=2).
    const auto full1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 34700, 0xB0, 0x00, 0x22, 0x80));
    const auto full2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    TEST_ASSERT_TRUE(parser.parse(full1.data(), full1.size()));
    TEST_ASSERT_TRUE(parser.parse(full2.data(), full2.size()));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));

    // Start next table (count=3) but provide only one row.
    const auto partial = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 3, 10525, 0x88, 0x00, 0xA8, 0x80));
    TEST_ASSERT_TRUE(parser.parse(partial.data(), partial.size()));

    // Parser should keep prior complete table until new table is complete.
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24150, 34700);
}

void test_alert_stream_count_zero_clears_alerts() {
    PacketParser parser;

    const auto full1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 1, 34700, 0xB0, 0x00, 0x22, 0x80));
    TEST_ASSERT_TRUE(parser.parse(full1.data(), full1.size()));
    TEST_ASSERT_TRUE(parser.hasAlerts());

    // Count=0 clear row.
    const auto clear = makePacket(PACKET_ID_ALERT_DATA, std::vector<uint8_t>{0x00, 0x00});
    TEST_ASSERT_TRUE(parser.parse(clear.data(), clear.size()));
    TEST_ASSERT_FALSE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));
}

void test_strict_alert_stream_duplicate_index_replaces_prior_row() {
    requireStrictAuditEnabled();
    PacketParser parser;

    const auto row1a = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    const auto row1b = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 24152, 0x92, 0x81, 0x84, 0x00));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 34700, 0xB0, 0x00, 0x22, 0x80));

    TEST_ASSERT_TRUE(parser.parse(row1a.data(), row1a.size()));
    TEST_ASSERT_TRUE(parser.parse(row1b.data(), row1b.size()));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0,
        static_cast<uint32_t>(parser.getAlertCount()),
        "strict: parser should wait for unique row indexes before publishing a table");

    TEST_ASSERT_TRUE(parser.parse(row2.data(), row2.size()));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24152, 34700);
}

void test_strict_contract_parser_aux0_fields_exist() {
    requireStrictAuditEnabled();
    const std::string header = readTextFile("src/packet_parser.h");
    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "strict: failed to read src/packet_parser.h");
    TEST_ASSERT_TRUE_MESSAGE(header.find("isJunk") != std::string::npos,
                             "strict: AlertData must expose aux0 junk bit");
    TEST_ASSERT_TRUE_MESSAGE(header.find("photoType") != std::string::npos,
                             "strict: AlertData must expose aux0 photo type");
    TEST_ASSERT_TRUE_MESSAGE(header.find("hasJunkAlert") != std::string::npos,
                             "strict: DisplayState must expose table-level junk state");
    TEST_ASSERT_TRUE_MESSAGE(header.find("hasPhotoAlert") != std::string::npos,
                             "strict: DisplayState must expose table-level photo state");
}

void test_strict_contract_parser_aux0_fw_gates_exist() {
    requireStrictAuditEnabled();
    const std::string src = readTextFile("src/packet_parser.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "strict: failed to read src/packet_parser.cpp");
    TEST_ASSERT_TRUE_MESSAGE(src.find("41032") != std::string::npos,
                             "strict: parser must gate junk aux0 bit to >= 4.1032");
    TEST_ASSERT_TRUE_MESSAGE(src.find("41037") != std::string::npos,
                             "strict: parser must gate photo aux0 bits to >= 4.1037");
}

void test_strict_contract_display_photo_fallback_exists() {
    requireStrictAuditEnabled();
    const std::string displaySrc = readTextFile("src/display_update.cpp");
    TEST_ASSERT_FALSE_MESSAGE(displaySrc.empty(), "strict: failed to read src/display_update.cpp");
    TEST_ASSERT_TRUE_MESSAGE(displaySrc.find("state.hasPhotoAlert") != std::string::npos,
                             "strict: display update must use table-level photo fallback");
    TEST_ASSERT_TRUE_MESSAGE(displaySrc.find("priority.photoType") != std::string::npos,
                             "strict: display update must use priority photoType");
    TEST_ASSERT_TRUE_MESSAGE(displaySrc.find("alert.photoType") != std::string::npos,
                             "strict: persisted display update must use alert photoType");

    const std::string mainSrc = readTextFile("src/main.cpp");
    TEST_ASSERT_FALSE_MESSAGE(mainSrc.empty(), "strict: failed to read src/main.cpp");
    TEST_ASSERT_TRUE_MESSAGE(mainSrc.find("state.hasPhotoAlert") != std::string::npos,
                             "strict: loop tail frequency refresh must use photo fallback state");
    TEST_ASSERT_TRUE_MESSAGE(mainSrc.find("priority.photoType") != std::string::npos,
                             "strict: loop tail frequency refresh must use priority photoType");
}

void test_strict_contract_live_top_counter_follows_raw_v1_symbol() {
    requireStrictAuditEnabled();
    const std::string src = readTextFile("src/display_update.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "strict: failed to read src/display_update.cpp");
    TEST_ASSERT_TRUE_MESSAGE(src.find("char liveTopCounterChar = state.bogeyCounterChar;") != std::string::npos,
                             "strict: live top counter must start from raw V1 symbol");
    TEST_ASSERT_TRUE_MESSAGE(src.find("bool liveTopCounterDot = state.bogeyCounterDot;") != std::string::npos,
                             "strict: live top counter dot must follow raw V1 packet");
    TEST_ASSERT_TRUE_MESSAGE(src.find("static_cast<char>('0' + alertCount)") == std::string::npos,
                             "strict: live top counter must not normalize to alert count");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_display_stream_decodes_junk_counter_char);
    RUN_TEST(test_alert_stream_out_of_order_rows_completes_table);
    RUN_TEST(test_alert_stream_duplicate_index_replaces_prior_row);
    RUN_TEST(test_alert_stream_zero_based_index_fallback_supported);
    RUN_TEST(test_alert_stream_missing_row_keeps_previous_complete_table);
    RUN_TEST(test_alert_stream_count_zero_clears_alerts);

    RUN_TEST(test_strict_alert_stream_duplicate_index_replaces_prior_row);
    RUN_TEST(test_strict_contract_parser_aux0_fields_exist);
    RUN_TEST(test_strict_contract_parser_aux0_fw_gates_exist);
    RUN_TEST(test_strict_contract_display_photo_fallback_exists);
    RUN_TEST(test_strict_contract_live_top_counter_follows_raw_v1_symbol);
    return UNITY_END();
}

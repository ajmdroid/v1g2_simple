/**
 * ESP Packet Parser for V1 Gen2
 *
 * The V1G2 packets are framed with 0xAA ... 0xAB. Packet ID lives at byte 3,
 * payload begins at byte 5 (after dest/src/id/len). The payload layout matches
 * the v1g2-t4s3 logic: 0x31 = display/update, 0x43 = alert table entries.
 */

#include "packet_parser.h"
#include "../include/config.h"
#include <algorithm>

namespace {
struct BandArrowData {
    bool laser = false;
    bool ka = false;
    bool k = false;
    bool x = false;
    bool mute = false;
    bool front = false;
    bool side = false;
    bool rear = false;
};

uint16_t combineMSBLSB(uint8_t msb, uint8_t lsb) {
    return (static_cast<uint16_t>(msb) << 8) | lsb;
}

BandArrowData processBandArrow(uint8_t v) {
    BandArrowData d;
    d.laser = (v & 0b00000001) != 0;
    d.ka    = (v & 0b00000010) != 0;
    d.k     = (v & 0b00000100) != 0;
    d.x     = (v & 0b00001000) != 0;
    d.mute  = (v & 0b00010000) != 0;
    d.front = (v & 0b00100000) != 0;
    d.side  = (v & 0b01000000) != 0;
    d.rear  = (v & 0b10000000) != 0;
    return d;
}
} // namespace

PacketParser::PacketParser() {
    alerts.clear();
    alertChunks.clear();
}

bool PacketParser::parse(const uint8_t* data, size_t length) {
    if (!validatePacket(data, length)) {
        return false;
    }

    uint8_t packetId = data[3];
    const uint8_t* payload = (length > 5) ? &data[5] : nullptr;
    size_t payloadLen = (length > 6) ? length - 6 : 0; // drop start/dest/src/id/len/end

    switch (packetId) {
        case PACKET_ID_DISPLAY_DATA:
            return parseDisplayData(payload, payloadLen);
        case PACKET_ID_ALERT_DATA:
            return parseAlertData(payload, payloadLen);
        default:
            Serial.printf("Unknown packet ID: 0x%02X (len=%u)\n", packetId, (unsigned)length);
            return false;
    }
}

bool PacketParser::validatePacket(const uint8_t* data, size_t length) {
    if (length < 8) {
        return false;
    }
    if (data[0] != ESP_PACKET_START || data[length - 1] != ESP_PACKET_END) {
        return false;
    }
    return true; // checksum intentionally not enforced; V1G2 can chunk packets
}

uint8_t PacketParser::calculateChecksum(const uint8_t* data, size_t length) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum += data[i];
    }
    return checksum;
}

bool PacketParser::parseDisplayData(const uint8_t* payload, size_t length) {
    // Expected payload >= 8 bytes (matches v1g2-t4s3 parsing window)
    if (!payload || length < 8) {
        return false;
    }

    // band/arrow information sits at payload[3]
    BandArrowData arrow = processBandArrow(payload[3]);
    decodeMode(payload, length);

    displayState.activeBands = BAND_NONE;
    if (arrow.laser) displayState.activeBands |= BAND_LASER;
    if (arrow.ka)    displayState.activeBands |= BAND_KA;
    if (arrow.k)     displayState.activeBands |= BAND_K;
    if (arrow.x)     displayState.activeBands |= BAND_X;

    displayState.arrows = DIR_NONE;
    if (arrow.front) displayState.arrows = static_cast<Direction>(displayState.arrows | DIR_FRONT);
    if (arrow.side)  displayState.arrows = static_cast<Direction>(displayState.arrows | DIR_SIDE);
    if (arrow.rear)  displayState.arrows = static_cast<Direction>(displayState.arrows | DIR_REAR);

    displayState.muted = arrow.mute;
    
    // If laser is detected via display data, set full signal bars
    // Laser alerts don't have granular strength - they're on/off
    if (arrow.laser) {
        displayState.signalBars = 6; // Full bars for laser
    } else if (displayState.activeBands == BAND_NONE) {
        // No active bands from display data - clear signal bars
        // (alert data will override this if alerts are present)
        displayState.signalBars = 0;
    }
    
    return true;
}

Band PacketParser::decodeBand(uint8_t bandArrow) const {
    if (bandArrow & 0b00000001) return BAND_LASER;
    if (bandArrow & 0b00000010) return BAND_KA;
    if (bandArrow & 0b00000100) return BAND_K;
    if (bandArrow & 0b00001000) return BAND_X;
    return BAND_NONE;
}

Direction PacketParser::decodeDirection(uint8_t bandArrow) const {
    if (bandArrow & 0b00100000) return DIR_FRONT;
    if (bandArrow & 0b01000000) return DIR_SIDE;
    if (bandArrow & 0b10000000) return DIR_REAR;
    return DIR_NONE;
}

uint8_t PacketParser::mapStrengthToBars(Band band, uint8_t raw) const {
    // V1 Gen2 uses 0-6 signal bar range
    // If the device already provides a 0-6 style value, use it directly
    if (raw <= 6) {
        return raw;
    }

    // Threshold tables for raw RSSI -> 0..6 bars
    // Adjusted from original 0-8 scale to 0-6 scale
    static constexpr uint8_t kaThresholds[] = {0x00, 0x8F, 0x99, 0xA4, 0xAF, 0xB5, 0xFF};
    static constexpr uint8_t kThresholds[]  = {0x00, 0x87, 0x95, 0xA3, 0xB1, 0xBF, 0xFF};
    static constexpr uint8_t xThresholds[]  = {0x00, 0x95, 0xA5, 0xB3, 0xC0, 0xCC, 0xFF};

    const uint8_t* table = nullptr;
    switch (band) {
        case BAND_KA: table = kaThresholds; break;
        case BAND_K:  table = kThresholds;  break;
        case BAND_X:  table = xThresholds;  break;
        default:      return raw > 0 ? 6 : 0; // Laser/junk/unknown -> full bars
    }

    for (uint8_t i = 0; i < 7; ++i) {
        if (raw <= table[i]) {
            return i;
        }
    }
    return 0;
}

bool PacketParser::parseAlertData(const uint8_t* payload, size_t length) {
    if (!payload || length < 1) {
        return false;
    }

    uint8_t alertCount = payload[0] & 0x0F;
    if (alertCount == 0) {
        alerts.clear();
        alertChunks.clear();
        displayState.signalBars = 0;
        displayState.arrows = DIR_NONE;
        displayState.activeBands = BAND_NONE;
        displayState.muted = false;
        return true;
    }

    if (length < 7) {
        return false;
    }

    std::array<uint8_t, 7> chunk{};
    size_t copyLen = std::min<size_t>(7, length);
    for (size_t i = 0; i < copyLen; ++i) {
        chunk[i] = payload[i];
    }
    alertChunks.push_back(chunk);

    // Wait until we've received the full set of alert table rows
    if (alertChunks.size() < alertCount) {
        return true;
    }

    alerts.clear();
    displayState.activeBands = BAND_NONE;

    for (size_t i = 0; i < alertChunks.size() && i < alertCount; ++i) {
        const auto& a = alertChunks[i];
        uint8_t bandArrow = a[5];

        Band band = decodeBand(bandArrow);
        Direction dir = decodeDirection(bandArrow);

        AlertData alert;
        alert.band = band;
        alert.direction = dir;
        alert.frontStrength = mapStrengthToBars(band, a[3]);
        alert.rearStrength = mapStrengthToBars(band, a[4]);
        alert.frequency = (band == BAND_LASER) ? 0 : combineMSBLSB(a[1], a[2]); // MHz
        alert.isValid = true;

        alerts.push_back(alert);
        if (band != BAND_NONE) {
            displayState.activeBands |= band;
        }
        // Mute bit rides in bandArrow too
        displayState.muted = (bandArrow & 0x10) != 0;
    }

    if (!alerts.empty()) {
        AlertData priority = getPriorityAlert();
        displayState.signalBars = std::max(priority.frontStrength, priority.rearStrength);
        displayState.arrows = priority.direction;
    } else {
        displayState.signalBars = 0;
        displayState.arrows = DIR_NONE;
    }

    alertChunks.clear();
    return true;
}

void PacketParser::decodeMode(const uint8_t* payload, size_t length) {
    // Mode bits live in payload[6] (aux1) per v1g2-t4s3 decode_v2
    if (!payload || length < 7) {
        return;
    }
    uint8_t aux1 = payload[6];
    uint8_t mode = (aux1 >> 2) & 0x03;
    switch (mode) {
        case 1: displayState.modeChar = 'A'; break;
        case 2: displayState.modeChar = 'l'; break;
        case 3: displayState.modeChar = 'L'; break;
        default: displayState.modeChar = 0; break;
    }
    displayState.hasMode = displayState.modeChar != 0;
}

AlertData PacketParser::getPriorityAlert() const {
    if (alerts.empty()) {
        return AlertData();
    }

    AlertData priority = alerts[0];
    for (const auto& alert : alerts) {
        uint8_t strength = std::max(alert.frontStrength, alert.rearStrength);
        uint8_t prioStrength = std::max(priority.frontStrength, priority.rearStrength);
        if (strength > prioStrength) {
            priority = alert;
        }
    }

    return priority;
}

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

PacketParser::PacketParser() : alertCount(0), chunkCount(0) {
}

void PacketParser::resetAlertAssembly() {
    // Drop any partially collected alert rows without altering display state
    chunkCount = 0;
}

// Static flag to signal priority state reset on next call
static bool s_resetPriorityStateFlag = false;

void PacketParser::resetPriorityState() {
    s_resetPriorityStateFlag = true;
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
        
        // ACK responses from V1 to our commands - silently ignore
        case PACKET_ID_WRITE_USER_BYTES:    // 0x13 - ACK for profile write
            return true;  // Acknowledged, no further processing needed
        case PACKET_ID_TURN_OFF_DISPLAY:    // 0x32 - ACK for display off
            // Update display power state (dark mode enabled)
            displayState.displayOn = false;
            displayState.hasDisplayOn = true;
            return true;
        case PACKET_ID_TURN_ON_DISPLAY:     // 0x33 - ACK for display on
            // Update display power state (dark mode disabled)
            displayState.displayOn = true;
            displayState.hasDisplayOn = true;
            return true;
        case PACKET_ID_MUTE_ON:             // 0x34 - ACK for mute on
        case PACKET_ID_MUTE_OFF:            // 0x35 - ACK for mute off
        case 0x36:                          // ACK for mode change (reqChangeMode)
        case PACKET_ID_REQ_WRITE_VOLUME:    // 0x39 - ACK for volume change
        case PACKET_ID_VERSION:             // 0x01 - Version response
        case PACKET_ID_RESP_USER_BYTES:     // 0x12 - User bytes response
            return true;  // Acknowledged, no further processing needed
            
        default:
            // Unknown packet - silently ignore in hot path
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

bool PacketParser::parseDisplayData(const uint8_t* payload, size_t length) {
    // Expected payload >= 8 bytes (matches v1g2-t4s3 parsing window)
    if (!payload || length < 8) {
        return false;
    }

    // Display packet structure:
    // payload[3] = image1 (currently ON bits)
    // payload[4] = image2 (steady/NOT-flashing bits)
    // Bits in image1 but NOT in image2 = FLASHING
    // V1 hardware handles the actual blink animation internally - we must do the same
    
    uint8_t image1 = payload[3];
    uint8_t image2 = payload[4];
    
    // band/arrow information from image1
    BandArrowData arrow = processBandArrow(image1);
    decodeMode(payload, length);
    
    // Calculate flash bits: things that are ON (image1) but NOT steady (image2)
    // These bits should blink on our display
    uint8_t flashingBits = image1 & ~image2;
    
    // Band flash bits (lower nibble): L=0x01, Ka=0x02, K=0x04, X=0x08
    displayState.bandFlashBits = flashingBits & 0x0F;
    
    // Arrow flash bits (upper nibble): Front=0x20, Side=0x40, Rear=0x80
    displayState.flashBits = flashingBits & 0xE0;

    displayState.activeBands = BAND_NONE;
    if (arrow.laser) displayState.activeBands |= BAND_LASER;
    if (arrow.ka)    displayState.activeBands |= BAND_KA;
    if (arrow.k)     displayState.activeBands |= BAND_K;
    if (arrow.x)     displayState.activeBands |= BAND_X;

    displayState.arrows = DIR_NONE;
    if (arrow.front) displayState.arrows = static_cast<Direction>(displayState.arrows | DIR_FRONT);
    if (arrow.side)  displayState.arrows = static_cast<Direction>(displayState.arrows | DIR_SIDE);
    if (arrow.rear)  displayState.arrows = static_cast<Direction>(displayState.arrows | DIR_REAR);

    // Always trust display packet's mute flag - V1 logic mute shows here
    // even when individual alert entries don't have mute bit set
    displayState.muted = arrow.mute;
    
    // Extract volume from auxData2 - in raw packet it's at data[12]
    // Since we stripped 5 bytes (header), it's payload[7]
    // mainVol = upper nibble, muteVol = lower nibble
    if (length > 7) {
        uint8_t auxData2 = payload[7];
        displayState.mainVolume = (auxData2 & 0xF0) >> 4;
        displayState.muteVolume = auxData2 & 0x0F;
        
        // Consider muted if mute flag is set OR if main volume is zero
        if (displayState.mainVolume == 0) {
            displayState.muted = true;
        }
    }
    
    // If laser is detected via display data, set full signal bars
    // Laser alerts don't have granular strength - they're on/off
    if (arrow.laser) {
        displayState.signalBars = 6; // Full bars for laser
    } else if (displayState.activeBands == BAND_NONE) {
        // No active bands from display data - clear alerts immediately
        // Don't wait for alert packet with 0 count - display packet is authoritative
        displayState.signalBars = 0;
        alertCount = 0;
        chunkCount = 0;
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
    // V1 Gen2 sends raw RSSI values (typically 0x80-0xC0 range)
    // Use threshold tables to convert to 0-6 bar display
    // Match V1's visual decay rate - instant up, gradual down
    
    // Threshold tables for raw RSSI -> 0..6 bars
    // Values below 0x80 typically mean "no signal" on that antenna
    // Format: {0-bar max, 1-bar max, 2-bar max, 3-bar max, 4-bar max, 5-bar max, 6-bar}
    static constexpr uint8_t kaThresholds[] = {0x7F, 0x88, 0x92, 0x9C, 0xA6, 0xB0, 0xFF};
    static constexpr uint8_t kThresholds[]  = {0x7F, 0x86, 0x90, 0x9A, 0xA4, 0xAE, 0xFF};
    static constexpr uint8_t xThresholds[]  = {0x7F, 0x8A, 0x98, 0xA6, 0xB4, 0xC2, 0xFF};

    const uint8_t* table = nullptr;
    switch (band) {
        case BAND_KA: table = kaThresholds; break;
        case BAND_K:  table = kThresholds;  break;
        case BAND_X:  table = xThresholds;  break;
        case BAND_LASER: {
            uint8_t bars = (raw > 0x10) ? 6 : 0; // treat tiny noise as zero
            return bars;
        }
        default:
            return 0;
    }

    // Time-based decay for signal bar smoothing
    // Faster decay for snappier response (was 150ms to match V1)
    static uint8_t lastBarsKa = 0, lastBarsK = 0, lastBarsX = 0;
    static unsigned long lastDropTimeKa = 0, lastDropTimeK = 0, lastDropTimeX = 0;
    constexpr unsigned long DROP_INTERVAL_MS = 100;  // Reduced from 150ms for snappier response
    
    uint8_t* lastBarsPtr = nullptr;
    unsigned long* lastDropTimePtr = nullptr;
    switch (band) {
        case BAND_KA: lastBarsPtr = &lastBarsKa; lastDropTimePtr = &lastDropTimeKa; break;
        case BAND_K:  lastBarsPtr = &lastBarsK;  lastDropTimePtr = &lastDropTimeK;  break;
        case BAND_X:  lastBarsPtr = &lastBarsX;  lastDropTimePtr = &lastDropTimeX;  break;
        default: break;
    }

    // Map raw to bars
    uint8_t newBars = 0;
    for (uint8_t i = 0; i < 7; ++i) {
        if (raw <= table[i]) {
            newBars = i;
            break;
        }
    }
    if (newBars == 0 && raw > table[0]) {
        newBars = 6;
    }

    if (lastBarsPtr && lastDropTimePtr) {
        unsigned long now = millis();
        
        if (newBars >= *lastBarsPtr) {
            // Going up or same - instant response
            *lastBarsPtr = newBars;
            *lastDropTimePtr = now;
        } else {
            // Going down - limit drop rate to match V1
            unsigned long elapsed = now - *lastDropTimePtr;
            uint8_t allowedDrops = elapsed / DROP_INTERVAL_MS;
            
            if (allowedDrops > 0) {
                uint8_t targetBars = (*lastBarsPtr > allowedDrops) ? (*lastBarsPtr - allowedDrops) : 0;
                if (targetBars < newBars) targetBars = newBars;  // Don't go below actual signal
                *lastBarsPtr = targetBars;
                *lastDropTimePtr = now;
            }
            newBars = *lastBarsPtr;
        }
    }

    return newBars;
}

bool PacketParser::parseAlertData(const uint8_t* payload, size_t length) {
    if (!payload || length < 1) {
        return false;
    }

    // Byte0: low nibble = alert count; rest of row carries frequency/strength/arrow
    uint8_t receivedAlertCount = payload[0] & 0x0F;
    if (receivedAlertCount == 0) {
        alertCount = 0;
        chunkCount = 0;
        displayState.signalBars = 0;
        displayState.arrows = DIR_NONE;
        displayState.activeBands = BAND_NONE;
        displayState.muted = false;
        return true;
    }

    // Each alert row is 7-8 bytes in V1G2 captures; require at least 7
    if (length < 7) {
        return false;
    }

    // Add new chunk with strict bounds checking to prevent overflow
    if (chunkCount >= MAX_ALERTS) {
        // Overflow - silently drop (hot path)
        return false;
    }

    std::array<uint8_t, 8> chunk{};
    size_t copyLen = std::min<size_t>(8, length);
    for (size_t i = 0; i < copyLen; ++i) {
        chunk[i] = payload[i];
    }
    alertChunks[chunkCount++] = chunk;

    // Wait until we've received the full set of alert table rows
    if (chunkCount < receivedAlertCount) {
        return true;
    }

    alertCount = 0;
    displayState.activeBands = BAND_NONE;
    bool anyMuted = false;

    for (size_t i = 0; i < chunkCount && i < receivedAlertCount; ++i) {
        const auto& a = alertChunks[i];
        uint8_t bandArrow = a[5];  // band + arrow + mute (matches captures: 0x24 for K/front)

        Band band = decodeBand(bandArrow);
        Direction dir = decodeDirection(bandArrow);

        // Add new alert, checking for overflow
        if (alertCount < MAX_ALERTS) {
            AlertData& alert = alerts[alertCount++];
            alert.band = band;
            alert.direction = dir;
            alert.frontStrength = mapStrengthToBars(band, a[3]);
            alert.rearStrength = mapStrengthToBars(band, a[4]);
            alert.frequency = (band == BAND_LASER) ? 0 : combineMSBLSB(a[1], a[2]); // MHz
            alert.isValid = true;

            if (band != BAND_NONE) {
                displayState.activeBands |= band;
            }
            anyMuted |= ((bandArrow & 0x10) != 0);
        }
    }

    // Combine alert mute bits with display packet's mute flag
    // V1 logic mute shows in display packet even if alert entries don't have mute bit
    displayState.muted = displayState.muted || anyMuted;

    if (alertCount > 0) {
        // Find MAX signal strength across ALL alerts (not just priority)
        // V1 display shows the strongest signal from any alert
        uint8_t maxSignal = 0;
        size_t priorityIdx = 0;
        for (size_t i = 0; i < alertCount; ++i) {
            uint8_t sig = std::max(alerts[i].frontStrength, alerts[i].rearStrength);
            if (sig > maxSignal) {
                maxSignal = sig;
                priorityIdx = i;
            }
        }
        displayState.signalBars = maxSignal;
        
        // Set priority arrow from the strongest alert (for multi-alert display)
        displayState.priorityArrow = alerts[priorityIdx].direction;
        
        // Note: displayState.arrows already set by parseDisplayData() - shows ALL active directions
    } else {
        displayState.signalBars = 0;
        displayState.arrows = DIR_NONE;
    }

    chunkCount = 0; // Clear chunks after processing
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
    if (alertCount == 0) {
        return AlertData();
    }

    // Priority rules:
    // 1. Laser ALWAYS wins (it's always 6 bars and highest threat)
    // 2. Otherwise, strongest signal wins
    // 3. Use hysteresis only for close calls to prevent flickering
    
    // First check if any alert is Laser - it always takes priority
    for (size_t i = 0; i < alertCount; ++i) {
        if (alerts[i].band == BAND_LASER && alerts[i].isValid) {
            return alerts[i];  // Laser always wins, no stickiness needed
        }
    }
    
    // No Laser - find strongest radar alert
    AlertData strongest = alerts[0];
    for (size_t i = 1; i < alertCount; ++i) {
        const auto& alert = alerts[i];
        uint8_t strength = std::max(alert.frontStrength, alert.rearStrength);
        uint8_t strongestStrength = std::max(strongest.frontStrength, strongest.rearStrength);
        if (strength > strongestStrength) {
            strongest = alert;
        }
    }
    
    // For radar alerts, use mild hysteresis to prevent flickering
    // Only stick with last priority if it's still present and not much weaker
    static AlertData lastPriority;
    static unsigned long lastPriorityTime = 0;
    
    // Check if reset was requested (e.g., on V1 disconnect)
    if (s_resetPriorityStateFlag) {
        lastPriority = AlertData();
        lastPriorityTime = 0;
        s_resetPriorityStateFlag = false;
    }
    
    constexpr unsigned long PRIORITY_STICK_MS = 300;   // Reduced from 500ms
    constexpr uint8_t PRIORITY_HYSTERESIS = 1;         // Reduced from 2 bars
    
    unsigned long now = millis();
    
    // Check if last priority is still present
    bool lastStillPresent = false;
    AlertData currentLastMatch;
    if (lastPriority.isValid && lastPriority.band != BAND_NONE && lastPriority.band != BAND_LASER) {
        for (size_t i = 0; i < alertCount; ++i) {
            if (alerts[i].band == lastPriority.band) {
                if (lastPriority.frequency == 0 || alerts[i].frequency == 0 ||
                    ((lastPriority.frequency > alerts[i].frequency) ? 
                     (lastPriority.frequency - alerts[i].frequency) : 
                     (alerts[i].frequency - lastPriority.frequency)) < 50) {
                    lastStillPresent = true;
                    currentLastMatch = alerts[i];
                    break;
                }
            }
        }
    }
    
    // Determine if we should switch
    bool shouldSwitch = true;  // Default to switching
    
    if (lastStillPresent) {
        uint8_t strongestStrength = std::max(strongest.frontStrength, strongest.rearStrength);
        uint8_t lastStrength = std::max(currentLastMatch.frontStrength, currentLastMatch.rearStrength);
        
        // Only stick if: same alert OR (within time window AND not much weaker)
        bool sameAlert = (strongest.band == lastPriority.band && 
                         (strongest.frequency == 0 || lastPriority.frequency == 0 ||
                          strongest.frequency == lastPriority.frequency));
        
        if (sameAlert) {
            shouldSwitch = false;  // Same alert, just update it
            lastPriority = strongest;
        } else if ((now - lastPriorityTime) < PRIORITY_STICK_MS && 
                   strongestStrength <= lastStrength + PRIORITY_HYSTERESIS) {
            // Within time window and new alert isn't significantly stronger
            shouldSwitch = false;
            lastPriority = currentLastMatch;  // Update with current strength
        }
    }
    
    if (shouldSwitch) {
        lastPriority = strongest;
        lastPriorityTime = now;
    }

    return lastPriority;
}

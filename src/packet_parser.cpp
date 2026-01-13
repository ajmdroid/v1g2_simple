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

// Static flag for priority state reset - no longer used since we trust V1's priority
// Kept for API compatibility
static bool s_resetPriorityStateFlag = false;

void PacketParser::resetPriorityState() {
    // No-op: We now trust V1's priority ordering directly (alerts[0])
    // No local state to reset
    s_resetPriorityStateFlag = true;
}

// Static flag to signal signal bar decay reset on next call
static bool s_resetSignalBarDecayFlag = false;

void PacketParser::resetSignalBarDecay() {
    s_resetSignalBarDecayFlag = true;
}

// Static flag to signal alert count tracker reset on next call
static bool s_resetAlertCountFlag = false;

void PacketParser::resetAlertCountTracker() {
    s_resetAlertCountFlag = true;
}

// Global display signal bar decay state
static uint8_t s_displayedSignalBars = 0;
static unsigned long s_lastSignalBarDropTime = 0;
static bool s_resetDisplaySignalDecayFlag = false;
constexpr unsigned long SIGNAL_BAR_DROP_INTERVAL_MS = 350;  // V1 drops ~3 bars/sec

void PacketParser::resetDisplaySignalDecay() {
    s_resetDisplaySignalDecayFlag = true;
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
        case PACKET_ID_RESP_USER_BYTES:     // 0x12 - User bytes response
            return true;  // Acknowledged, no further processing needed
        
        case PACKET_ID_VERSION: {           // 0x01 - Version response
            // Parse V1 firmware version from response
            // Payload format: [versionID][major][minor][rev1][rev2][ctrl]
            // versionID 'V' = main firmware version
            // Example: V 4 1 0 2 8 = version 4.1028
            if (length >= 12) {  // Full packet with 7-byte payload
                const uint8_t* payload = data + 5;
                char versionID = (char)payload[1];
                if (versionID == 'V') {
                    // Convert ASCII digits to integer version
                    // e.g., "4.1028" becomes 41028
                    char major = (char)payload[2];
                    char minor = (char)payload[3];
                    char rev1 = (char)payload[4];
                    char rev2 = (char)payload[5];
                    char ctrl = (char)payload[6];
                    
                    // Build version number: major * 10000 + minor * 1000 + rev1 * 100 + rev2 * 10 + ctrl
                    uint32_t version = 0;
                    if (major >= '0' && major <= '9') version += (major - '0') * 10000;
                    if (minor >= '0' && minor <= '9') version += (minor - '0') * 1000;
                    if (rev1 >= '0' && rev1 <= '9') version += (rev1 - '0') * 100;
                    if (rev2 >= '0' && rev2 <= '9') version += (rev2 - '0') * 10;
                    if (ctrl >= '0' && ctrl <= '9') version += (ctrl - '0');
                    
                    displayState.v1FirmwareVersion = version;
                    displayState.hasV1Version = true;
                    Serial.printf("[PacketParser] V1 firmware version: %c.%c%c%c%c (v%lu)\\n",
                                  major, minor, rev1, rev2, ctrl, version);
                }
            }
            return true;
        }
            
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
        displayState.hasVolumeData = true;  // Mark that we've received volume data
        
        // Consider muted if mute flag is set OR if main volume is zero
        if (displayState.mainVolume == 0) {
            displayState.muted = true;
        }
    }
    
    // If laser is detected via display data, set full signal bars
    // Laser alerts don't have granular strength - they're on/off
    if (arrow.laser) {
        displayState.signalBars = 6; // Full bars for laser
    }
    // Note: Don't clear alertCount here based on activeBands being BAND_NONE
    // During V1's blink cycle, image1 band bits toggle off momentarily (creating the blink)
    // This would incorrectly clear alerts during blink. Let parseAlertData() be source of truth
    // for alert count - it receives explicit alert count from V1's alert table packets.
    
    return true;
}

// Apply V1-style decay to displayed signal bars
// Instant rise, gradual fall (~3 bars/sec)
uint8_t PacketParser::applySignalBarDecay(uint8_t newBars) {
    // Check if reset was requested (e.g., on V1 disconnect)
    if (s_resetDisplaySignalDecayFlag) {
        s_displayedSignalBars = 0;
        s_lastSignalBarDropTime = 0;
        s_resetDisplaySignalDecayFlag = false;
    }
    
    unsigned long now = millis();
    
    if (newBars >= s_displayedSignalBars) {
        // Going up or same - instant response
        s_displayedSignalBars = newBars;
        s_lastSignalBarDropTime = now;
    } else {
        // Going down - limit drop rate to match V1
        unsigned long elapsed = now - s_lastSignalBarDropTime;
        uint8_t allowedDrops = elapsed / SIGNAL_BAR_DROP_INTERVAL_MS;
        
        if (allowedDrops > 0) {
            uint8_t targetBars = (s_displayedSignalBars > allowedDrops) 
                               ? (s_displayedSignalBars - allowedDrops) : 0;
            // Don't go below actual signal
            if (targetBars < newBars) targetBars = newBars;
            s_displayedSignalBars = targetBars;
            s_lastSignalBarDropTime = now;
        }
    }
    
    return s_displayedSignalBars;
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

// Decode V1's LED bitmap to bar count (0-8)
// V1 sends signal strength as consecutive bits from LSB: 1=1bar, 3=2bars, 7=3bars, etc.
uint8_t PacketParser::decodeLEDBitmap(uint8_t bitmap) const {
    switch (bitmap) {
        case 1:   return 1;
        case 3:   return 2;
        case 7:   return 3;
        case 15:  return 4;
        case 31:  return 5;
        case 63:  return 6;
        case 127: return 7;
        case 255: return 8;
        default:  return 0;  // No signal or invalid
    }
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
    // Match V1's signal bar decay rate
    static uint8_t lastBarsKa = 0, lastBarsK = 0, lastBarsX = 0;
    static unsigned long lastDropTimeKa = 0, lastDropTimeK = 0, lastDropTimeX = 0;
    constexpr unsigned long DROP_INTERVAL_MS = 300;  // Match V1's decay rate
    
    // Check if reset was requested (e.g., on V1 disconnect)
    if (s_resetSignalBarDecayFlag) {
        lastBarsKa = lastBarsK = lastBarsX = 0;
        lastDropTimeKa = lastDropTimeK = lastDropTimeX = 0;
        s_resetSignalBarDecayFlag = false;
    }
    
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

    // Byte0: high nibble = alert index (0-based), low nibble = alert count
    // The first alert sent (index 0) is the V1's priority alert
    uint8_t alertIndex = (payload[0] >> 4) & 0x0F;
    uint8_t receivedAlertCount = payload[0] & 0x0F;
    if (receivedAlertCount == 0) {
        alertCount = 0;
        chunkCount = 0;
        // Apply decay even when alerts clear - V1 bars fade down gradually
        displayState.signalBars = applySignalBarDecay(0);
        displayState.arrows = DIR_NONE;
        // Note: Don't clear displayState.activeBands here - let parseDisplayData() handle it
        // The display packet's image1 will show no bands when alerts truly clear
        displayState.muted = false;
        return true;
    }

    // Each alert row is 7-8 bytes in V1G2 captures; require at least 7
    if (length < 7) {
        return false;
    }

    // Track expected alert count - if it changes mid-assembly, reset to avoid stale data
    // This handles the case where alerts change while we're still collecting chunks
    static uint8_t lastExpectedCount = 0;
    
    // Check if reset was requested (e.g., on V1 disconnect)
    if (s_resetAlertCountFlag) {
        lastExpectedCount = 0;
        s_resetAlertCountFlag = false;
    }
    
    if (receivedAlertCount != lastExpectedCount) {
        // Alert count changed - discard any partial assembly and start fresh
        chunkCount = 0;
        lastExpectedCount = receivedAlertCount;
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
    // Note: Don't reset displayState.activeBands here - let parseDisplayData() be source of truth
    // for what bands are visually shown (including blink behavior). We just extract alert details.
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
            
            // V1 sends signal bars as LED bitmap in bytes 6 (front) and 7 (rear)
            // Bitmap: 1=1bar, 3=2bars, 7=3bars, 15=4bars, 31=5bars, 63=6bars, 127=7bars, 255=8bars
            // Fall back to raw RSSI mapping if bitmap is 0 (shouldn't happen with valid alerts)
            uint8_t frontLEDs = decodeLEDBitmap(a[6]);
            uint8_t rearLEDs = decodeLEDBitmap(a[7]);
            
            // Use LED bitmap if valid, otherwise fall back to raw RSSI mapping
            alert.frontStrength = (frontLEDs > 0) ? frontLEDs : mapStrengthToBars(band, a[3]);
            alert.rearStrength = (rearLEDs > 0) ? rearLEDs : mapStrengthToBars(band, a[4]);
            
            alert.frequency = (band == BAND_LASER) ? 0 : combineMSBLSB(a[1], a[2]); // MHz
            alert.isValid = true;

            // Note: We no longer update displayState.activeBands here
            // The display packet (image1) is authoritative for band indicators
            anyMuted |= ((bandArrow & 0x10) != 0);
        }
    }

    // Combine alert mute bits with display packet's mute flag
    // V1 logic mute shows in display packet even if alert entries don't have mute bit
    displayState.muted = displayState.muted || anyMuted;

    if (alertCount > 0) {
        // V1 sends alerts with index 0 being the priority alert
        // The first chunk received has the priority index in its high nibble
        // Store that for getPriorityAlert() to use
        displayState.v1PriorityIndex = (alertChunks[0][0] >> 4) & 0x0F;
        
        // Find MAX signal strength across ALL alerts for signal bar display
        uint8_t maxSignal = 0;
        for (size_t i = 0; i < alertCount; ++i) {
            uint8_t sig = std::max(alerts[i].frontStrength, alerts[i].rearStrength);
            if (sig > maxSignal) {
                maxSignal = sig;
            }
        }
        // Apply decay - instant rise, gradual fall like V1
        displayState.signalBars = applySignalBarDecay(maxSignal);
        
        // Set priority arrow from V1's reported priority alert (index 0 in our array)
        // Alerts are stored in the order received, with priority (index 0) first
        displayState.priorityArrow = alerts[0].direction;
        
        // Note: displayState.arrows already set by parseDisplayData() - shows ALL active directions
    } else {
        // Apply decay even when alerts clear
        displayState.signalBars = applySignalBarDecay(0);
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

    // V1 sends alerts in priority order - index 0 is always the priority alert
    // The V1 already handles priority determination including:
    // - Laser always being highest priority
    // - Signal strength comparison
    // - Its own hysteresis logic
    // Trust the V1's priority decision rather than recomputing it ourselves
    return alerts[0];
}

/**
 * ESP Packet Parser for V1 Gen2
 *
 * The V1G2 packets are framed with 0xAA ... 0xAB. Packet ID lives at byte 3,
 * payload begins at byte 5 (after dest/src/id/len).
 * 
 * Protocol reference: v1g2-t4s3 (Kenny's original ESP32/T4 implementation)
 * This code maintains compatibility with the original Valentine Research protocol.
 * Packet IDs: 0x31 = display/update, 0x43 = alert table entries.
 */

#include "packet_parser.h"
#include "../include/config.h"
#include "../include/band_utils.h"
#include <algorithm>
#include "debug_logger.h"

#ifndef UNIT_TEST
#include "perf_metrics.h"
#define PARSER_PERF_INC(counter) PERF_INC(counter)
#define PARSER_TRACE_ENABLED() (perfDebugEnabled)
#else
#define PARSER_PERF_INC(counter) do { } while (0)
#define PARSER_TRACE_ENABLED() (false)
#endif

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

const char* bandToString(Band band) {
    return bandName(band);
}

const char* directionToString(Direction dir) {
    switch (dir) {
        case DIR_FRONT: return "Front";
        case DIR_SIDE:  return "Side";
        case DIR_REAR:  return "Rear";
        default:        return "None";
    }
}

// Decode V1's 7-segment bogey counter byte to a character
// Based on V1 protocol - shows J=Junk, P=Photo, volume digits, L=Logic, etc.
// Bit 7 = decimal point (returned separately)
// Returns: character to display, hasDot = true if decimal point should show
char decodeBogeyCounterByte(uint8_t bogeyImage, bool& hasDot) {
    hasDot = (bogeyImage & 0x80) != 0;  // Bit 7 = decimal point
    
    switch (bogeyImage & 0x7F) {
        case 6:   return '1';
        case 7:   return '7';
        case 24:  return '&';  // Little L (logic mode)
        case 28:  return 'u';
        case 30:  return 'J';  // Junk
        case 56:  return 'L';  // Logic
        case 57:  return 'C';
        case 62:  return 'U';
        case 63:  return '0';
        case 73:  return '#';  // LASER bars
        case 79:  return '3';
        case 88:  return 'c';
        case 91:  return '2';
        case 94:  return 'd';
        case 102: return '4';
        case 109: return '5';
        case 111: return '9';
        case 113: return 'F';
        case 115: return 'P';  // Photo radar
        case 119: return 'A';
        case 121: return 'E';
        case 124: return 'b';
        case 125: return '6';
        case 127: return '8';
        default:  return ' ';  // Blank/unknown
    }
}

} // namespace

PacketParser::PacketParser()
    : alertCount(0) {
    alertChunkPresent.fill(false);
    alertChunkCountTag.fill(0);
    alertIndexModeByCount.fill(AlertIndexMode::Unknown);
}

void PacketParser::resetAlertAssembly() {
    // Drop any partially collected alert rows without altering display state
    clearAlertCache();
}

void PacketParser::clearAlertCache() {
    alertChunkPresent.fill(false);
    alertChunkCountTag.fill(0);
    alertIndexModeByCount.fill(AlertIndexMode::Unknown);
}

void PacketParser::clearAlertCacheForCount(uint8_t count) {
    if (count == 0 || count > MAX_ALERTS) {
        return;
    }
    for (size_t i = 0; i < MAX_ALERTS; ++i) {
        if (alertChunkPresent[i] && alertChunkCountTag[i] == count) {
            alertChunkPresent[i] = false;
            alertChunkCountTag[i] = 0;
        }
    }
    alertIndexModeByCount[count] = AlertIndexMode::Unknown;
}



// Static flag to signal alert count tracker reset on next parseAlertData() call
// Used on V1 disconnect to ensure clean state on reconnect
static bool s_resetAlertCountFlag = false;

void PacketParser::resetAlertCountTracker() {
    s_resetAlertCountFlag = true;
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
    // payload[0] = bogey counter image1 (7-segment: 0-9, J=Junk, P=Photo, etc.)
    // payload[1] = bogey counter image2 (unused for now)
    // payload[2] = LED bar bitmap
    // payload[3] = image1 (currently ON bits - bands/arrows)
    // payload[4] = image2 (steady/NOT-flashing bits)
    // payload[5] = auxData0 (status bits: soft/system/euro/display-active)
    // payload[6] = auxData1 (mode/bluetooth flags)
    // payload[7] = auxData2 (volume: upper=main, lower=mute)
    // Bits in image1 but NOT in image2 = FLASHING
    // V1 hardware handles the actual blink animation internally - we must do the same
    
    // Decode bogey counter byte - shows what V1's display shows (J, P, volume, etc.)
    uint8_t bogeyByte = payload[0];
    bool hasDot = false;
    char bogeyChar = decodeBogeyCounterByte(bogeyByte, hasDot);
    displayState.bogeyCounterByte = bogeyByte;
    displayState.bogeyCounterChar = bogeyChar;
    displayState.bogeyCounterDot = hasDot;
    
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
    
    // V1 sends LED bar state directly in the display packet at payload[2]
    // This is the authoritative signal strength from V1's own display
    // Bitmap: 0x01=1bar, 0x03=2bars, 0x07=3bars, 0x0F=4bars, 0x1F=5bars, 0x3F=6bars
    // Pass through directly - no filtering needed (matches arrow behavior)
    if (length > 2) {
        uint8_t ledBitmap = payload[2];
        displayState.signalBars = decodeLEDBitmap(ledBitmap);
    }

    // AndroidESPLibrary2 treats display aux0 as status flags (soft mute/system/euro/display active),
    // not as a direct alert-table priority index. Priority selection is resolved from alert rows.
    
    // If laser is detected via display data, set full signal bars
    // Laser alerts don't have granular strength - they're on/off
    if (arrow.laser) {
        displayState.signalBars = 6; // Full bars for laser
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

// Decode V1's LED bitmap to bar count (0-8)
// V1 sends signal strength as consecutive bits from LSB: 1=1bar, 3=2bars, 7=3bars, etc.
// But if that doesn't match, try counting set bits (popcount)
uint8_t PacketParser::decodeLEDBitmap(uint8_t bitmap) const {
    // First try the expected bitmap pattern
    switch (bitmap) {
        case 1:   return 1;
        case 3:   return 2;
        case 7:   return 3;
        case 15:  return 4;
        case 31:  return 5;
        case 63:  return 6;
        case 127: return 7;
        case 255: return 8;
        case 0:   return 0;
        default:
            // Not a standard bitmap - try popcount (number of set bits)
            // This handles cases where V1 sends non-standard patterns
            return __builtin_popcount(bitmap);
    }
}

uint8_t PacketParser::mapStrengthToBars(Band band, uint8_t raw) const {
    // Convert raw RSSI to 0-6 bars for individual alert strength tracking
    // Note: Main display signal bars come from V1's LED bitmap in parseDisplayData()
    // This is used for per-alert strength in the alerts array
    
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

    // Map raw RSSI to bars using threshold table
    uint8_t bars = 0;
    for (uint8_t i = 0; i < 7; ++i) {
        if (raw <= table[i]) {
            bars = i;
            break;
        }
    }
    if (bars == 0 && raw > table[0]) {
        bars = 6;
    }

    return bars;
}

bool PacketParser::parseAlertData(const uint8_t* payload, size_t length) {
    if (!payload || length < 1) {
        return false;
    }

    // Byte0: high nibble = alert index, low nibble = alert count.
    // Official Android/iOS libraries assemble alert tables by index 1..count.
    // Keep a zero-based fallback path for compatibility with legacy captures.
    uint8_t alertIndex = (payload[0] >> 4) & 0x0F;
    uint8_t receivedAlertCount = payload[0] & 0x0F;
    if (receivedAlertCount == 0) {
        alertCount = 0;
        clearAlertCache();
        // DON'T clear signalBars here - parseDisplayData reads V1's LED bitmap
        displayState.arrows = DIR_NONE;
        displayState.muted = false;
        displayState.hasJunkAlert = false;
        displayState.hasPhotoAlert = false;
        return true;
    }

    // Each alert row is 7-8 bytes in V1G2 captures; require at least 7
    if (length < 7) {
        return false;
    }

    // Check if reset was requested (e.g., on V1 disconnect)
    if (s_resetAlertCountFlag) {
        clearAlertCache();
        s_resetAlertCountFlag = false;
    }

    if (receivedAlertCount > MAX_ALERTS) {
        // Invalid table size - drop and wait for next valid row.
        return false;
    }

    // Keep a per-count index-mode hint so we can follow whichever index base the
    // V1 stream uses without discarding partial rows when count changes.
    AlertIndexMode mode = alertIndexModeByCount[receivedAlertCount];
    if (mode == AlertIndexMode::Unknown) {
        const bool oneBasedCandidate = (alertIndex >= 1 && alertIndex <= receivedAlertCount);
        const bool zeroBasedCandidate = (alertIndex < receivedAlertCount);
        if (alertIndex == 0 && zeroBasedCandidate) {
            mode = AlertIndexMode::ZeroBased;
        } else if (alertIndex == receivedAlertCount && oneBasedCandidate) {
            mode = AlertIndexMode::OneBased;
        } else if (oneBasedCandidate && !zeroBasedCandidate) {
            mode = AlertIndexMode::OneBased;
        } else if (!oneBasedCandidate && zeroBasedCandidate) {
            mode = AlertIndexMode::ZeroBased;
        } else if (oneBasedCandidate) {
            // Ambiguous interior rows (e.g., idx=1 when count=2) default to
            // one-based, matching VR library table assembly.
            mode = AlertIndexMode::OneBased;
        }
        alertIndexModeByCount[receivedAlertCount] = mode;
    }

    size_t slot = MAX_ALERTS;
    if (mode == AlertIndexMode::OneBased) {
        if (alertIndex >= 1 && alertIndex <= receivedAlertCount) {
            slot = static_cast<size_t>(alertIndex - 1);
        }
    } else if (mode == AlertIndexMode::ZeroBased) {
        if (alertIndex < receivedAlertCount) {
            slot = static_cast<size_t>(alertIndex);
        }
    }

    // If row index is invalid for this table mode, drop it and keep existing cache.
    if (slot >= MAX_ALERTS) {
        if (PARSER_TRACE_ENABLED()) {
            Serial.printf("[AlertAsm] drop idx=%u cnt=%u mode=%u (invalid slot)\n",
                          static_cast<unsigned>(alertIndex),
                          static_cast<unsigned>(receivedAlertCount),
                          static_cast<unsigned>(mode));
        }
        return true;
    }

    const bool replacingRow =
        alertChunkPresent[slot] && (alertChunkCountTag[slot] == receivedAlertCount);
    alertChunkPresent[slot] = true;
    alertChunkCountTag[slot] = receivedAlertCount;
    std::array<uint8_t, 8>& chunk = alertChunks[slot];
    chunk.fill(0);
    size_t copyLen = std::min<size_t>(8, length);
    for (size_t i = 0; i < copyLen; ++i) {
        chunk[i] = payload[i];
    }

    size_t rowsForCount = 0;
    for (size_t i = 0; i < receivedAlertCount; ++i) {
        if (alertChunkPresent[i] && alertChunkCountTag[i] == receivedAlertCount) {
            ++rowsForCount;
        }
    }

    if (PARSER_TRACE_ENABLED()) {
        Serial.printf(
            "[AlertRow] idx=%u cnt=%u slot=%u rows=%u/%u mode=%u repl=%u raw0=0x%02X f=%u bandArrow=0x%02X aux0=0x%02X\n",
                      static_cast<unsigned>(alertIndex),
                      static_cast<unsigned>(receivedAlertCount),
                      static_cast<unsigned>(slot),
                      static_cast<unsigned>(rowsForCount),
                      static_cast<unsigned>(receivedAlertCount),
                      static_cast<unsigned>(mode),
                      replacingRow ? 1u : 0u,
                      static_cast<unsigned>(payload[0]),
                      static_cast<unsigned>(combineMSBLSB(payload[1], payload[2])),
                      static_cast<unsigned>(payload[5]),
                      static_cast<unsigned>(payload[6]));
    }

    // Rolling per-index cache: only publish when every row 0..count-1 is present
    // for the same table count.
    if (rowsForCount < receivedAlertCount) {
        if (PARSER_TRACE_ENABLED()) {
            Serial.printf("[AlertAsm] partial rows=%u/%u mode=%u\n",
                          static_cast<unsigned>(rowsForCount),
                          static_cast<unsigned>(receivedAlertCount),
                          static_cast<unsigned>(mode));
        }
        return true;
    }

    std::array<AlertData, MAX_ALERTS> nextAlerts{};
    size_t nextAlertCount = 0;
    bool anyMuted = false;
    bool anyJunk = false;
    bool anyPhoto = false;

    // We have enough rows; validate all required row slots for this count and
    // decode into a new table buffer before publishing.
    for (size_t i = 0; i < receivedAlertCount && i < MAX_ALERTS; ++i) {
        if (!alertChunkPresent[i] || alertChunkCountTag[i] != receivedAlertCount) {
            if (PARSER_TRACE_ENABLED()) {
                Serial.printf("[AlertAsm] missing row slot=%u cnt=%u rows=%u\n",
                              static_cast<unsigned>(i),
                              static_cast<unsigned>(receivedAlertCount),
                              static_cast<unsigned>(rowsForCount));
            }
            return true;
        }

        const auto& a = alertChunks[i];
        uint8_t bandArrow = a[5];  // band + arrow + mute (matches captures: 0x24 for K/front)
        uint8_t aux0 = a[6];       // aux0: bit7=priority, bit6=junk, low nibble=photo type

        Band band = decodeBand(bandArrow);
        Direction dir = decodeDirection(bandArrow);
        bool isPriority = (aux0 & 0x80) != 0;  // JB: (aux0 & 128) != 0
        // Match official Android/iOS library behavior:
        // junk flag is valid on V1 4.1032+, photo type on 4.1037+.
        const bool junkSupported =
            !displayState.hasV1Version || (displayState.v1FirmwareVersion >= 41032);
        const bool photoSupported =
            !displayState.hasV1Version || (displayState.v1FirmwareVersion >= 41037);
        bool isJunk = junkSupported && ((aux0 & 0x40) != 0);
        uint8_t photoType = photoSupported ? static_cast<uint8_t>(aux0 & 0x0F) : 0;

        if (nextAlertCount < MAX_ALERTS) {
            AlertData& alert = nextAlerts[nextAlertCount++];
            alert.band = band;
            alert.direction = dir;
            alert.isPriority = isPriority;
            alert.isJunk = isJunk;
            alert.photoType = photoType;
            
            // Bytes 3 and 4 are raw RSSI values for front/rear antennas
            // Use our RSSI-to-bars mapping with band-specific thresholds
            alert.frontStrength = mapStrengthToBars(band, a[3]);
            alert.rearStrength = mapStrengthToBars(band, a[4]);
            
            alert.frequency = (band == BAND_LASER) ? 0 : combineMSBLSB(a[1], a[2]); // MHz
            alert.isValid = true;

            anyMuted |= ((bandArrow & 0x10) != 0);
            anyJunk |= isJunk;
            anyPhoto |= (photoType != 0);
        }
    }

    alerts = nextAlerts;
    alertCount = nextAlertCount;

    if (alertCount > 0) {
        // Priority resolution order:
        // 1) Per-row aux0 bit7 (matches AndroidESPLibrary2 AlertData::isPriority())
        // 2) First usable alert
        // 3) Entry 0 as last-resort safety fallback
        auto isUsableAlert = [this](int idx) -> bool {
            if (idx < 0 || idx >= static_cast<int>(alertCount)) return false;
            const AlertData& a = alerts[static_cast<size_t>(idx)];
            if (!a.isValid || a.band == BAND_NONE) return false;
            if (a.band != BAND_LASER && a.frequency == 0) return false;
            return true;
        };

        int priorityFromRowFlag = -1;
        for (size_t i = 0; i < alertCount; ++i) {
            if (alerts[i].isPriority) {
                priorityFromRowFlag = static_cast<int>(i);
                break;
            }
        }

        enum class PrioritySource : uint8_t { RowFlag = 2, FirstUsable = 3, FirstEntry = 4 };

        int priorityIdx = -1;
        PrioritySource source = PrioritySource::FirstEntry;
        if (isUsableAlert(priorityFromRowFlag)) {
            priorityIdx = priorityFromRowFlag;
            source = PrioritySource::RowFlag;
        }
        if (priorityIdx < 0) {
            for (size_t i = 0; i < alertCount; ++i) {
                if (isUsableAlert(static_cast<int>(i))) {
                    priorityIdx = static_cast<int>(i);
                    source = PrioritySource::FirstUsable;
                    break;
                }
            }
        }
        if (priorityIdx < 0) {
            priorityIdx = 0;
            source = PrioritySource::FirstEntry;
        }

        switch (source) {
            case PrioritySource::RowFlag:
                PARSER_PERF_INC(prioritySelectRowFlag);
                break;
            case PrioritySource::FirstUsable:
                PARSER_PERF_INC(prioritySelectFirstUsable);
                break;
            case PrioritySource::FirstEntry:
            default:
                PARSER_PERF_INC(prioritySelectFirstEntry);
                break;
        }

        if (!isUsableAlert(priorityIdx)) {
            PARSER_PERF_INC(prioritySelectInvalidChosen);
        }

        if (PARSER_TRACE_ENABLED()) {
            const char* src =
                (source == PrioritySource::RowFlag) ? "rowFlag" :
                (source == PrioritySource::FirstUsable) ? "firstUsable" : "firstEntry";
            Serial.printf("[AlertPri] src=%s idx=%u cnt=%u rowFlagIdx=%d\n",
                          src,
                          static_cast<unsigned>(priorityIdx),
                          static_cast<unsigned>(alertCount),
                          priorityFromRowFlag);
        }
        
        displayState.v1PriorityIndex = priorityIdx;
        displayState.priorityArrow = alerts[priorityIdx].direction;
    }

    // Combine alert mute bits with display packet's mute flag.
    displayState.muted = displayState.muted || anyMuted;
    displayState.hasJunkAlert = anyJunk;
    displayState.hasPhotoAlert = anyPhoto;

    if (debugLogger.isEnabledFor(DebugLogCategory::Alerts)) {
        static bool hasLast = false;
        static size_t lastCount = 0;
        static Band lastBand = BAND_NONE;
        static Direction lastDir = DIR_NONE;
        static uint32_t lastFreq = 0;
        static uint8_t lastFront = 0;
        static uint8_t lastRear = 0;
        static bool lastMuted = false;

        Band priBand = BAND_NONE;
        Direction priDir = DIR_NONE;
        uint32_t priFreq = 0;
        uint8_t priFront = 0;
        uint8_t priRear = 0;

        if (alertCount > 0) {
            size_t idx = std::min<size_t>(displayState.v1PriorityIndex, alertCount - 1);
            const AlertData& pri = alerts[idx];
            priBand = pri.band;
            priDir = pri.direction;
            priFreq = pri.frequency;
            priFront = pri.frontStrength;
            priRear = pri.rearStrength;
        }

        bool changed = !hasLast ||
                       alertCount != lastCount ||
                       priBand != lastBand ||
                       priDir != lastDir ||
                       priFreq != lastFreq ||
                       priFront != lastFront ||
                       priRear != lastRear ||
                       displayState.muted != lastMuted;

        if (changed) {
            debugLogger.logf(
                DebugLogCategory::Alerts,
                "[Alerts] count=%u pri=%s dir=%s freq=%u front=%u rear=%u muted=%s",
                static_cast<unsigned>(alertCount),
                bandToString(priBand),
                directionToString(priDir),
                static_cast<unsigned>(priFreq),
                static_cast<unsigned>(priFront),
                static_cast<unsigned>(priRear),
                displayState.muted ? "true" : "false");

            hasLast = true;
            lastCount = alertCount;
            lastBand = priBand;
            lastDir = priDir;
            lastFreq = priFreq;
            lastFront = priFront;
            lastRear = priRear;
            lastMuted = displayState.muted;
        }
    }

    // Match VR AlertDataProcessor behavior: clear per-count rows after a complete
    // table publish so the next table is assembled from fresh rows.
    clearAlertCacheForCount(receivedAlertCount);
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

    // displayState.v1PriorityIndex is resolved in parseAlertData():
    // alert-row aux0 bit7 first, then safety fallbacks.
    uint8_t idx = displayState.v1PriorityIndex;
    if (idx < alertCount) {
        return alerts[idx];
    }
    return alerts[0];  // Fallback
}

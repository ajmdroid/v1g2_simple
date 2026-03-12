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
    alertChunkRxMs.fill(0);
    alertTableFirstSeenMs.fill(0);
}

bool PacketParser::parse(const uint8_t* data, size_t length) {
    if (!data || length < 7) {
        return false;
    }
    if (data[0] != ESP_PACKET_START || data[length - 1] != ESP_PACKET_END) {
        return false;
    }

    uint8_t packetId = data[3];
    switch (packetId) {
        case PACKET_ID_WRITE_USER_BYTES:
        case PACKET_ID_TURN_OFF_DISPLAY:
        case PACKET_ID_TURN_ON_DISPLAY:
        case PACKET_ID_MUTE_ON:
        case PACKET_ID_MUTE_OFF:
        case 0x36:
        case PACKET_ID_REQ_WRITE_VOLUME:
        case PACKET_ID_RESP_USER_BYTES:
            break;
        default:
            if (!validatePacket(data, length)) {
                return false;
            }
            break;
    }

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

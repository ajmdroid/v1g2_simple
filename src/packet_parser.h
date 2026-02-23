/**
 * ESP Packet Parser for V1 Gen2
 * Decodes display data and alert data packets
 */

#ifndef PACKET_PARSER_H
#define PACKET_PARSER_H

#include <Arduino.h>
#include <array>
#include <vector>

// Radar bands
enum Band {
    BAND_NONE   = 0,
    BAND_LASER  = 1 << 0,
    BAND_KA     = 1 << 1,
    BAND_K      = 1 << 2,
    BAND_X      = 1 << 3
};

// Direction indicators
enum Direction {
    DIR_NONE    = 0,
    DIR_FRONT   = 1,
    DIR_SIDE    = 2,
    DIR_REAR    = 4
};

// Alert data structure
struct AlertData {
    Band band;
    Direction direction;
    uint8_t frontStrength;  // 0-6 (V1 Gen2 uses 6 bars)
    uint8_t rearStrength;   // 0-6
    uint32_t frequency;     // MHz
    bool isValid;
    bool isPriority;        // aux0 bit 7 - V1's priority flag
    bool isJunk;            // aux0 bit 6 - junked alert
    uint8_t photoType;      // aux0 bits 0..3 - photo type (V4.1037+)
    uint8_t rawBandBits;    // bandArrow low 5 bits (VR-style raw band field)
    bool isKu;              // True when rawBandBits resolves to Ku (0x10)
    
    AlertData() : band(BAND_NONE), direction(DIR_NONE), 
                  frontStrength(0), rearStrength(0), 
                  frequency(0), isValid(false), isPriority(false),
                  isJunk(false), photoType(0), rawBandBits(0), isKu(false) {}
};

// Display state
struct DisplayState {
    uint8_t activeBands;    // Bitmap of active bands
    Direction arrows;       // Bitmap of arrow directions (from display packet - all active)
    Direction priorityArrow; // Arrow from V1's priority alert (alerts[0])
    uint8_t signalBars;     // 0-8 signal strength (from V1's LED bitmap)
    bool muted;
    bool systemTest;
    char modeChar;
    bool hasMode;
    bool displayOn;         // True if main display is ON (not dark)
    bool hasDisplayOn;      // True if we've seen explicit on/off ack
    uint8_t flashBits;      // Blink state for arrows (from display packet)
    uint8_t bandFlashBits;  // Blink state for bands (L=0x01, Ka=0x02, K=0x04, X=0x08)
    uint8_t mainVolume;     // Main volume 0-9
    uint8_t muteVolume;     // Muted volume 0-9
    uint32_t v1FirmwareVersion;  // V1 firmware version as integer (e.g., 41028 for 4.1028)
    bool hasV1Version;      // True if we've received version from V1
    bool hasVolumeData;     // True if we've received volume data in display packet
    uint8_t v1PriorityIndex; // Resolved priority alert index for current table (0-based)
    uint8_t bogeyCounterByte;  // Raw 7-segment byte from V1 display (for decoding J, P, volume, etc)
    char bogeyCounterChar;     // Decoded character from bogeyCounterByte
    bool bogeyCounterDot;      // Decimal point from bogeyCounterByte (bit 7)
    bool hasJunkAlert;         // True if any alert row has aux0 junk bit set
    bool hasPhotoAlert;        // True if any alert row has photo type > 0
    
    DisplayState() : activeBands(BAND_NONE), arrows(DIR_NONE), priorityArrow(DIR_NONE),
                     signalBars(0), muted(false), systemTest(false),
                     modeChar(0), hasMode(false), displayOn(true), hasDisplayOn(false),
                     flashBits(0), bandFlashBits(0), mainVolume(0), muteVolume(0),
                     v1FirmwareVersion(0), hasV1Version(false), hasVolumeData(false),
                     v1PriorityIndex(0), bogeyCounterByte(0), bogeyCounterChar('0'), 
                     bogeyCounterDot(false), hasJunkAlert(false), hasPhotoAlert(false) {}
    
    // Check if V1 firmware supports volume display
    // Show volume if we've received volume data OR confirmed firmware version 4.1028+
    bool supportsVolume() const { return hasVolumeData || (hasV1Version && v1FirmwareVersion >= 41028); }
};

class PacketParser {
public:
    static constexpr size_t MAX_ALERTS = 15;  // V1 spec supports up to 15 simultaneous alerts

    PacketParser();
    
    // Parse incoming ESP packet
    bool parse(const uint8_t* data, size_t length);
    
    // Get current display state
    const DisplayState& getDisplayState() const { return displayState; }
    
    // Get resolved priority alert (follows V1 priority signal)
    AlertData getPriorityAlert() const;

    // Get a renderable priority alert (valid band + usable frequency semantics).
    // Returns true and writes to out when a renderable alert exists.
    bool getRenderablePriorityAlert(AlertData& out) const;
    
    // Get all alerts
    const std::array<AlertData, MAX_ALERTS>& getAllAlerts() const { return alerts; }
    
    // Get number of active alerts
    size_t getAlertCount() const { return alertCount; }

    // Check if there are active alerts
    // Only check alertCount - display state can lag behind
    bool hasAlerts() const { return alertCount > 0; }
    
    // Check if V1 firmware supports volume display
    // Show volume if we've received volume data OR confirmed firmware version 4.1028+
    bool supportsVolume() const { return displayState.hasVolumeData || (displayState.hasV1Version && displayState.v1FirmwareVersion >= 41028); }

    // Clear any partially assembled alert chunks (used when we re-request alert data)
    void resetAlertAssembly();
    

    

    
    // Reset alert count tracker (call on V1 disconnect to clear stale assembly state)
    static void resetAlertCountTracker();

private:
    DisplayState displayState;
    std::array<AlertData, MAX_ALERTS> alerts;
    size_t alertCount;
    std::array<std::array<uint8_t, 8>, MAX_ALERTS> alertChunks;  // full 8-byte alert rows
    std::array<bool, MAX_ALERTS> alertChunkPresent;
    std::array<uint8_t, MAX_ALERTS> alertChunkCountTag;
    std::array<uint32_t, MAX_ALERTS> alertChunkRxMs;
    std::array<uint32_t, MAX_ALERTS + 1> alertTableFirstSeenMs;

    // Packet parsing helpers
    bool parseDisplayData(const uint8_t* payload, size_t length);
    bool parseAlertData(const uint8_t* payload, size_t length);
    bool validatePacket(const uint8_t* data, size_t length);
    void clearAlertCache();
    void clearAlertCacheForCount(uint8_t count);
    
    // Data extraction
    Band decodeBand(uint8_t bandArrow) const;
    Direction decodeDirection(uint8_t bandArrow) const;
    uint8_t decodeLEDBitmap(uint8_t bitmap) const;  // Convert V1 LED bitmap to bar count (1-8)
    uint8_t mapStrengthToBars(Band band, uint8_t raw) const;  // Fallback if LED bitmap unavailable
    void decodeMode(const uint8_t* payload, size_t length);
};

#endif // PACKET_PARSER_H

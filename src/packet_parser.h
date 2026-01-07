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
    
    AlertData() : band(BAND_NONE), direction(DIR_NONE), 
                  frontStrength(0), rearStrength(0), 
                  frequency(0), isValid(false) {}
};

// Display state
struct DisplayState {
    uint8_t activeBands;    // Bitmap of active bands
    Direction arrows;       // Bitmap of arrow directions
    uint8_t signalBars;     // 0-6 signal strength (V1 Gen2)
    bool muted;
    bool systemTest;
    char modeChar;
    bool hasMode;
    bool displayOn;         // True if main display is ON (not dark)
    bool hasDisplayOn;      // True if we've seen explicit on/off ack
    
    DisplayState() : activeBands(BAND_NONE), arrows(DIR_NONE), 
                     signalBars(0), muted(false), systemTest(false),
                     modeChar(0), hasMode(false), displayOn(true), hasDisplayOn(false) {}
};

class PacketParser {
public:
    static constexpr size_t MAX_ALERTS = 10;

    PacketParser();
    
    // Parse incoming ESP packet
    bool parse(const uint8_t* data, size_t length);
    
    // Get current display state
    const DisplayState& getDisplayState() const { return displayState; }
    
    // Get priority alert (highest strength)
    AlertData getPriorityAlert() const;
    
    // Get all alerts
    const std::array<AlertData, MAX_ALERTS>& getAllAlerts() const { return alerts; }
    
    // Get number of active alerts
    size_t getAlertCount() const { return alertCount; }

    // Check if there are active alerts
    // Check both alertCount AND displayState.activeBands to handle timing gaps
    // between alert data packets and display data packets
    bool hasAlerts() const { return alertCount > 0 || displayState.activeBands != BAND_NONE; }

private:
    DisplayState displayState;
    std::array<AlertData, MAX_ALERTS> alerts;
    size_t alertCount;
    std::array<std::array<uint8_t, 7>, MAX_ALERTS> alertChunks;
    size_t chunkCount;
    
    // Packet parsing helpers
    bool parseDisplayData(const uint8_t* payload, size_t length);
    bool parseAlertData(const uint8_t* payload, size_t length);
    bool validatePacket(const uint8_t* data, size_t length);
    
    // Data extraction
    Band decodeBand(uint8_t bandArrow) const;
    Direction decodeDirection(uint8_t bandArrow) const;
    uint8_t mapStrengthToBars(Band band, uint8_t raw) const;
    void decodeMode(const uint8_t* payload, size_t length);
};

#endif // PACKET_PARSER_H

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
    uint8_t frontStrength;  // 0-8
    uint8_t rearStrength;   // 0-8
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
    uint8_t signalBars;     // 0-8 signal strength
    bool muted;
    bool systemTest;
    char modeChar;
    bool hasMode;
    
    DisplayState() : activeBands(BAND_NONE), arrows(DIR_NONE), 
                     signalBars(0), muted(false), systemTest(false),
                     modeChar(0), hasMode(false) {}
};

class PacketParser {
public:
    PacketParser();
    
    // Parse incoming ESP packet
    bool parse(const uint8_t* data, size_t length);
    
    // Get current display state
    DisplayState getDisplayState() const { return displayState; }
    
    // Get priority alert (highest strength)
    AlertData getPriorityAlert() const;
    
    // Get all alerts
    const std::vector<AlertData>& getAllAlerts() const { return alerts; }
    
    // Check if there are active alerts
    bool hasAlerts() const { return !alerts.empty(); }

private:
    DisplayState displayState;
    std::vector<AlertData> alerts;
    std::vector<std::array<uint8_t, 7>> alertChunks;
    
    // Packet parsing helpers
    bool parseDisplayData(const uint8_t* payload, size_t length);
    bool parseAlertData(const uint8_t* payload, size_t length);
    bool validatePacket(const uint8_t* data, size_t length);
    uint8_t calculateChecksum(const uint8_t* data, size_t length);
    
    // Data extraction
    Band decodeBand(uint8_t bandArrow) const;
    Direction decodeDirection(uint8_t bandArrow) const;
    uint8_t mapStrengthToBars(Band band, uint8_t raw) const;
    void decodeMode(const uint8_t* payload, size_t length);
};

#endif // PACKET_PARSER_H

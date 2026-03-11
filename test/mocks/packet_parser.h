// Mock packet_parser.h for native unit testing
// Uses same include guard as real packet_parser.h to prevent conflicts
#ifndef PACKET_PARSER_H
#define PACKET_PARSER_H

#include <cstdint>
#include <vector>
#include <algorithm>

// Band constants
enum Band {
    BAND_NONE   = 0,
    BAND_LASER  = 1 << 0,
    BAND_KA     = 1 << 1,
    BAND_K      = 1 << 2,
    BAND_X      = 1 << 3
};

// Direction constants
enum Direction {
    DIR_NONE    = 0,
    DIR_FRONT   = 1,
    DIR_SIDE    = 2,
    DIR_REAR    = 4
};

// Alert data structure
struct AlertData {
    Band band = BAND_NONE;
    Direction direction = DIR_NONE;
    uint8_t frontStrength = 0;
    uint8_t rearStrength = 0;
    uint32_t frequency = 0;
    bool isValid = false;
    bool isPriority = false;
    uint8_t photoType = 0;
    uint8_t rawBandBits = 0;
    bool isKu = false;
    
    static AlertData create(Band b, Direction d, uint8_t front, uint8_t rear, 
                           uint32_t freq, bool valid = true, bool priority = false) {
        AlertData a;
        a.band = b;
        a.direction = d;
        a.frontStrength = front;
        a.rearStrength = rear;
        a.frequency = freq;
        a.isValid = valid;
        a.isPriority = priority;
        return a;
    }
};

// Display state structure
struct DisplayState {
    uint8_t activeBands = BAND_NONE;
    Direction arrows = DIR_NONE;
    Direction priorityArrow = DIR_NONE;
    uint8_t signalBars = 0;
    bool muted = false;
    bool systemTest = false;
    char modeChar = 0;
    bool hasMode = false;
    bool displayOn = true;
    bool hasDisplayOn = false;
    uint8_t flashBits = 0;
    uint8_t bandFlashBits = 0;
    uint8_t mainVolume = 5;
    uint8_t muteVolume = 0;
    uint32_t v1FirmwareVersion = 0;
    bool hasV1Version = false;
    bool hasVolumeData = false;
    uint8_t v1PriorityIndex = 0;
    uint8_t bogeyCounterByte = 0;
    char bogeyCounterChar = '0';
    bool bogeyCounterDot = false;
    bool hasPhotoAlert = false;
};

/**
 * Mock PacketParser - controllable state for testing
 */
class PacketParser {
public:
    static constexpr size_t MAX_ALERTS = 15;

    // Test-controllable state
    DisplayState state;
    std::vector<AlertData> alerts;
    AlertData priorityAlert;
    bool hasAlertsFlag = false;
    int parseCalls = 0;
    bool parseReturnValue = true;
    
    // Static call tracking
    static int resetAlertCountTrackerCalls;
    
    void reset() {
        state = DisplayState();
        alerts.clear();
        priorityAlert = AlertData();
        hasAlertsFlag = false;
        parseCalls = 0;
        parseReturnValue = true;
        resetAlertCountTrackerCalls = 0;
        resetAlertAssemblyCalls = 0;
    }
    
    // Test helpers - set state
    void setAlerts(const std::vector<AlertData>& a) {
        alerts = a;
        hasAlertsFlag = !alerts.empty();
        if (hasAlertsFlag) {
            // Find priority alert
            auto it = std::find_if(alerts.begin(), alerts.end(), 
                [](const AlertData& alert) { return alert.isPriority; });
            if (it != alerts.end()) {
                priorityAlert = *it;
            } else {
                priorityAlert = alerts[0];
            }
        }
    }
    
    void setMuted(bool m) { state.muted = m; }
    void setActiveBands(uint8_t bands) { state.activeBands = bands; }
    void setMainVolume(uint8_t vol) { state.mainVolume = vol; state.hasVolumeData = true; }
    void setMuteVolume(uint8_t vol) { state.muteVolume = vol; }
    
    // Parser interface
    bool hasAlerts() const { return hasAlertsFlag; }
    int getAlertCount() const { return static_cast<int>(alerts.size()); }
    AlertData getPriorityAlert() const { return priorityAlert; }
    bool getRenderablePriorityAlert(AlertData& out) const {
        auto isRenderable = [](const AlertData& a) -> bool {
            if (!a.isValid || a.band == BAND_NONE) return false;
            return (a.band == BAND_LASER) || (a.frequency != 0);
        };
        if (isRenderable(priorityAlert)) {
            out = priorityAlert;
            return true;
        }
        for (const auto& alert : alerts) {
            if (isRenderable(alert)) {
                out = alert;
                return true;
            }
        }
        out = AlertData();
        return false;
    }
    const std::vector<AlertData>& getAllAlerts() const { return alerts; }
    DisplayState getDisplayState() const { return state; }

    bool parse(const uint8_t* /*data*/, size_t /*length*/) {
        parseCalls++;
        return parseReturnValue;
    }
    
    // Reset methods
    int resetAlertAssemblyCalls = 0;
    void resetAlertAssembly() { resetAlertAssemblyCalls++; }
    
    static void resetAlertCountTracker() { resetAlertCountTrackerCalls++; }
};

// Static member definitions
inline int PacketParser::resetAlertCountTrackerCalls = 0;
#endif // PACKET_PARSER_H

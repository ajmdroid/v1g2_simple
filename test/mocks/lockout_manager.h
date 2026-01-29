// Mock lockout_manager.h for native unit testing
#pragma once

#include <cstdint>
#include "packet_parser.h"  // For Band enum

struct GPSFix {
    double latitude = 0.0;
    double longitude = 0.0;
    float speed = 0.0f;
    float heading = 0.0f;
    bool valid = false;
};

class LockoutManager {
public:
    // Test-controllable state
    bool shouldMuteResult = false;
    int shouldMuteAlertCalls = 0;
    Band lastMuteCheckBand = BAND_NONE;
    
    void reset() {
        shouldMuteResult = false;
        shouldMuteAlertCalls = 0;
        lastMuteCheckBand = BAND_NONE;
    }
    
    bool shouldMuteAlert(double /*lat*/, double /*lon*/, Band band) {
        shouldMuteAlertCalls++;
        lastMuteCheckBand = band;
        return shouldMuteResult;
    }
    
    void setMuteResult(bool result) { shouldMuteResult = result; }
};

class AutoLockoutManager {
public:
    int recordAlertCalls = 0;
    int recordPassthroughCalls = 0;
    
    void reset() {
        recordAlertCalls = 0;
        recordPassthroughCalls = 0;
    }
    
    void recordAlert(double /*lat*/, double /*lon*/, Band /*band*/, 
                     uint32_t /*freq*/, uint8_t /*strength*/, uint8_t /*dir*/,
                     bool /*isMoving*/, float /*heading*/) {
        recordAlertCalls++;
    }
    
    void recordPassthrough(double /*lat*/, double /*lon*/) {
        recordPassthroughCalls++;
    }
};

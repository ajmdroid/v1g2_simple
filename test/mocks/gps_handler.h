#pragma once

#include "lockout_manager.h"  // For GPSFix

class GPSHandler {
public:
    // Test-controllable state
    bool validFix = false;
    bool readyForNav = false;
    bool moving = false;
    float speedMps = 0.0f;
    float smoothedHeading = 0.0f;
    GPSFix fix;
    
    void reset() {
        validFix = false;
        readyForNav = false;
        moving = false;
        speedMps = 0.0f;
        smoothedHeading = 0.0f;
        fix = GPSFix();
    }
    
    // Setters for test setup
    void setValidFix(bool v) { validFix = v; fix.valid = v; }
    void setReadyForNavigation(bool v) { readyForNav = v; }
    void setMoving(bool v) { moving = v; }
    void setSpeedMps(float v) { speedMps = v; fix.speed = v; }
    void setSmoothedHeading(float v) { smoothedHeading = v; fix.heading = v; }
    void setPosition(double lat, double lon) { 
        fix.latitude = lat; 
        fix.longitude = lon; 
    }
    
    // GPSHandler interface
    bool hasValidFix() const { return validFix; }
    bool isReadyForNavigation() const { return readyForNav; }
    bool isMoving() const { return moving; }
    float getSpeed() const { return speedMps; }
    float getSmoothedHeading() const { return smoothedHeading; }
    GPSFix getFix() const { return fix; }
};

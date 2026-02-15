// Mock display.h for native unit testing
#pragma once

#include <cstdint>
#ifdef ARDUINO
#include <Arduino.h>
#else
#include "Arduino.h"
#endif

// Forward declare AlertData
struct AlertData;
struct DisplayState;

// Screen dimensions (needed by some tests)
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 640
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 172
#endif

/**
 * Mock V1Display - tracks method calls for verification
 */
class V1Display {
public:
    // Call tracking
    int showScanningCalls = 0;
    int showRestingCalls = 0;
    int showDisconnectedCalls = 0;
    int updateCalls = 0;
    int updatePersistedCalls = 0;
    int updateCameraAlertCalls = 0;
    int flushCalls = 0;
    int forceNextRedrawCalls = 0;
    int drawWiFiIndicatorCalls = 0;
    int drawBatteryIndicatorCalls = 0;
    int drawProfileIndicatorCalls = 0;
    int setLockoutMutedCalls = 0;
    bool lastLockoutMutedValue = false;

    // Static method tracking
    static int resetChangeTrackingCalls;

    void reset() {
        showScanningCalls = 0;
        showRestingCalls = 0;
        showDisconnectedCalls = 0;
        updateCalls = 0;
        updatePersistedCalls = 0;
        updateCameraAlertCalls = 0;
        flushCalls = 0;
        forceNextRedrawCalls = 0;
        drawWiFiIndicatorCalls = 0;
        drawBatteryIndicatorCalls = 0;
        drawProfileIndicatorCalls = 0;
        setLockoutMutedCalls = 0;
        lastLockoutMutedValue = false;
        resetChangeTrackingCalls = 0;
    }

    // Display methods
    void showScanning() { showScanningCalls++; }
    void showResting() { showRestingCalls++; }
    void showDisconnected() { showDisconnectedCalls++; }
    
    void update(const DisplayState& /*state*/) { updateCalls++; }
    void update(const AlertData& /*priority*/, const AlertData* /*alerts*/, 
                int /*count*/, const DisplayState& /*state*/) { updateCalls++; }
    void updatePersisted(const AlertData& /*alert*/, const DisplayState& /*state*/) { 
        updatePersistedCalls++; 
    }
    void updateCameraAlert(uint8_t /*cameraType*/, bool /*muted*/ = false) {
        updateCameraAlertCalls++;
    }
    
    void flush() { flushCalls++; }
    void forceNextRedraw() { forceNextRedrawCalls++; }
    
    void drawWiFiIndicator() { drawWiFiIndicatorCalls++; }
    void drawBatteryIndicator() { drawBatteryIndicatorCalls++; }
    void drawProfileIndicator(int /*slot*/) { drawProfileIndicatorCalls++; }
    
    void setLockoutMuted(bool muted) { 
        setLockoutMutedCalls++; 
        lastLockoutMutedValue = muted;
    }
    
    void setBLEProxyStatus(bool /*connected*/, bool /*proxy*/, bool /*receiving*/) {}
    void setBrightness(uint8_t /*level*/) {}
    
    // Static methods
    static void resetChangeTracking() { resetChangeTrackingCalls++; }
};

// Definition of static member
inline int V1Display::resetChangeTrackingCalls = 0;

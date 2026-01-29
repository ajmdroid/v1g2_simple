// Mock camera_alert_module.h for native unit testing
#pragma once

class CameraAlertModule {
public:
    int updateCardStateForV1Calls = 0;
    bool lastCardStateForV1 = false;
    int updateMainDisplayCalls = 0;
    bool lastMainDisplayHasV1Alerts = false;
    
    void reset() {
        updateCardStateForV1Calls = 0;
        lastCardStateForV1 = false;
        updateMainDisplayCalls = 0;
        lastMainDisplayHasV1Alerts = false;
    }
    
    void updateCardStateForV1(bool hasV1Alerts) {
        updateCardStateForV1Calls++;
        lastCardStateForV1 = hasV1Alerts;
    }
    
    void updateMainDisplay(bool v1HasActiveAlerts) {
        updateMainDisplayCalls++;
        lastMainDisplayHasV1Alerts = v1HasActiveAlerts;
    }
};

// Mock power_module.h for native unit testing
#pragma once

class PowerModule {
public:
    int onV1ConnectionChangeCalls = 0;
    bool lastConnectionState = false;
    
    void reset() {
        onV1ConnectionChangeCalls = 0;
        lastConnectionState = false;
    }
    
    void onV1ConnectionChange(bool connected) {
        onV1ConnectionChangeCalls++;
        lastConnectionState = connected;
    }
};

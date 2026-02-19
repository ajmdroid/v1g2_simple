// Mock voice_module.h for native unit testing
#pragma once

class VoiceModule {
public:
    int clearAllStateCalls = 0;
    float mockSpeedMph = 0.0f;
    bool mockHasValidSpeed = false;
    
    void resetMock() {
        clearAllStateCalls = 0;
        mockSpeedMph = 0.0f;
        mockHasValidSpeed = false;
    }
    
    float getCurrentSpeedMph(unsigned long /*now*/) { return mockSpeedMph; }
    bool hasValidSpeedSource(unsigned long /*now*/) const { return mockHasValidSpeed; }
    void reset() { clearAllState(); }  // Alias for consistency with other modules
    void clearAllState() { clearAllStateCalls++; }
};

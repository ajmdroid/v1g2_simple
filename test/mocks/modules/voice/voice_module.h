// Mock voice_module.h for native unit testing
#pragma once

class VoiceModule {
public:
    int clearAllStateCalls = 0;
    
    void resetMock() {
        clearAllStateCalls = 0;
    }
    
    float getCurrentSpeedMph(unsigned long /*now*/) { return 0.0f; }
    void reset() { clearAllState(); }  // Alias for consistency with other modules
    void clearAllState() { clearAllStateCalls++; }
};

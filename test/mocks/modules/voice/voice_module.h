// Mock voice_module.h for native unit testing
#pragma once

class VoiceModule {
public:
    int clearAllStateCalls = 0;
    
    void reset() {
        clearAllStateCalls = 0;
    }
    
    float getCurrentSpeedMph(unsigned long /*now*/) { return 0.0f; }
    void clearAllState() { clearAllStateCalls++; }
};

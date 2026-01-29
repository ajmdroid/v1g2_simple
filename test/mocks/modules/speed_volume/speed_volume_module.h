// Mock speed_volume_module.h for native unit testing
#pragma once

class SpeedVolumeModule {
public:
    bool boostActive = false;
    uint8_t originalVolume = 0xFF;
    int resetCalls = 0;
    
    void reset() {
        boostActive = false;
        originalVolume = 0xFF;
        resetCalls = 0;
    }
    
    bool isBoostActive() const { return boostActive; }
    uint8_t getOriginalVolume() const { return originalVolume; }
    void resetState() { resetCalls++; }  // Named differently to avoid conflict with test reset()
};

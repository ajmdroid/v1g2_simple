// Mock speed_volume_module.h for native unit testing
#pragma once

class SpeedVolumeModule {
public:
    bool boostActive = false;
    bool quietActive = false;
    uint8_t originalVolume = 0xFF;
    uint8_t mockQuietVolume = 0xFF;
    int resetCalls = 0;
    
    void reset() {
        boostActive = false;
        quietActive = false;
        originalVolume = 0xFF;
        mockQuietVolume = 0xFF;
        resetCalls = 0;
    }
    
    bool isBoostActive() const { return boostActive; }
    bool isQuietActive() const { return quietActive; }
    uint8_t getOriginalVolume() const { return originalVolume; }
    uint8_t getQuietVolume() const { return mockQuietVolume; }
    void resetState() { resetCalls++; }  // Named differently to avoid conflict with test reset()
};

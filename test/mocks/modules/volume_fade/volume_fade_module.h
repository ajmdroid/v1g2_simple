// Mock volume_fade_module.h for native unit testing
#pragma once

class VolumeFadeModule {
public:
    bool tracking = false;
    
    void reset() {
        tracking = false;
    }
    
    bool isTracking() const { return tracking; }
};

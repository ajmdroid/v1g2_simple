#pragma once

#include <cstdint>
#include <cstring>

class V1ProfileManager {
public:
    int setCurrentSettingsCalls = 0;
    uint8_t lastSettings[6] = {};

    void reset() {
        setCurrentSettingsCalls = 0;
        std::memset(lastSettings, 0, sizeof(lastSettings));
    }

    void setCurrentSettings(const uint8_t* bytes) {
        setCurrentSettingsCalls++;
        if (bytes) {
            std::memcpy(lastSettings, bytes, sizeof(lastSettings));
        }
    }
};

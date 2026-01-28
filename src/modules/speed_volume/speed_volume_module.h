#pragma once

#include <stdint.h>

class SettingsManager;

struct SpeedVolumeContext {
    bool bleConnected = false;
    bool fadeTakingControl = false;
    uint8_t currentVolume = 0;
    uint8_t currentMuteVolume = 0;
    float speedMph = 0.0f;
    unsigned long now = 0;
};

struct SpeedVolumeAction {
    enum class Type : uint8_t {
        NONE = 0,
        BOOST,
        RESTORE
    };
    Type type = Type::NONE;
    uint8_t volume = 0;
    uint8_t muteVolume = 0;
    
    bool hasAction() const { return type != Type::NONE; }
};

class SpeedVolumeModule {
public:
    SpeedVolumeModule();
    
    void begin(SettingsManager* settings);
    SpeedVolumeAction process(const SpeedVolumeContext& ctx);
    void reset();
    bool isBoostActive() const { return boostActive; }
    uint8_t getOriginalVolume() const { return originalVolume; }
    
private:
    SettingsManager* settings = nullptr;
    bool boostActive = false;
    uint8_t originalVolume = 0xFF;
    unsigned long lastCheckMs = 0;
    bool loggedSettings = false;
    
    static constexpr unsigned long CHECK_INTERVAL_MS = 2000;
};


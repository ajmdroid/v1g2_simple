#pragma once

#include <stdint.h>

// Forward declarations
class SettingsManager;
class V1BLEClient;
class PacketParser;
class VoiceModule;
class VolumeFadeModule;

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

struct SpeedVolumeContext {
    bool bleConnected = false;
    bool fadeTakingControl = false;
    uint8_t currentVolume = 0;
    uint8_t currentMuteVolume = 0;
    float speedMph = 0.0f;
    unsigned long now = 0;
};

class SpeedVolumeModule {
public:
    SpeedVolumeModule();
    
    // Full initialization with all dependencies for self-contained processing
    void begin(SettingsManager* settings,
               V1BLEClient* bleClient,
               PacketParser* parser,
               VoiceModule* voiceModule,
               VolumeFadeModule* volumeFadeModule);

    // Lightweight init for unit tests (no BLE/parser needed)
    void begin(SettingsManager* settings);
    
    // Self-contained process: gathers context internally and executes BLE commands
    void process(unsigned long nowMs);

    // Pure decision helper for tests (no BLE side effects)
    SpeedVolumeAction process(const SpeedVolumeContext& ctx);
    
    void reset();
    bool isBoostActive() const { return boostActive; }
    uint8_t getOriginalVolume() const { return originalVolume; }
    
private:
    SettingsManager* settings = nullptr;
    V1BLEClient* ble = nullptr;
    PacketParser* parser = nullptr;
    VoiceModule* voice = nullptr;
    VolumeFadeModule* volumeFade = nullptr;
    
    bool boostActive = false;
    uint8_t originalVolume = 0xFF;
    unsigned long lastCheckMs = 0;
    bool loggedSettings = false;
    
    static constexpr unsigned long CHECK_INTERVAL_MS = 2000;
};

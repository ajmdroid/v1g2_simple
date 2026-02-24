#pragma once

#include <stdint.h>

struct SpeedVolumeRuntimeContext {
    uint32_t nowMs = 0;
    uint8_t configuredVoiceVolume = 0;
};

// Orchestrates speed-volume policy evaluation and speaker quiet synchronization.
class SpeedVolumeRuntimeModule {
public:
    struct Providers {
        void (*runSpeedVolumeProcess)(void* ctx, uint32_t nowMs) = nullptr;
        void* speedVolumeContext = nullptr;

        bool (*readSpeedQuietActive)(void* ctx) = nullptr;
        void* speedQuietActiveContext = nullptr;
        uint8_t (*readSpeedQuietVolume)(void* ctx) = nullptr;
        void* speedQuietVolumeContext = nullptr;

        void (*runSpeakerQuietSync)(
            void* ctx, bool quietNow, uint8_t quietVolume, uint8_t configuredVoiceVolume) = nullptr;
        void* speakerQuietContext = nullptr;
    };

    void begin(const Providers& hooks);
    void reset();
    void process(const SpeedVolumeRuntimeContext& ctx);

private:
    Providers providers{};
};

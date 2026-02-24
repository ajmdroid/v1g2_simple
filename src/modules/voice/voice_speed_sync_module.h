#pragma once

#include <stdint.h>

#include "../speed/speed_source_selector.h"

struct VoiceSpeedSyncContext {
    uint32_t nowMs = 0;
};

// Synchronizes selected runtime speed samples into VoiceModule cache.
class VoiceSpeedSyncModule {
public:
    struct Providers {
        bool (*selectSpeedSample)(void* ctx, uint32_t nowMs, SpeedSelection& selection) = nullptr;
        void* speedSelectorContext = nullptr;

        void (*updateVoiceSpeedSample)(void* ctx, float speedMph, uint32_t timestampMs) = nullptr;
        void (*clearVoiceSpeedSample)(void* ctx) = nullptr;
        void* voiceContext = nullptr;
    };

    void begin(const Providers& hooks);
    void reset();
    void process(const VoiceSpeedSyncContext& ctx);

private:
    Providers providers{};
};

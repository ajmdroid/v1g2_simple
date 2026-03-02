#pragma once

#include <stdint.h>

struct LoopSettingsPrepValues {
    bool enableWifiAtBoot = false;
    bool enableSignalTraceLogging = false;
};

struct LoopSettingsPrepContext {
    uint32_t nowMs = 0;

    void (*runTapGesture)(uint32_t nowMs) = nullptr;
    LoopSettingsPrepValues (*readSettingsValues)() = nullptr;
};

// Orchestrates tap-gesture processing and loop settings snapshot reads.
class LoopSettingsPrepModule {
public:
    struct Providers {
        void (*runTapGesture)(void* ctx, uint32_t nowMs) = nullptr;
        void* tapGestureContext = nullptr;

        LoopSettingsPrepValues (*readSettingsValues)(void* ctx) = nullptr;
        void* settingsContext = nullptr;
    };

    void begin(const Providers& hooks);
    void reset();
    LoopSettingsPrepValues process(const LoopSettingsPrepContext& ctx);

private:
    Providers providers{};
};

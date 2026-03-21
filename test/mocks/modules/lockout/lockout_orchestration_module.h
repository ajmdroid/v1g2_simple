#pragma once

#include <cstdint>

#include "../gps/gps_runtime_module.h"

enum class LockoutVolumeCommandType : uint8_t {
    None = 0,
    PreQuietDrop,
    PreQuietRestore,
};

struct LockoutVolumeCommand {
    LockoutVolumeCommandType type = LockoutVolumeCommandType::None;
    uint8_t volume = 0;
    uint8_t muteVolume = 0;

    bool hasAction() const {
        return type != LockoutVolumeCommandType::None;
    }
};

struct LockoutOrchestrationResult {
    bool prioritySuppressed = false;
    LockoutVolumeCommand volumeCommand{};
};

class LockoutOrchestrationModule {
public:
    int processCalls = 0;
    uint32_t lastNowMs = 0;
    bool lastProxyConnected = false;
    bool lastEnableSignalTrace = false;
    GpsRuntimeStatus lastGpsStatus{};
    LockoutOrchestrationResult nextResult{};

    bool mockPreQuietActive = false;
    uint8_t volumeHintMain = 0xFF;
    uint8_t volumeHintMute = 0;
    int setVolumeHintCalls = 0;
    int clearVolumeHintCalls = 0;

    LockoutOrchestrationResult process(uint32_t nowMs,
                                       const GpsRuntimeStatus& gpsStatus,
                                       bool proxyClientConnected,
                                       bool enableSignalTrace) {
        processCalls++;
        lastNowMs = nowMs;
        lastGpsStatus = gpsStatus;
        lastProxyConnected = proxyClientConnected;
        lastEnableSignalTrace = enableSignalTrace;
        return nextResult;
    }

    bool isPreQuietActive() const { return mockPreQuietActive; }

    void setVolumeHint(uint8_t mainVol, uint8_t muteVol) {
        setVolumeHintCalls++;
        volumeHintMain = mainVol;
        volumeHintMute = muteVol;
    }

    void clearVolumeHint() {
        clearVolumeHintCalls++;
        volumeHintMain = 0xFF;
        volumeHintMute = 0;
    }
};

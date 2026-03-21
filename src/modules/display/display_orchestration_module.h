#pragma once

#include <Arduino.h>
#include "display_ble_context.h"

class V1Display;
class V1BLEClient;
class BleQueueModule;
class DisplayPreviewModule;
class DisplayRestoreModule;
class PacketParser;
class SettingsManager;
class GpsRuntimeModule;
class LockoutOrchestrationModule;
struct LockoutVolumeCommand;
class VolumeFadeModule;
class SpeedMuteModule;

struct DisplayOrchestrationEarlyContext {
    uint32_t nowMs = 0;
    bool bootSplashHoldActive = false;
    bool overloadThisLoop = false;
    DisplayBleContext bleContext{};
    bool bleReceiving = false;
};

struct DisplayOrchestrationParsedContext {
    uint32_t nowMs = 0;
    bool parsedReady = false;
    bool bootSplashHoldActive = false;
    bool enableSignalTraceLogging = false;
};

struct DisplayOrchestrationParsedResult {
    bool lockoutEvaluated = false;
    bool lockoutPrioritySuppressed = false;
    bool runDisplayPipeline = false;
};

struct DisplayOrchestrationRefreshContext {
    uint32_t nowMs = 0;
    bool bootSplashHoldActive = false;
    bool overloadLateThisLoop = false;
    bool pipelineRanThisLoop = false;
};

struct DisplayOrchestrationRefreshResult {
    bool signalPriorityActive = false;
};

class DisplayOrchestrationModule {
public:
    void begin(V1Display* displayPtr,
               V1BLEClient* bleClient,
               BleQueueModule* bleQueueModule,
               DisplayPreviewModule* previewModule,
               DisplayRestoreModule* restoreModule,
               PacketParser* parserPtr,
               SettingsManager* settingsManager,
               GpsRuntimeModule* gpsModule,
               LockoutOrchestrationModule* lockoutModule,
               VolumeFadeModule* volumeFadeModule,
               SpeedMuteModule* speedMuteModule);

    void processEarly(const DisplayOrchestrationEarlyContext& ctx);
    DisplayOrchestrationParsedResult processParsedFrame(const DisplayOrchestrationParsedContext& ctx);
    DisplayOrchestrationRefreshResult processLightweightRefresh(const DisplayOrchestrationRefreshContext& ctx);

private:
    void reset();

    V1Display* display = nullptr;
    V1BLEClient* ble = nullptr;
    BleQueueModule* bleQueue = nullptr;
    DisplayPreviewModule* preview = nullptr;
    DisplayRestoreModule* restore = nullptr;
    PacketParser* parser = nullptr;
    SettingsManager* settings = nullptr;
    GpsRuntimeModule* gpsRuntime = nullptr;
    LockoutOrchestrationModule* lockout = nullptr;
    VolumeFadeModule* volumeFade = nullptr;
    SpeedMuteModule* speedMute = nullptr;

    uint32_t lastGpsSatUpdateMs = 0;
    unsigned long lastFreqUiMs = 0;
    unsigned long lastCardUiMs = 0;

    static constexpr uint32_t GPS_SAT_UPDATE_INTERVAL_MS = 90000;
    static constexpr uint32_t LOCKOUT_INDICATOR_STALE_MS = 2000;
    static constexpr unsigned long FREQ_UI_MAX_MS = 75;
    static constexpr unsigned long FREQ_UI_PREVIEW_MAX_MS = 250;
    static constexpr unsigned long CARD_UI_MAX_MS = 100;

    bool executeLockoutVolumeCommand(const LockoutVolumeCommand& command, uint32_t nowMs);
    bool retryPendingPreQuietRestore(uint32_t nowMs);
    void executeVolumeFade(uint32_t nowMs, bool lockoutPrioritySuppressed);
    bool processSpeedVolume(uint32_t nowMs);
    bool retryPendingSpeedVolRestore(uint32_t nowMs);

    // Pending pre-quiet restore retry state.
    // Mirrors volume-fade's pendingRestore pattern: stash the target volumes
    // and retry until V1 confirms or a timeout expires.
    uint8_t pendingPqRestoreVol_ = 0xFF;       // 0xFF = no pending restore
    uint8_t pendingPqRestoreMuteVol_ = 0;
    uint32_t pendingPqRestoreSetMs_ = 0;
    uint32_t pendingPqRestoreLastRetryMs_ = 0;
    static constexpr uint32_t PQ_RESTORE_TIMEOUT_MS = 2000;
    static constexpr uint32_t PQ_RESTORE_RETRY_INTERVAL_MS = 75;

    // Speed volume state.
    // When speed mute is active and v1Volume != 0xFF, the orchestrator
    // owns V1 hardware volume: captures original, drops to target,
    // retries until confirmed, and restores on unmute.
    bool speedVolActive_ = false;
    uint8_t speedVolSavedOriginal_ = 0xFF;   // Original main volume (0xFF = not captured)
    uint8_t speedVolSavedMuteVol_ = 0;       // Original mute volume
    uint8_t pendingSpeedVolRestoreVol_ = 0xFF; // 0xFF = no pending restore
    uint8_t pendingSpeedVolRestoreMuteVol_ = 0;
    uint32_t pendingSpeedVolRestoreSetMs_ = 0;
    uint32_t pendingSpeedVolRestoreLastRetryMs_ = 0;
    uint32_t speedVolLastRetryMs_ = 0;        // Rate-limit drop retries
    static constexpr uint32_t SPEED_VOL_RETRY_INTERVAL_MS = 75;
    static constexpr uint32_t SPEED_VOL_RESTORE_TIMEOUT_MS = 2000;
};

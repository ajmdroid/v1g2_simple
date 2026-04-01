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
class VolumeFadeModule;
class SpeedMuteModule;
class QuietCoordinatorModule;

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
};

struct DisplayOrchestrationParsedResult {
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
               VolumeFadeModule* volumeFadeModule,
               SpeedMuteModule* speedMuteModule,
               QuietCoordinatorModule* quietCoordinator);

    void processEarly(const DisplayOrchestrationEarlyContext& ctx);
    DisplayOrchestrationParsedResult processParsedFrame(const DisplayOrchestrationParsedContext& ctx);
    DisplayOrchestrationRefreshResult processLightweightRefresh(const DisplayOrchestrationRefreshContext& ctx);

private:
    void reset();
    void syncQuietPresentation();
    void executeVolumeFade(uint32_t nowMs);
    bool processSpeedVolume(uint32_t nowMs);
    bool retryPendingSpeedVolRestore(uint32_t nowMs);

    V1Display* display = nullptr;
    V1BLEClient* ble = nullptr;
    BleQueueModule* bleQueue = nullptr;
    DisplayPreviewModule* preview = nullptr;
    DisplayRestoreModule* restore = nullptr;
    PacketParser* parser = nullptr;
    SettingsManager* settings = nullptr;
    GpsRuntimeModule* gpsRuntime = nullptr;
    VolumeFadeModule* volumeFade = nullptr;
    SpeedMuteModule* speedMute = nullptr;
    QuietCoordinatorModule* quiet = nullptr;

    uint32_t lastGpsSatUpdateMs = 0;
    unsigned long lastFreqUiMs = 0;
    unsigned long lastCardUiMs = 0;

    static constexpr uint32_t GPS_SAT_UPDATE_INTERVAL_MS = 90000;
    static constexpr uint32_t PARSED_STALE_MS = 2000;
    static constexpr unsigned long FREQ_UI_MAX_MS = 75;
    static constexpr unsigned long FREQ_UI_PREVIEW_MAX_MS = 250;
    static constexpr unsigned long CARD_UI_MAX_MS = 100;

};

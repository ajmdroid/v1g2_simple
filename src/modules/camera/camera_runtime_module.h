#pragma once

#include <math.h>
#include <stdint.h>

#include "camera_data_loader.h"
#include "camera_event_log.h"
#include "camera_index.h"

struct CameraRuntimeCounters {
    uint32_t cameraTicks = 0;
    uint32_t cameraTickSkipsOverload = 0;
    uint32_t cameraTickSkipsNonCore = 0;
    uint32_t cameraTickSkipsMemoryGuard = 0;
    uint32_t cameraCandidatesChecked = 0;
    uint32_t cameraMatches = 0;
    uint32_t cameraAlertsStarted = 0;
    uint32_t cameraBudgetExceeded = 0;
    uint32_t cameraLoadFailures = 0;
    uint32_t cameraLoadSkipsMemoryGuard = 0;
    uint32_t cameraIndexSwapCount = 0;
    uint32_t cameraIndexSwapFailures = 0;
};

struct CameraRuntimeStatus {
    bool enabled = false;
    bool indexLoaded = false;
    uint32_t tickIntervalMs = 0;
    uint32_t lastTickMs = 0;
    uint32_t lastTickDurationUs = 0;
    uint32_t maxTickDurationUs = 0;
    uint32_t lastCandidatesChecked = 0;
    uint32_t lastMatches = 0;
    bool lastCapReached = false;
    float lastHeadingDeltaDeg = NAN;
    uint32_t lastInternalFree = 0;
    uint32_t lastInternalLargestBlock = 0;
    uint32_t memoryGuardMinFree = 0;
    uint32_t memoryGuardMinLargestBlock = 0;
    CameraRuntimeCounters counters;
    CameraDataLoaderStatus loader;
};

class CameraRuntimeModule {
public:
    static constexpr uint32_t DEFAULT_TICK_INTERVAL_MS = 200;
    // Must stay above WiFi AP+STA runtime threshold (20 KiB) + margin.
    static constexpr uint32_t kMemoryGuardMinFreeInternal = 32768;      // 32 KiB
    static constexpr uint32_t kMemoryGuardMinLargestBlock = 16384;      // 16 KiB

    void begin(bool enabled);
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    // Main-loop low-priority hook.
    void process(uint32_t nowMs, bool skipNonCoreThisLoop, bool overloadThisLoop);

    // Compatibility helper for existing scaffolding/tests.
    bool tryLoadDefault(uint32_t nowMs);
    void requestReload();

    CameraRuntimeStatus snapshot() const;

    const CameraIndex& index() const { return index_; }
    // Read access is safe from loop()-owned code paths (e.g., WiFi handlers invoked via wifiManager.process()).
    const CameraEventLog& eventLog() const { return eventLog_; }

private:
    bool enabled_ = false;
    uint32_t tickIntervalMs_ = DEFAULT_TICK_INTERVAL_MS;
    uint32_t lastTickMs_ = 0;
    uint32_t lastTickDurationUs_ = 0;
    uint32_t maxTickDurationUs_ = 0;
    uint32_t lastCandidatesChecked_ = 0;
    uint32_t lastMatches_ = 0;
    bool lastCapReached_ = false;
    float lastHeadingDeltaDeg_ = NAN;
    uint32_t lastInternalFree_ = 0;
    uint32_t lastInternalLargestBlock_ = 0;
    CameraRuntimeCounters counters_ = {};
    CameraIndex index_;
    CameraEventLog eventLog_;
    CameraDataLoader dataLoader_;
};

extern CameraRuntimeModule cameraRuntimeModule;

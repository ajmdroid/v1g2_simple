#pragma once

#include <stdint.h>

#include "camera_data_loader.h"
#include "camera_event_log.h"
#include "camera_index.h"

struct CameraRuntimeCounters {
    uint32_t cameraTicks = 0;
    uint32_t cameraTickSkipsOverload = 0;
    uint32_t cameraTickSkipsNonCore = 0;
    uint32_t cameraCandidatesChecked = 0;
    uint32_t cameraMatches = 0;
    uint32_t cameraAlertsStarted = 0;
    uint32_t cameraBudgetExceeded = 0;
    uint32_t cameraLoadFailures = 0;
};

struct CameraRuntimeStatus {
    bool enabled = false;
    bool indexLoaded = false;
    uint32_t tickIntervalMs = 0;
    uint32_t lastTickMs = 0;
    CameraRuntimeCounters counters;
    CameraDataLoaderStatus loader;
};

class CameraRuntimeModule {
public:
    static constexpr uint32_t DEFAULT_TICK_INTERVAL_MS = 200;

    void begin(bool enabled);
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    // Main-loop low-priority hook.
    void process(uint32_t nowMs, bool skipNonCoreThisLoop, bool overloadThisLoop);

    // M1 helper: placeholder load call (no IO yet, always fails cleanly).
    bool tryLoadDefault(uint32_t nowMs);

    CameraRuntimeStatus snapshot() const;

    const CameraIndex& index() const { return index_; }
    const CameraEventLog& eventLog() const { return eventLog_; }

private:
    bool enabled_ = false;
    uint32_t tickIntervalMs_ = DEFAULT_TICK_INTERVAL_MS;
    uint32_t lastTickMs_ = 0;
    CameraRuntimeCounters counters_ = {};
    CameraIndex index_;
    CameraEventLog eventLog_;
    CameraDataLoader dataLoader_;
};

extern CameraRuntimeModule cameraRuntimeModule;


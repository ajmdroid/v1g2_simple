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

enum class CameraLifecycleState : uint8_t {
    IDLE = 0,
    ACTIVE = 1,
    PREEMPTED = 2,
    SUPPRESSED_UNTIL_EXIT = 3,
};

enum class CameraClearReason : uint8_t {
    NONE = 0,
    PASS_DISTANCE = 1,
    TURN_AWAY = 2,
    ELIGIBILITY_INVALID = 3,
    PREEMPTED_BY_SIGNAL = 4,
    REPLACED_BY_NEW_MATCH = 5,
    TIMEOUT = 6,
};

struct CameraActiveAlertStatus {
    bool active = false;
    uint32_t cameraId = 0;
    uint8_t type = 0;
    uint16_t distanceM = 0;
    float headingDeltaDeg = NAN;
    uint32_t startTsMs = 0;
    uint32_t lastUpdateTsMs = 0;
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
    CameraLifecycleState lifecycleState = CameraLifecycleState::IDLE;
    CameraClearReason lastClearReason = CameraClearReason::NONE;
    uint32_t suppressedCameraId = 0;
    uint16_t alertDistanceFt = 0;
    uint8_t alertPersistSec = 0;
    CameraActiveAlertStatus activeAlert;
    CameraRuntimeCounters counters;
    CameraDataLoaderStatus loader;
};

class CameraRuntimeModule {
public:
    static constexpr uint32_t DEFAULT_TICK_INTERVAL_MS = 200;
    static constexpr uint16_t kAlertDistanceFtMin = 500;
    static constexpr uint16_t kAlertDistanceFtMax = 2000;
    // Preserve historical fixed 500 m trigger behavior by default.
    static constexpr uint16_t kAlertDistanceFtDefault = 1640;
    static constexpr uint8_t kAlertPersistSecMin = 3;
    static constexpr uint8_t kAlertPersistSecMax = 10;
    static constexpr uint8_t kAlertPersistSecDefault = 5;
    // Must stay above WiFi AP+STA runtime threshold (20 KiB) + margin.
    static constexpr uint32_t kMemoryGuardMinFreeInternal = 32768;      // 32 KiB
    static constexpr uint32_t kMemoryGuardMinLargestBlock = 16384;      // 16 KiB

    void begin(bool enabled);
    void setEnabled(bool enabled);
    void setAlertTuning(uint16_t distanceFt, uint8_t persistSec);
    bool isEnabled() const { return enabled_; }

    // Main-loop low-priority hook.
    void process(uint32_t nowMs,
                 bool skipNonCoreThisLoop,
                 bool overloadThisLoop,
                 bool signalPriorityActive = false);

    // Compatibility helper for existing scaffolding/tests.
    bool tryLoadDefault(uint32_t nowMs);
    void requestReload();
    void notifySignalPreempted(uint32_t nowMs);

    CameraRuntimeStatus snapshot() const;

    const CameraIndex& index() const { return index_; }
    // Read access is safe from loop()-owned code paths (e.g., WiFi handlers invoked via wifiManager.process()).
    const CameraEventLog& eventLog() const { return eventLog_; }

private:
    void clearActiveAlert(CameraClearReason reason, uint32_t nowMs, bool suppressSamePass);
    void startActiveAlert(uint32_t nowMs,
                          uint32_t cameraId,
                          uint8_t cameraType,
                          uint16_t distanceM,
                          float headingDeltaDeg);
    bool shouldLiftSuppression(bool sawSuppressedThisTick);

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
    CameraLifecycleState lifecycleState_ = CameraLifecycleState::IDLE;
    CameraClearReason lastClearReason_ = CameraClearReason::NONE;
    float alertRadiusM_ = static_cast<float>(kAlertDistanceFtDefault) * 0.3048f;
    uint32_t maxAlertDurationMs_ = static_cast<uint32_t>(kAlertPersistSecDefault) * 1000UL;
    uint16_t alertDistanceFt_ = kAlertDistanceFtDefault;
    uint8_t alertPersistSec_ = kAlertPersistSecDefault;
    CameraActiveAlertStatus activeAlert_ = {};
    uint32_t suppressedCameraId_ = 0;
    uint8_t turnAwayConsecutiveTicks_ = 0;
    uint8_t suppressionExitTicks_ = 0;
    CameraRuntimeCounters counters_ = {};
    CameraIndex index_;
    CameraEventLog eventLog_;
    CameraDataLoader dataLoader_;
};

extern CameraRuntimeModule cameraRuntimeModule;

/**
 * Main loop phase helpers extracted from main.cpp.
 *
 * Keeps setup()/loop() orchestration readable while preserving exact behavior.
 */

#ifndef MAIN_LOOP_PHASES_H
#define MAIN_LOOP_PHASES_H

#include <Arduino.h>
#include "modules/system/loop_connection_early_module.h"
#include "modules/system/loop_display_module.h"
#include "modules/system/loop_ingest_module.h"
#include "modules/system/loop_post_display_module.h"
#include "modules/system/loop_power_touch_module.h"
#include "modules/system/loop_pre_ingest_module.h"
#include "modules/system/loop_runtime_snapshot_module.h"
#include "modules/system/loop_settings_prep_module.h"

struct LoopConnectionEarlyPhaseValues {
    bool bootSplashHoldActive = false;
    bool initialScanningScreenShown = false;
    bool bleConnectedNow = false;
    bool bleBackpressure = false;
    bool skipNonCoreThisLoop = false;
    bool overloadThisLoop = false;
};

struct LoopIngestPhaseValues {
    LoopSettingsPrepValues loopSettingsPrepValues;
    bool bootReady = false;
    bool bleBackpressure = false;
    bool skipLateNonCoreThisLoop = false;
    bool overloadLateThisLoop = false;
};

struct LoopDisplayPreWifiPhaseValues {
    bool loopSignalPriorityActive = false;
};

struct LoopWifiPhaseValues {
    LoopRuntimeSnapshotValues loopRuntimeSnapshotValues;
    bool wifiAutoStartDone = false;
};

struct LoopFinalizePhaseValues {
    unsigned long dispatchNowMs = 0;
    bool bleConnectedNow = false;
    unsigned long lastLoopUs = 0;
};

LoopConnectionEarlyPhaseValues processLoopConnectionEarlyPhase(
    unsigned long nowMs,
    unsigned long nowUs,
    unsigned long lastLoopUs,
    bool currentBootSplashHoldActive,
    unsigned long currentBootSplashHoldUntilMs,
    bool currentInitialScanningScreenShown);

LoopIngestPhaseValues processLoopIngestPhase(
    unsigned long nowMs,
    bool currentBootReady,
    unsigned long bootReadyDeadlineMs,
    bool skipNonCoreThisLoop,
    bool overloadThisLoop,
    void (*runBleProcess)(),
    void (*runBleDrain)());

LoopDisplayPreWifiPhaseValues processLoopDisplayPreWifiPhase(
    unsigned long nowMs,
    bool bootSplashHoldActive,
    bool overloadLateThisLoop,
    bool enableSignalTraceLogging,
    bool skipLateNonCoreThisLoop,
    void (*runDisplayPipeline)(uint32_t nowMs, bool lockoutPrioritySuppressed));

LoopWifiPhaseValues processLoopWifiPhase(
    unsigned long nowMs,
    unsigned long v1ConnectedAtMs,
    bool enableWifi,
    bool enableWifiAtBoot,
    bool currentWifiAutoStartDone,
    bool skipLateNonCoreThisLoop,
    bool bootSplashHoldActive,
    void (*runWifiManagerProcess)());

LoopFinalizePhaseValues processLoopFinalizePhase(
    unsigned long nowMs,
    const LoopSettingsPrepValues& loopSettingsPrepValues,
    bool bootSplashHoldActive,
    bool displayPreviewRunning,
    bool bleBackpressure,
    unsigned long scanScreenDwellMs,
    unsigned long connectionStateProcessMaxGapMs,
    unsigned long loopStartUs);

bool shouldReturnEarlyFromLoopPowerTouchPhase(unsigned long nowMs,
                                              unsigned long loopStartUs);

#endif  // MAIN_LOOP_PHASES_H

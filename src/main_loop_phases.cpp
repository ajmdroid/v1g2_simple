#include "main_loop_phases.h"

#include "battery_manager.h"
#include "ble_client.h"
#include "modules/system/loop_tail_module.h"
#include "modules/system/periodic_maintenance_module.h"
#include "modules/wifi/wifi_runtime_module.h"
#include "../include/config.h"
#include "../include/main_globals.h"

LoopConnectionEarlyPhaseValues processLoopConnectionEarlyPhase(
    const unsigned long nowMs,
    const unsigned long nowUs,
    const unsigned long lastLoopUs,
    const bool currentBootSplashHoldActive,
    const unsigned long currentBootSplashHoldUntilMs,
    const bool currentInitialScanningScreenShown) {
    LoopConnectionEarlyContext loopConnectionEarlyCtx;
    loopConnectionEarlyCtx.nowMs = nowMs;
    loopConnectionEarlyCtx.nowUs = nowUs;
    loopConnectionEarlyCtx.lastLoopUs = lastLoopUs;
    loopConnectionEarlyCtx.bootSplashHoldActive = currentBootSplashHoldActive;
    loopConnectionEarlyCtx.bootSplashHoldUntilMs = currentBootSplashHoldUntilMs;
    loopConnectionEarlyCtx.initialScanningScreenShown = currentInitialScanningScreenShown;
    const LoopConnectionEarlyResult loopConnectionEarlyResult =
        loopConnectionEarlyModule.process(loopConnectionEarlyCtx);

    LoopConnectionEarlyPhaseValues values;
    values.bootSplashHoldActive = loopConnectionEarlyResult.bootSplashHoldActive;
    values.initialScanningScreenShown = loopConnectionEarlyResult.initialScanningScreenShown;
    values.bleConnectedNow = loopConnectionEarlyResult.bleConnectedNow;
    values.bleBackpressure = loopConnectionEarlyResult.bleBackpressure;
    values.skipNonCoreThisLoop = loopConnectionEarlyResult.skipNonCoreThisLoop;
    values.overloadThisLoop = loopConnectionEarlyResult.overloadThisLoop;
    return values;
}

LoopIngestPhaseValues processLoopIngestPhase(
    const unsigned long nowMs,
    const bool currentBootReady,
    const unsigned long bootReadyDeadlineMs,
    const bool skipNonCoreThisLoop,
    const bool overloadThisLoop) {
    LoopSettingsPrepContext loopSettingsPrepCtx;
    loopSettingsPrepCtx.nowMs = nowMs;
    const LoopSettingsPrepValues loopSettingsPrepValues =
        loopSettingsPrepModule.process(loopSettingsPrepCtx);

    LoopPreIngestContext loopPreIngestCtx;
    loopPreIngestCtx.nowMs = nowMs;
    loopPreIngestCtx.bootReady = currentBootReady;
    loopPreIngestCtx.bootReadyDeadlineMs = bootReadyDeadlineMs;
#ifdef REPLAY_MODE
    loopPreIngestCtx.replayMode = true;
#endif
    const LoopPreIngestResult loopPreIngestResult = loopPreIngestModule.process(loopPreIngestCtx);
    const bool runBleProcessThisLoop = loopPreIngestResult.runBleProcessThisLoop;

    LoopIngestContext loopIngestCtx;
    loopIngestCtx.nowMs = nowMs;
    loopIngestCtx.bleProcessEnabled = runBleProcessThisLoop;
    loopIngestCtx.skipNonCoreThisLoop = skipNonCoreThisLoop;
    loopIngestCtx.overloadThisLoop = overloadThisLoop;
    const LoopIngestResult loopIngestResult = loopIngestModule.process(loopIngestCtx);

    LoopIngestPhaseValues values;
    values.loopSettingsPrepValues = loopSettingsPrepValues;
    values.bootReady = loopPreIngestResult.bootReady;
    values.bleBackpressure = loopIngestResult.bleBackpressure;
    values.skipLateNonCoreThisLoop = loopIngestResult.skipLateNonCoreThisLoop;
    values.overloadLateThisLoop = loopIngestResult.overloadLateThisLoop;
    return values;
}

void processLoopDisplayPreWifiPhase(
    const unsigned long nowMs,
    const bool bootSplashHoldActive,
    const bool overloadLateThisLoop) {

    LoopDisplayContext loopDisplayCtx;
    loopDisplayCtx.nowMs = nowMs;
    loopDisplayCtx.bootSplashHoldActive = bootSplashHoldActive;
    loopDisplayCtx.overloadLateThisLoop = overloadLateThisLoop;
    loopDisplayModule.process(loopDisplayCtx);

    LoopPostDisplayContext loopPostDisplayPreWifiCtx;
    loopPostDisplayPreWifiCtx.enableAutoPush = true;
    loopPostDisplayPreWifiCtx.runSpeedAndDispatch = false;
    loopPostDisplayPreWifiCtx.nowMs = nowMs;
    loopPostDisplayModule.process(loopPostDisplayPreWifiCtx);
}

LoopWifiPhaseValues processLoopWifiPhase(
    const unsigned long nowMs,
    const unsigned long v1ConnectedAtMs,
    const bool enableWifi,
    const bool enableWifiAtBoot,
    const bool currentWifiAutoStartDone,
    const bool skipLateNonCoreThisLoop,
    const bool bleBackpressure,
    const bool overloadLateThisLoop,
    const bool bleConnectBurstSettling,
    const bool bootSplashHoldActive) {
    LoopRuntimeSnapshotContext loopRuntimeSnapshotCtx;
    const LoopRuntimeSnapshotValues loopRuntimeSnapshotValues =
        loopRuntimeSnapshotModule.process(loopRuntimeSnapshotCtx);

    WifiRuntimeContext wifiRuntimeCtx;
    wifiRuntimeCtx.nowMs = nowMs;
    wifiRuntimeCtx.v1ConnectedAtMs = v1ConnectedAtMs;
    wifiRuntimeCtx.enableWifi = enableWifi;
    wifiRuntimeCtx.enableWifiAtBoot = enableWifiAtBoot;
    wifiRuntimeCtx.bleConnected = loopRuntimeSnapshotValues.bleConnected;
    wifiRuntimeCtx.canStartDma = loopRuntimeSnapshotValues.canStartDma;
    wifiRuntimeCtx.wifiAutoStartDone = currentWifiAutoStartDone;
    wifiRuntimeCtx.skipLateNonCoreThisLoop = skipLateNonCoreThisLoop;
    wifiRuntimeCtx.bleBackpressure = bleBackpressure;
    wifiRuntimeCtx.overloadLateThisLoop = overloadLateThisLoop;
    wifiRuntimeCtx.bleConnectBurstSettling = bleConnectBurstSettling;
    wifiRuntimeCtx.displayPreviewRunning = loopRuntimeSnapshotValues.displayPreviewRunning;
    wifiRuntimeCtx.bootSplashHoldActive = bootSplashHoldActive;
    const WifiRuntimeResult wifiRuntimeResult = wifiRuntimeModule.process(wifiRuntimeCtx);

    LoopWifiPhaseValues values;
    values.loopRuntimeSnapshotValues = loopRuntimeSnapshotValues;
    values.wifiAutoStartDone = wifiRuntimeResult.wifiAutoStartDone;
    return values;
}

LoopFinalizePhaseValues processLoopFinalizePhase(
    const unsigned long nowMs,
    const bool bootSplashHoldActive,
    const bool displayPreviewRunning,
    const bool bleBackpressure,
    const unsigned long scanScreenDwellMs,
    const unsigned long connectionStateProcessMaxGapMs,
    const unsigned long loopStartUs) {
    LoopPostDisplayContext loopPostDisplayPostWifiCtx;
    loopPostDisplayPostWifiCtx.enableAutoPush = false;
    loopPostDisplayPostWifiCtx.runSpeedAndDispatch = true;
    loopPostDisplayPostWifiCtx.nowMs = nowMs;
    loopPostDisplayPostWifiCtx.displayUpdateIntervalMs = DISPLAY_UPDATE_MS;
    loopPostDisplayPostWifiCtx.scanScreenDwellMs = scanScreenDwellMs;
    loopPostDisplayPostWifiCtx.bootSplashHoldActive = bootSplashHoldActive;
    loopPostDisplayPostWifiCtx.displayPreviewRunning = displayPreviewRunning;
    loopPostDisplayPostWifiCtx.maxProcessGapMs = connectionStateProcessMaxGapMs;
    const LoopPostDisplayResult loopPostDisplayResult =
        loopPostDisplayModule.process(loopPostDisplayPostWifiCtx);

    periodicMaintenanceModule.process(loopPostDisplayResult.dispatchNowMs);

    LoopFinalizePhaseValues values;
    values.dispatchNowMs = loopPostDisplayResult.dispatchNowMs;
    values.bleConnectedNow = loopPostDisplayResult.bleConnectedNow;
    values.lastLoopUs = loopTailModule.process(bleBackpressure, loopStartUs);
    return values;
}

bool shouldReturnEarlyFromLoopPowerTouchPhase(const unsigned long nowMs,
                                              const unsigned long loopStartUs) {
    LoopPowerTouchContext loopPowerTouchCtx;
    loopPowerTouchCtx.nowMs = nowMs;
    loopPowerTouchCtx.loopStartUs = loopStartUs;
    loopPowerTouchCtx.bootButtonPressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);
    const LoopPowerTouchResult loopPowerTouchResult = loopPowerTouchModule.process(loopPowerTouchCtx);
    return loopPowerTouchResult.shouldReturnEarly;
}

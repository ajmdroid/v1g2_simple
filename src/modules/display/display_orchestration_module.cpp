#include "display_orchestration_module.h"

#ifndef UNIT_TEST
#include "display.h"
#include "ble_client.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/display/display_preview_module.h"
#include "modules/display/display_restore_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/speed_mute/speed_mute_module.h"
#include "packet_parser.h"
#include "settings.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/lockout/lockout_orchestration_module.h"
#include "perf_metrics.h"
#endif

void DisplayOrchestrationModule::begin(V1Display* displayPtr,
                                       V1BLEClient* bleClient,
                                       BleQueueModule* bleQueueModule,
                                       DisplayPreviewModule* previewModule,
                                       DisplayRestoreModule* restoreModule,
                                       PacketParser* parserPtr,
                                       SettingsManager* settingsManager,
                                       GpsRuntimeModule* gpsModule,
                                       LockoutOrchestrationModule* lockoutModule,
                                       VolumeFadeModule* volumeFadeModule,
                                       SpeedMuteModule* speedMuteModule) {
    display = displayPtr;
    ble = bleClient;
    bleQueue = bleQueueModule;
    preview = previewModule;
    restore = restoreModule;
    parser = parserPtr;
    settings = settingsManager;
    gpsRuntime = gpsModule;
    lockout = lockoutModule;
    volumeFade = volumeFadeModule;
    speedMute = speedMuteModule;
    reset();
}

void DisplayOrchestrationModule::reset() {
    lastGpsSatUpdateMs = 0;
    lastFreqUiMs = 0;
    lastCardUiMs = 0;
    pendingPqRestoreVol_ = 0xFF;
    pendingPqRestoreMuteVol_ = 0;
    pendingPqRestoreSetMs_ = 0;
    pendingPqRestoreLastRetryMs_ = 0;
    speedVolActive_ = false;
    speedVolSavedOriginal_ = 0xFF;
    speedVolSavedMuteVol_ = 0;
    pendingSpeedVolRestoreVol_ = 0xFF;
    pendingSpeedVolRestoreMuteVol_ = 0;
    pendingSpeedVolRestoreSetMs_ = 0;
    pendingSpeedVolRestoreLastRetryMs_ = 0;
    speedVolLastRetryMs_ = 0;
}

bool DisplayOrchestrationModule::executeLockoutVolumeCommand(const LockoutVolumeCommand& command,
                                                             const uint32_t nowMs) {
    if (!ble || !command.hasAction()) {
        return false;
    }

    const bool sent = ble->setVolume(command.volume, command.muteVolume);
    if (command.type == LockoutVolumeCommandType::PreQuietRestore) {
        if (volumeFade) {
            volumeFade->setBaselineHint(command.volume, command.muteVolume, nowMs);
        }
        // Always arm the pending-restore tracker so we confirm V1 echoes
        // the correct volume.  If setVolume was paced/dropped, the retry
        // loop in retryPendingPreQuietRestore() will resend.
        pendingPqRestoreVol_ = command.volume;
        pendingPqRestoreMuteVol_ = command.muteVolume;
        pendingPqRestoreSetMs_ = nowMs;
        pendingPqRestoreLastRetryMs_ = nowMs;
#ifndef UNIT_TEST
        perfRecordPreQuietRestore();
#endif
        Serial.println("[Lockout] PRE-QUIET: volume restored");
        return true;
    }

    if (command.type == LockoutVolumeCommandType::PreQuietDrop) {
        // Successful drop clears any pending restore (volume was re-dropped).
        pendingPqRestoreVol_ = 0xFF;
#ifndef UNIT_TEST
        perfRecordPreQuietDrop();
#endif
        Serial.println("[Lockout] PRE-QUIET: volume dropped in lockout zone");
    }

    return true;
}

bool DisplayOrchestrationModule::retryPendingPreQuietRestore(const uint32_t nowMs) {
    if (pendingPqRestoreVol_ == 0xFF || !ble || !parser) {
        return false;
    }

    // Timeout: give up after PQ_RESTORE_TIMEOUT_MS to avoid infinite retries.
    if ((nowMs - pendingPqRestoreSetMs_) >= PQ_RESTORE_TIMEOUT_MS) {
        Serial.println("[Lockout] PRE-QUIET: restore retry timeout");
        pendingPqRestoreVol_ = 0xFF;
        return false;
    }

    // Check if V1 volume already matches the target (confirmed).
    const DisplayState state = parser->getDisplayState();
    if (state.mainVolume == pendingPqRestoreVol_) {
        pendingPqRestoreVol_ = 0xFF;
        return false;
    }

    // Pace retries at PQ_RESTORE_RETRY_INTERVAL_MS.
    if ((nowMs - pendingPqRestoreLastRetryMs_) < PQ_RESTORE_RETRY_INTERVAL_MS) {
        return true;  // Still pending but not retrying this frame
    }

    pendingPqRestoreLastRetryMs_ = nowMs;
    ble->setVolume(pendingPqRestoreVol_, pendingPqRestoreMuteVol_);
#ifndef UNIT_TEST
    perfRecordPreQuietRestoreRetry();
#endif
    return true;
}

bool DisplayOrchestrationModule::processSpeedVolume(const uint32_t nowMs) {
    if (!speedMute || !ble || !parser) {
        return retryPendingSpeedVolRestore(nowMs);
    }

    const auto& smSettings = speedMute->getSettings();
    const auto& smState = speedMute->getState();

    // Feature not configured for V1 volume control.
    if (smSettings.v1Volume == 0xFF) {
        if (speedVolActive_) {
            // Settings changed at runtime — restore and exit.
            ble->setVolume(speedVolSavedOriginal_, speedVolSavedMuteVol_);
            if (volumeFade) {
                volumeFade->setBaselineHint(speedVolSavedOriginal_, speedVolSavedMuteVol_, nowMs);
            }
            if (lockout) lockout->clearVolumeHint();
            pendingSpeedVolRestoreVol_ = speedVolSavedOriginal_;
            pendingSpeedVolRestoreMuteVol_ = speedVolSavedMuteVol_;
            pendingSpeedVolRestoreSetMs_ = nowMs;
            pendingSpeedVolRestoreLastRetryMs_ = nowMs;
            speedVolActive_ = false;
            speedVolSavedOriginal_ = 0xFF;
#ifndef UNIT_TEST
            perfRecordSpeedVolRestore();
#endif
        }
        return retryPendingSpeedVolRestore(nowMs);
    }

    bool wantsActive = smState.muteActive;

    // Band override: Laser and Ka always force restore so the user
    // hears the real threat at full volume.
    if (wantsActive && parser->hasAlerts()) {
        const DisplayState ds = parser->getDisplayState();
        const bool laserOrKa = (ds.activeBands & 0x03) != 0; // BAND_LASER|BAND_KA
        if (laserOrKa) {
            wantsActive = false;
        }
    }

    // Pre-quiet busy = pre-quiet in DROPPED phase or pending PQ restore.
    const bool pqBusy = (lockout && lockout->isPreQuietActive()) ||
                        pendingPqRestoreVol_ != 0xFF;

    // --- Activation: not active → wants active ---
    if (wantsActive && !speedVolActive_) {
        if (pqBusy) return false;  // Pre-quiet owns volume, defer.
        // Cancel any pending restore from a previous cycle.
        pendingSpeedVolRestoreVol_ = 0xFF;

        const DisplayState ds = parser->getDisplayState();
        speedVolSavedOriginal_ = ds.mainVolume;
        speedVolSavedMuteVol_ = ds.muteVolume;
        speedVolActive_ = true;
        speedVolLastRetryMs_ = nowMs;
        ble->setVolume(smSettings.v1Volume, speedVolSavedMuteVol_);
        // Set hint so pre-quiet captures the user's true volume.
        if (lockout) lockout->setVolumeHint(speedVolSavedOriginal_, speedVolSavedMuteVol_);
#ifndef UNIT_TEST
        perfRecordSpeedVolDrop();
#endif
        Serial.printf("[SpeedVol] DROP: %d -> %d\n", speedVolSavedOriginal_, smSettings.v1Volume);
        return true;
    }

    // --- Deactivation: active → doesn't want ---
    if (!wantsActive && speedVolActive_) {
        ble->setVolume(speedVolSavedOriginal_, speedVolSavedMuteVol_);
        if (volumeFade) {
            volumeFade->setBaselineHint(speedVolSavedOriginal_, speedVolSavedMuteVol_, nowMs);
        }
        if (lockout) lockout->clearVolumeHint();
        pendingSpeedVolRestoreVol_ = speedVolSavedOriginal_;
        pendingSpeedVolRestoreMuteVol_ = speedVolSavedMuteVol_;
        pendingSpeedVolRestoreSetMs_ = nowMs;
        pendingSpeedVolRestoreLastRetryMs_ = nowMs;
#ifndef UNIT_TEST
        perfRecordSpeedVolRestore();
#endif
        Serial.printf("[SpeedVol] RESTORE: -> %d\n", speedVolSavedOriginal_);
        speedVolActive_ = false;
        speedVolSavedOriginal_ = 0xFF;
        return retryPendingSpeedVolRestore(nowMs);
    }

    // --- Steady state: active and wants active ---
    if (speedVolActive_) {
        if (pqBusy) return true;  // Pre-quiet overrides, hold state.
        // Check V1 confirms our target volume.
        const DisplayState ds = parser->getDisplayState();
        if (ds.mainVolume == smSettings.v1Volume) return true;  // Confirmed.
        // Rate-limited retry.
        if ((nowMs - speedVolLastRetryMs_) >= SPEED_VOL_RETRY_INTERVAL_MS) {
            speedVolLastRetryMs_ = nowMs;
            ble->setVolume(smSettings.v1Volume, speedVolSavedMuteVol_);
#ifndef UNIT_TEST
            perfRecordSpeedVolRetry();
#endif
        }
        return true;
    }

    return retryPendingSpeedVolRestore(nowMs);
}

bool DisplayOrchestrationModule::retryPendingSpeedVolRestore(const uint32_t nowMs) {
    if (pendingSpeedVolRestoreVol_ == 0xFF || !ble || !parser) {
        return false;
    }

    // Timeout.
    if ((nowMs - pendingSpeedVolRestoreSetMs_) >= SPEED_VOL_RESTORE_TIMEOUT_MS) {
        Serial.println("[SpeedVol] restore retry timeout");
        pendingSpeedVolRestoreVol_ = 0xFF;
        return false;
    }

    // Confirmed.
    const DisplayState ds = parser->getDisplayState();
    if (ds.mainVolume == pendingSpeedVolRestoreVol_) {
        pendingSpeedVolRestoreVol_ = 0xFF;
        return false;
    }

    // Pace retries.
    if ((nowMs - pendingSpeedVolRestoreLastRetryMs_) < SPEED_VOL_RETRY_INTERVAL_MS) {
        return true;
    }

    pendingSpeedVolRestoreLastRetryMs_ = nowMs;
    ble->setVolume(pendingSpeedVolRestoreVol_, pendingSpeedVolRestoreMuteVol_);
#ifndef UNIT_TEST
    perfRecordSpeedVolRetry();
#endif
    return true;
}

void DisplayOrchestrationModule::executeVolumeFade(const uint32_t nowMs,
                                                   const bool lockoutPrioritySuppressed) {
    if (!ble || !parser || !volumeFade) {
        return;
    }

    const DisplayState state = parser->getDisplayState();
    const bool hasAlerts = parser->hasAlerts();
    AlertData priority;
    const bool hasRenderablePriority =
        hasAlerts && parser->getRenderablePriorityAlert(priority);

    VolumeFadeContext fadeCtx;
    fadeCtx.hasAlert = hasAlerts;
    fadeCtx.currentVolume = state.mainVolume;
    fadeCtx.currentMuteVolume = state.muteVolume;
    fadeCtx.now = nowMs;
    if (hasAlerts) {
        fadeCtx.alertMuted = state.muted;
        fadeCtx.alertSuppressed = lockoutPrioritySuppressed;
        fadeCtx.currentFrequency =
            hasRenderablePriority ? static_cast<uint16_t>(priority.frequency) : 0;
    }

    const VolumeFadeAction fadeAction = volumeFade->process(fadeCtx);
    if (!fadeAction.hasAction()) {
        return;
    }

    if (fadeAction.type == VolumeFadeAction::Type::FADE_DOWN) {
        ble->setVolume(fadeAction.targetVolume, fadeAction.targetMuteVolume);
    } else if (fadeAction.type == VolumeFadeAction::Type::RESTORE) {
        ble->setVolume(fadeAction.restoreVolume, fadeAction.restoreMuteVolume);
    }
}

void DisplayOrchestrationModule::processEarly(const DisplayOrchestrationEarlyContext& ctx) {
    if (!display || !preview || !restore) {
        return;
    }

    if (!ctx.overloadThisLoop && !ctx.bootSplashHoldActive) {
        display->setBleContext(ctx.bleContext);
        display->setBLEProxyStatus(ctx.bleContext.v1Connected,
                                   ctx.bleContext.proxyConnected,
                                   ctx.bleReceiving);
    }

    if (!ctx.overloadThisLoop && !ctx.bootSplashHoldActive) {
        if (preview->isRunning()) {
            preview->update();
        } else {
            restore->process();
        }
    }
}

DisplayOrchestrationParsedResult DisplayOrchestrationModule::processParsedFrame(
        const DisplayOrchestrationParsedContext& ctx) {
    DisplayOrchestrationParsedResult result;
    if (!display || !ble || !bleQueue || !preview || !parser || !settings ||
        !gpsRuntime || !lockout) {
        return result;
    }

    if (ctx.parsedReady && !ctx.bootSplashHoldActive) {
        const GpsRuntimeStatus gpsStatus = gpsRuntime->snapshot(ctx.nowMs);
        const bool proxyClientConnected = ble->isProxyClientConnected();
        const auto lockoutResult = lockout->process(
            ctx.nowMs,
            gpsStatus,
            proxyClientConnected,
            ctx.enableSignalTraceLogging);

        result.lockoutEvaluated = true;
        result.lockoutPrioritySuppressed = lockoutResult.prioritySuppressed;
        const bool lockoutVolumeCommandExecuted =
            executeLockoutVolumeCommand(lockoutResult.volumeCommand, ctx.nowMs);

        // Retry any pending pre-quiet restore that hasn't been confirmed yet.
        const bool pqRestorePending = retryPendingPreQuietRestore(ctx.nowMs);

        // Speed volume: lower/restore V1 volume based on speed mute state.
        // Defers to pre-quiet when it owns volume. Gates volume fade.
        const bool speedVolBusy = processSpeedVolume(ctx.nowMs);

        // Suppress VOL 0 warning when speed-mute intentionally set volume to 0.
        {
            const bool speedVolZero = speedVolActive_ && speedMute &&
                                      speedMute->getSettings().v1Volume == 0;
            display->setSpeedVolZeroActive(speedVolZero);
        }

        if (lastGpsSatUpdateMs == 0 ||
            (ctx.nowMs - lastGpsSatUpdateMs >= GPS_SAT_UPDATE_INTERVAL_MS)) {
            display->setGpsSatellites(gpsStatus.enabled,
                                      gpsStatus.stableHasFix,
                                      gpsStatus.stableSatellites);
            lastGpsSatUpdateMs = ctx.nowMs;
        }

        result.runDisplayPipeline = !preview->isRunning();
        if (result.runDisplayPipeline && !lockoutVolumeCommandExecuted &&
            !pqRestorePending && !speedVolBusy) {
            executeVolumeFade(ctx.nowMs, result.lockoutPrioritySuppressed);
        }
        return result;
    }

    if (!ctx.bootSplashHoldActive) {
        const uint32_t lastParsedMs = bleQueue->getLastParsedTimestamp();
        if (!ble->isConnected() ||
            lastParsedMs == 0 ||
            static_cast<uint32_t>(ctx.nowMs - lastParsedMs) > LOCKOUT_INDICATOR_STALE_MS) {
            display->setLockoutIndicator(false);
        }
    }

    return result;
}

DisplayOrchestrationRefreshResult DisplayOrchestrationModule::processLightweightRefresh(
        const DisplayOrchestrationRefreshContext& ctx) {
    DisplayOrchestrationRefreshResult result;
    if (!display || !ble || !preview || !parser) {
        return result;
    }

    if (ctx.pipelineRanThisLoop) {
        lastFreqUiMs = ctx.nowMs;
    }

    const bool loopHasAlerts = parser->hasAlerts();
    AlertData loopPriority;
    const bool loopHasRenderablePriority =
        loopHasAlerts && parser->getRenderablePriorityAlert(loopPriority);
    result.signalPriorityActive = loopHasRenderablePriority;

    const bool previewRunning = preview->isRunning();
    const unsigned long freqUiMaxMs = previewRunning ? FREQ_UI_PREVIEW_MAX_MS : FREQ_UI_MAX_MS;
    if (!ctx.bootSplashHoldActive &&
        ble->isConnected() &&
        !previewRunning &&
        (ctx.nowMs - lastFreqUiMs) >= freqUiMaxMs) {
        const DisplayState& state = parser->getDisplayState();
        const bool isPhotoRadar =
            (loopPriority.photoType != 0) ||
            state.hasPhotoAlert ||
            (state.bogeyCounterChar == 'P');
        if (result.signalPriorityActive) {
            display->refreshFrequencyOnly(loopPriority.frequency,
                                          loopPriority.band,
                                          state.muted,
                                          isPhotoRadar);
        } else {
            display->refreshFrequencyOnly(0, BAND_NONE, false, false);
        }
        lastFreqUiMs = ctx.nowMs;
    }

    if (!ctx.bootSplashHoldActive &&
        ble->isConnected() &&
        !previewRunning &&
        !ctx.pipelineRanThisLoop &&
        ctx.overloadLateThisLoop &&
        (ctx.nowMs - lastCardUiMs) >= CARD_UI_MAX_MS) {
        const auto& allAlerts = parser->getAllAlerts();
        const int alertCount = static_cast<int>(parser->getAlertCount());
        const DisplayState& state = parser->getDisplayState();
        display->refreshSecondaryAlertCards(allAlerts.data(),
                                            alertCount,
                                            loopPriority,
                                            state.muted);
        lastCardUiMs = ctx.nowMs;
    }

    return result;
}

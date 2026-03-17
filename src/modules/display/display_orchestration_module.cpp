#include "display_orchestration_module.h"

#ifndef UNIT_TEST
#include "display.h"
#include "ble_client.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/display/display_preview_module.h"
#include "modules/display/display_restore_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "packet_parser.h"
#include "settings.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/lockout/lockout_orchestration_module.h"
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
                                       VolumeFadeModule* volumeFadeModule) {
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
    reset();
}

void DisplayOrchestrationModule::reset() {
    lastGpsSatUpdateMs = 0;
    lastFreqUiMs = 0;
    lastCardUiMs = 0;
}

bool DisplayOrchestrationModule::executeLockoutVolumeCommand(const LockoutVolumeCommand& command,
                                                             const uint32_t nowMs) {
    if (!ble || !command.hasAction()) {
        return false;
    }

    ble->setVolume(command.volume, command.muteVolume);
    if (command.type == LockoutVolumeCommandType::PreQuietRestore) {
        if (volumeFade) {
            volumeFade->setBaselineHint(command.volume, command.muteVolume, nowMs);
        }
        Serial.println("[Lockout] PRE-QUIET: volume restored");
        return true;
    }

    if (command.type == LockoutVolumeCommandType::PreQuietDrop) {
        Serial.println("[Lockout] PRE-QUIET: volume dropped in lockout zone");
    }

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

        if (lastGpsSatUpdateMs == 0 ||
            (ctx.nowMs - lastGpsSatUpdateMs >= GPS_SAT_UPDATE_INTERVAL_MS)) {
            display->setGpsSatellites(gpsStatus.enabled,
                                      gpsStatus.stableHasFix,
                                      gpsStatus.stableSatellites);
            lastGpsSatUpdateMs = ctx.nowMs;
        }

        result.runDisplayPipeline = !preview->isRunning();
        if (result.runDisplayPipeline && !lockoutVolumeCommandExecuted) {
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

#include "display_orchestration_module.h"
#include "modules/quiet/quiet_coordinator_module.h"

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
#include "perf_metrics.h"
#endif

#include "modules/quiet/quiet_coordinator_templates.h"

void DisplayOrchestrationModule::begin(V1Display* displayPtr,
                                       V1BLEClient* bleClient,
                                       BleQueueModule* bleQueueModule,
                                       DisplayPreviewModule* previewModule,
                                       DisplayRestoreModule* restoreModule,
                                       PacketParser* parserPtr,
                                       SettingsManager* settingsManager,
                                       VolumeFadeModule* volumeFadeModule,
                                       SpeedMuteModule* speedMuteModule,
                                       QuietCoordinatorModule* quietCoordinator) {
    display = displayPtr;
    ble = bleClient;
    bleQueue = bleQueueModule;
    preview = previewModule;
    restore = restoreModule;
    parser = parserPtr;
    settings = settingsManager;
    volumeFade = volumeFadeModule;
    speedMute = speedMuteModule;
    quiet = quietCoordinator;
    reset();
}

void DisplayOrchestrationModule::reset() {
    lastFreqUiMs = 0;
    lastCardUiMs = 0;
}

void DisplayOrchestrationModule::syncQuietPresentation() {
    if (!display || !quiet) {
        return;
    }

    const QuietPresentationState& presentation = quiet->getPresentationState();
    display->setSpeedVolZeroActive(presentation.speedVolZeroActive);
}

bool DisplayOrchestrationModule::processSpeedVolume(const uint32_t nowMs) {
    if (!quiet || !speedMute) {
        return false;
    }
    return quiet->processSpeedVolume(nowMs, *speedMute, volumeFade);
}

bool DisplayOrchestrationModule::retryPendingSpeedVolRestore(const uint32_t nowMs) {
    return quiet && quiet->retryPendingSpeedVolRestore(nowMs);
}

void DisplayOrchestrationModule::executeVolumeFade(const uint32_t nowMs) {
    if (quiet) {
        (void)quiet->executeVolumeFade(nowMs, volumeFade);
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
    if (!display || !ble || !bleQueue || !preview || !parser || !settings) {
        return result;
    }

    if (ctx.parsedReady && !ctx.bootSplashHoldActive) {
        // Speed volume: lower/restore V1 volume based on speed mute state.
        // Gates volume fade.
        const bool speedVolBusy = processSpeedVolume(ctx.nowMs);

        syncQuietPresentation();

        result.runDisplayPipeline = !preview->isRunning();
        if (result.runDisplayPipeline && !speedVolBusy) {
            executeVolumeFade(ctx.nowMs);
        }
        return result;
    }

    syncQuietPresentation();

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

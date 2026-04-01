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
    display_ = displayPtr;
    ble_ = bleClient;
    bleQueue_ = bleQueueModule;
    preview_ = previewModule;
    restore_ = restoreModule;
    parser_ = parserPtr;
    settings_ = settingsManager;
    volumeFade_ = volumeFadeModule;
    speedMute_ = speedMuteModule;
    quiet_ = quietCoordinator;
    reset();
}

void DisplayOrchestrationModule::reset() {
    lastFreqUiMs_ = 0;
    lastCardUiMs_ = 0;
}

void DisplayOrchestrationModule::syncQuietPresentation() {
    if (!display_ || !quiet_) {
        return;
    }

    const QuietPresentationState& presentation = quiet_->getPresentationState();
    display_->setSpeedVolZeroActive(presentation.speedVolZeroActive);
}

bool DisplayOrchestrationModule::processSpeedVolume(const uint32_t nowMs) {
    if (!quiet_ || !speedMute_) {
        return false;
    }
    return quiet_->processSpeedVolume(nowMs, *speedMute_, volumeFade_);
}

bool DisplayOrchestrationModule::retryPendingSpeedVolRestore(const uint32_t nowMs) {
    return quiet_ && quiet_->retryPendingSpeedVolRestore(nowMs);
}

void DisplayOrchestrationModule::executeVolumeFade(const uint32_t nowMs) {
    if (quiet_) {
        (void)quiet_->executeVolumeFade(nowMs, volumeFade_);
    }
}

void DisplayOrchestrationModule::processEarly(const DisplayOrchestrationEarlyContext& ctx) {
    if (!display_ || !preview_ || !restore_) {
        return;
    }

    if (!ctx.overloadThisLoop && !ctx.bootSplashHoldActive) {
        display_->setBleContext(ctx.bleContext);
        display_->setBLEProxyStatus(ctx.bleContext.v1Connected,
                                   ctx.bleContext.proxyConnected,
                                   ctx.bleReceiving);
    }

    if (!ctx.overloadThisLoop && !ctx.bootSplashHoldActive) {
        if (preview_->isRunning()) {
            preview_->update();
        } else {
            restore_->process();
        }
    }
}

DisplayOrchestrationParsedResult DisplayOrchestrationModule::processParsedFrame(
        const DisplayOrchestrationParsedContext& ctx) {
    DisplayOrchestrationParsedResult result;
    if (!display_ || !ble_ || !bleQueue_ || !preview_ || !parser_ || !settings_) {
        return result;
    }

    if (ctx.parsedReady && !ctx.bootSplashHoldActive) {
        // Speed volume: lower/restore V1 volume based on speed mute state.
        // Gates volume fade.
        const bool speedVolBusy = processSpeedVolume(ctx.nowMs);

        syncQuietPresentation();

        result.runDisplayPipeline = !preview_->isRunning();
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
    if (!display_ || !ble_ || !preview_ || !parser_) {
        return result;
    }

    if (ctx.pipelineRanThisLoop) {
        lastFreqUiMs_ = ctx.nowMs;
    }

    const bool loopHasAlerts = parser_->hasAlerts();
    AlertData loopPriority;
    const bool loopHasRenderablePriority =
        loopHasAlerts && parser_->getRenderablePriorityAlert(loopPriority);
    result.signalPriorityActive = loopHasRenderablePriority;

    const bool previewRunning = preview_->isRunning();
    const unsigned long freqUiMaxMs = previewRunning ? FREQ_UI_PREVIEW_MAX_MS : FREQ_UI_MAX_MS;
    if (!ctx.bootSplashHoldActive &&
        ble_->isConnected() &&
        !previewRunning &&
        (ctx.nowMs - lastFreqUiMs_) >= freqUiMaxMs) {
        const DisplayState& state = parser_->getDisplayState();
        const bool isPhotoRadar =
            (loopPriority.photoType != 0) ||
            state.hasPhotoAlert ||
            (state.bogeyCounterChar == 'P');
        if (result.signalPriorityActive) {
            display_->refreshFrequencyOnly(loopPriority.frequency,
                                          loopPriority.band,
                                          state.muted,
                                          isPhotoRadar);
        } else {
            display_->refreshFrequencyOnly(0, BAND_NONE, false, false);
        }
        lastFreqUiMs_ = ctx.nowMs;
    }

    if (!ctx.bootSplashHoldActive &&
        ble_->isConnected() &&
        !previewRunning &&
        !ctx.pipelineRanThisLoop &&
        ctx.overloadLateThisLoop &&
        (ctx.nowMs - lastCardUiMs_) >= CARD_UI_MAX_MS) {
        const auto& allAlerts = parser_->getAllAlerts();
        const int alertCount = static_cast<int>(parser_->getAlertCount());
        const DisplayState& state = parser_->getDisplayState();
        display_->refreshSecondaryAlertCards(allAlerts.data(),
                                            alertCount,
                                            loopPriority,
                                            state.muted);
        lastCardUiMs_ = ctx.nowMs;
    }

    return result;
}

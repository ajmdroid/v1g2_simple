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

    // Report whether a renderable priority alert exists.
    // No lightweight refresh paths — element caches handle all optimization.
    const bool loopHasAlerts = parser_->hasAlerts();
    AlertData loopPriority;
    const bool loopHasRenderablePriority =
        loopHasAlerts && parser_->getRenderablePriorityAlert(loopPriority);
    result.signalPriorityActive = loopHasRenderablePriority;

    return result;
}

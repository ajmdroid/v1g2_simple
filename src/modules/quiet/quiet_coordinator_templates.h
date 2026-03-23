#pragma once

#include "quiet_coordinator_module.h"

template <typename VolumeFadeLike>
bool QuietCoordinatorModule::handleLockoutVolumeCommand(const LockoutVolumeCommand& command,
                                                        const uint32_t nowMs,
                                                        VolumeFadeLike* volumeFade) {
    if (!command.hasAction()) {
        return false;
    }

    sendVolume(QuietOwner::PreQuiet, command.volume, command.muteVolume);
    if (command.type == LockoutVolumeCommandType::PreQuietRestore) {
        if (volumeFade) {
            volumeFade->setBaselineHint(command.volume, command.muteVolume, nowMs);
        }
        pendingPqRestoreVol_ = command.volume;
        pendingPqRestoreMuteVol_ = command.muteVolume;
        pendingPqRestoreSetMs_ = nowMs;
        pendingPqRestoreLastRetryMs_ = nowMs;
#ifndef UNIT_TEST
        perfRecordPreQuietRestore();
#endif
        Serial.println("[Lockout] PRE-QUIET: volume restored");
        presentation_.activeVolumeOwner = QuietOwner::PreQuiet;
        return true;
    }

    if (command.type == LockoutVolumeCommandType::PreQuietDrop) {
        pendingPqRestoreVol_ = 0xFF;
#ifndef UNIT_TEST
        perfRecordPreQuietDrop();
#endif
        Serial.println("[Lockout] PRE-QUIET: volume dropped in lockout zone");
        presentation_.activeVolumeOwner = QuietOwner::PreQuiet;
    }

    return true;
}

template <typename SpeedMuteLike>
void QuietCoordinatorModule::updateSpeedVolPresentation(const SpeedMuteLike* speedMute) {
    presentation_.speedVolZeroActive =
        speedVolActive_ && speedMute && speedMute->getSettings().v1Volume == 0;
    if (speedVolActive_ || pendingSpeedVolRestoreVol_ != 0xFF) {
        presentation_.activeVolumeOwner = QuietOwner::SpeedVolume;
    } else if (presentation_.activeVolumeOwner == QuietOwner::SpeedVolume) {
        presentation_.activeVolumeOwner = presentation_.preQuietActive ? QuietOwner::PreQuiet
                                                                        : QuietOwner::None;
    }
}

template <typename SpeedMuteLike, typename LockoutLike, typename VolumeFadeLike>
bool QuietCoordinatorModule::processSpeedVolume(const uint32_t nowMs,
                                                const SpeedMuteLike& speedMute,
                                                LockoutLike* lockout,
                                                VolumeFadeLike* volumeFade) {
    syncCommittedState();

    const auto& smSettings = speedMute.getSettings();
    const auto& smState = speedMute.getState();

    if (smSettings.v1Volume == 0xFF) {
        if (speedVolActive_) {
            sendVolume(QuietOwner::SpeedVolume, speedVolSavedOriginal_, speedVolSavedMuteVol_);
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
        updateSpeedVolPresentation(&speedMute);
        return retryPendingSpeedVolRestore(nowMs);
    }

    bool wantsActive = smState.muteActive;
    if (wantsActive && parser_ && parser_->hasAlerts()) {
        const DisplayState& ds = parser_->getDisplayState();
        const bool laserOrKa = (ds.activeBands & 0x03) != 0;
        if (laserOrKa) {
            wantsActive = false;
        }
    }

    const bool pqBusy = (lockout && lockout->isPreQuietActive()) || pendingPqRestoreVol_ != 0xFF;

    if (wantsActive && !speedVolActive_) {
        if (pqBusy) {
            updateSpeedVolPresentation(&speedMute);
            return false;
        }

        pendingSpeedVolRestoreVol_ = 0xFF;
        const DisplayState& ds = parser_->getDisplayState();
        speedVolSavedOriginal_ = ds.mainVolume;
        speedVolSavedMuteVol_ = ds.muteVolume;
        speedVolActive_ = true;
        speedVolLastRetryMs_ = nowMs;
        sendVolume(QuietOwner::SpeedVolume, smSettings.v1Volume, speedVolSavedMuteVol_);
        if (lockout) lockout->setVolumeHint(speedVolSavedOriginal_, speedVolSavedMuteVol_);
#ifndef UNIT_TEST
        perfRecordSpeedVolDrop();
#endif
        Serial.printf("[SpeedVol] DROP: %d -> %d\n", speedVolSavedOriginal_, smSettings.v1Volume);
        updateSpeedVolPresentation(&speedMute);
        return true;
    }

    if (!wantsActive && speedVolActive_) {
        sendVolume(QuietOwner::SpeedVolume, speedVolSavedOriginal_, speedVolSavedMuteVol_);
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
        updateSpeedVolPresentation(&speedMute);
        return retryPendingSpeedVolRestore(nowMs);
    }

    if (speedVolActive_) {
        if (pqBusy) {
            updateSpeedVolPresentation(&speedMute);
            return true;
        }
        if (committed_.mainVolume == smSettings.v1Volume) {
            updateSpeedVolPresentation(&speedMute);
            return true;
        }
        if ((nowMs - speedVolLastRetryMs_) >= SPEED_VOL_RETRY_INTERVAL_MS) {
            speedVolLastRetryMs_ = nowMs;
            sendVolume(QuietOwner::SpeedVolume, smSettings.v1Volume, speedVolSavedMuteVol_);
#ifndef UNIT_TEST
            perfRecordSpeedVolRetry();
#endif
        }
        updateSpeedVolPresentation(&speedMute);
        return true;
    }

    updateSpeedVolPresentation(&speedMute);
    return retryPendingSpeedVolRestore(nowMs);
}

template <typename VolumeFadeLike>
bool QuietCoordinatorModule::executeVolumeFade(const uint32_t nowMs,
                                               const bool lockoutPrioritySuppressed,
                                               VolumeFadeLike* volumeFade) {
    syncCommittedState();
    if (!volumeFade || !parser_) {
        return false;
    }

    const bool hasAlerts = parser_->hasAlerts();
    AlertData priority;
    const bool hasRenderablePriority =
        hasAlerts && parser_->getRenderablePriorityAlert(priority);

    VolumeFadeContext fadeCtx;
    fadeCtx.hasAlert = hasAlerts;
    fadeCtx.currentVolume = committed_.mainVolume;
    fadeCtx.currentMuteVolume = committed_.muteVolume;
    fadeCtx.now = nowMs;
    if (hasAlerts) {
        fadeCtx.alertMuted = committed_.muted;
        fadeCtx.alertSuppressed = lockoutPrioritySuppressed;
        fadeCtx.currentFrequency =
            hasRenderablePriority ? static_cast<uint16_t>(priority.frequency) : 0;
    }

    const VolumeFadeAction fadeAction = volumeFade->process(fadeCtx);
    if (!fadeAction.hasAction()) {
        return false;
    }

    if (fadeAction.type == VolumeFadeAction::Type::FADE_DOWN) {
        sendVolume(QuietOwner::VolumeFade,
                   fadeAction.targetVolume,
                   fadeAction.targetMuteVolume);
        return true;
    }
    if (fadeAction.type == VolumeFadeAction::Type::RESTORE) {
        sendVolume(QuietOwner::VolumeFade,
                   fadeAction.restoreVolume,
                   fadeAction.restoreMuteVolume);
        return true;
    }
    return false;
}

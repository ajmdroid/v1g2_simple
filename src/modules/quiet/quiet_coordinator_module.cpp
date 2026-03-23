#include "quiet_coordinator_module.h"

#ifndef UNIT_TEST
#include "../../ble_client.h"
#include "../../packet_parser.h"
#include "../../perf_metrics.h"
#include "../voice/voice_module.h"
#else
#include "../../../test/mocks/ble_client.h"
#include "../../../test/mocks/packet_parser.h"
#include "../voice/voice_module.h"
#endif

void QuietCoordinatorModule::begin(V1BLEClient* bleClient, PacketParser* parser) {
    ble_ = bleClient;
    parser_ = parser;
    reset();
}

void QuietCoordinatorModule::reset() {
    desired_ = QuietDesiredState{};
    committed_ = QuietCommittedState{};
    presentation_ = QuietPresentationState{};
    resetLockoutChannel();

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

    syncCommittedState();
}

void QuietCoordinatorModule::resetLockoutChannel() {
    lockoutMuteState_ = LockoutRuntimeMuteState{};
    overrideUnmuteActive_ = false;
    overrideUnmuteLastRetryMs_ = 0;
    overrideUnmuteRetryCount_ = 0;
    if (presentation_.activeMuteOwner == QuietOwner::LockoutMute ||
        presentation_.activeMuteOwner == QuietOwner::LockoutOverride) {
        presentation_.activeMuteOwner = QuietOwner::None;
    }
}

void QuietCoordinatorModule::syncCommittedState() {
    committed_.connected = ble_ ? ble_->isConnected() : false;
    committed_.hasDisplayState = false;
    committed_.muted = false;
    committed_.mainVolume = 0;
    committed_.muteVolume = 0;

    if (parser_) {
        const DisplayState& state = parser_->getDisplayState();
        committed_.hasDisplayState = true;
        committed_.muted = state.muted;
        committed_.mainVolume = state.mainVolume;
        committed_.muteVolume = state.muteVolume;
        presentation_.effectiveMuted = state.muted;
    } else {
        presentation_.effectiveMuted = false;
    }

    refreshPendingState();
}

void QuietCoordinatorModule::refreshPendingState() {
    if (desired_.mutePending && committed_.hasDisplayState && committed_.muted == desired_.mute) {
        desired_.mutePending = false;
    }
    if (desired_.volumePending && committed_.hasDisplayState && committed_.mainVolume == desired_.volume) {
        desired_.volumePending = false;
    }
}

QuietCommittedState QuietCoordinatorModule::getCommittedState() {
    syncCommittedState();
    return committed_;
}

bool QuietCoordinatorModule::sendMute(QuietOwner owner, bool muted) {
    syncCommittedState();

    desired_.muteOwner = owner;
    desired_.mute = muted;
    desired_.mutePending = committed_.hasDisplayState ? (committed_.muted != muted) : true;

    if (!ble_) {
        return false;
    }

    const bool sent = ble_->setMute(muted);
    if (sent) {
        presentation_.activeMuteOwner = muted ? owner : QuietOwner::None;
    }
    return sent;
}

bool QuietCoordinatorModule::sendVolume(QuietOwner owner, uint8_t volume, uint8_t muteVolume) {
    syncCommittedState();

    desired_.volumeOwner = owner;
    desired_.volume = volume;
    desired_.muteVolume = muteVolume;
    desired_.volumePending = committed_.hasDisplayState ? (committed_.mainVolume != volume) : true;

    if (!ble_) {
        return false;
    }

    const bool sent = ble_->setVolume(volume, muteVolume);
    if (sent) {
        presentation_.activeVolumeOwner = owner;
    }
    return sent;
}

bool QuietCoordinatorModule::processLockoutMute(const LockoutEnforcerResult& lockRes,
                                                const GpsLockoutCoreGuardStatus& lockoutGuard,
                                                bool bleConnected,
                                                bool v1Muted,
                                                bool overrideBandActive,
                                                uint32_t nowMs) {
    const LockoutRuntimeMuteDecision muteDecision =
        evaluateLockoutRuntimeMute(lockRes,
                                   lockoutGuard,
                                   bleConnected,
                                   v1Muted,
                                   overrideBandActive,
                                   lockoutMuteState_);

    if (muteDecision.sendMute) {
        sendMute(QuietOwner::LockoutMute, true);
        Serial.println("[Lockout] ENFORCE: mute sent to V1");
    }
    if (muteDecision.sendUnmute) {
        sendMute(QuietOwner::LockoutMute, false);
        Serial.println("[Lockout] ENFORCE: unmute sent to V1");
    }
    if (muteDecision.logGuardBlocked) {
        Serial.printf("[Lockout] ENFORCE blocked by core guard (%s)\n", lockoutGuard.reason);
    }

    const bool needsOverrideUnmute = bleConnected && overrideBandActive && v1Muted;
    if (needsOverrideUnmute) {
        if (overrideUnmuteRetryCount_ >= MAX_OVERRIDE_UNMUTE_RETRIES) {
            if (overrideUnmuteActive_) {
                Serial.printf("[Safety] Override unmute exhausted after %u retries\n",
                              overrideUnmuteRetryCount_);
                overrideUnmuteActive_ = false;
            }
        } else if (!overrideUnmuteActive_ ||
                   static_cast<uint32_t>(nowMs - overrideUnmuteLastRetryMs_) >=
                       OVERRIDE_UNMUTE_RETRY_MS) {
            sendMute(QuietOwner::LockoutOverride, false);
            overrideUnmuteLastRetryMs_ = nowMs;
            overrideUnmuteRetryCount_++;
            if (!overrideUnmuteActive_) {
                Serial.println("[Safety] Ka/Laser override active: unmute sent to V1");
            }
            overrideUnmuteActive_ = true;
        }
    } else {
        overrideUnmuteActive_ = false;
        overrideUnmuteLastRetryMs_ = 0;
        overrideUnmuteRetryCount_ = 0;
    }

    if (overrideBandActive || overrideUnmuteActive_) {
        presentation_.activeMuteOwner = QuietOwner::None;
    } else if (lockoutMuteState_.lockoutMuteActive) {
        presentation_.activeMuteOwner = QuietOwner::LockoutMute;
    } else if (presentation_.activeMuteOwner == QuietOwner::LockoutMute) {
        presentation_.activeMuteOwner = QuietOwner::None;
    }

    return muteDecision.sendMute || muteDecision.sendUnmute || overrideUnmuteActive_;
}

bool QuietCoordinatorModule::retryPendingPreQuietRestore(const uint32_t nowMs) {
    syncCommittedState();
    if (pendingPqRestoreVol_ == 0xFF || !ble_ || !parser_) {
        return false;
    }

    if ((nowMs - pendingPqRestoreSetMs_) >= PQ_RESTORE_TIMEOUT_MS) {
        Serial.println("[Lockout] PRE-QUIET: restore retry timeout");
        pendingPqRestoreVol_ = 0xFF;
        if (presentation_.activeVolumeOwner == QuietOwner::PreQuiet && !presentation_.preQuietActive) {
            presentation_.activeVolumeOwner = QuietOwner::None;
        }
        return false;
    }

    if (committed_.mainVolume == pendingPqRestoreVol_) {
        pendingPqRestoreVol_ = 0xFF;
        if (!presentation_.preQuietActive && presentation_.activeVolumeOwner == QuietOwner::PreQuiet) {
            presentation_.activeVolumeOwner = QuietOwner::None;
        }
        return false;
    }

    if ((nowMs - pendingPqRestoreLastRetryMs_) < PQ_RESTORE_RETRY_INTERVAL_MS) {
        return true;
    }

    pendingPqRestoreLastRetryMs_ = nowMs;
    sendVolume(QuietOwner::PreQuiet, pendingPqRestoreVol_, pendingPqRestoreMuteVol_);
#ifndef UNIT_TEST
    perfRecordPreQuietRestoreRetry();
#endif
    return true;
}

bool QuietCoordinatorModule::retryPendingSpeedVolRestore(const uint32_t nowMs) {
    syncCommittedState();
    if (pendingSpeedVolRestoreVol_ == 0xFF || !ble_ || !parser_) {
        return false;
    }

    if ((nowMs - pendingSpeedVolRestoreSetMs_) >= SPEED_VOL_RESTORE_TIMEOUT_MS) {
        Serial.println("[SpeedVol] restore retry timeout");
        pendingSpeedVolRestoreVol_ = 0xFF;
        if (presentation_.activeVolumeOwner == QuietOwner::SpeedVolume) {
            presentation_.activeVolumeOwner = presentation_.preQuietActive ? QuietOwner::PreQuiet
                                                                            : QuietOwner::None;
        }
        presentation_.speedVolZeroActive = false;
        return false;
    }

    if (committed_.mainVolume == pendingSpeedVolRestoreVol_) {
        pendingSpeedVolRestoreVol_ = 0xFF;
        if (!speedVolActive_ && presentation_.activeVolumeOwner == QuietOwner::SpeedVolume) {
            presentation_.activeVolumeOwner = presentation_.preQuietActive ? QuietOwner::PreQuiet
                                                                            : QuietOwner::None;
        }
        presentation_.speedVolZeroActive = false;
        return false;
    }

    if ((nowMs - pendingSpeedVolRestoreLastRetryMs_) < SPEED_VOL_RETRY_INTERVAL_MS) {
        return true;
    }

    pendingSpeedVolRestoreLastRetryMs_ = nowMs;
    sendVolume(QuietOwner::SpeedVolume,
               pendingSpeedVolRestoreVol_,
               pendingSpeedVolRestoreMuteVol_);
#ifndef UNIT_TEST
    perfRecordSpeedVolRetry();
#endif
    return true;
}

void QuietCoordinatorModule::setPreQuietActive(const bool active) {
    presentation_.preQuietActive = active;
    if (active) {
        presentation_.activeVolumeOwner = QuietOwner::PreQuiet;
    } else if (!speedVolActive_ && pendingSpeedVolRestoreVol_ == 0xFF &&
               pendingPqRestoreVol_ == 0xFF &&
               presentation_.activeVolumeOwner == QuietOwner::PreQuiet) {
        presentation_.activeVolumeOwner = QuietOwner::None;
    }
}

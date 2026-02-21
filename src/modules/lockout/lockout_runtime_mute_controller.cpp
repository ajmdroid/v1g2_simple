#include "lockout_runtime_mute_controller.h"

#include "../../settings.h"

LockoutRuntimeMuteDecision evaluateLockoutRuntimeMute(const LockoutEnforcerResult& lockRes,
                                                      const GpsLockoutCoreGuardStatus& lockoutGuard,
                                                      bool bleConnected,
                                                      bool v1Muted,
                                                      bool overrideBandActive,
                                                      LockoutRuntimeMuteState& state) {
    LockoutRuntimeMuteDecision decision;

    const bool enforceMode =
        lockRes.mode == static_cast<uint8_t>(LOCKOUT_RUNTIME_ENFORCE);
    const bool enforceAllowed = enforceMode && !lockoutGuard.tripped;
    const bool suppressionRequested = lockRes.evaluated && lockRes.shouldMute;
    const bool suppressionActive = suppressionRequested && enforceAllowed && !overrideBandActive;

    if (suppressionActive && !state.lockoutMuteActive && bleConnected) {
        decision.sendMute = true;
        state.lockoutMuteActive = true;
        state.muteWasActiveBeforeLockout = v1Muted;
        state.lockoutGuardBlockedLogged = false;
    }

    if (suppressionRequested &&
        enforceMode &&
        !enforceAllowed &&
        !state.lockoutGuardBlockedLogged) {
        decision.logGuardBlocked = true;
        state.lockoutGuardBlockedLogged = true;
    }

    // If lockout suppression was active and no longer can apply (left zone,
    // guard tripped, or override band active), release the lockout mute.
    if (state.lockoutMuteActive && !suppressionActive) {
        if (bleConnected && !state.muteWasActiveBeforeLockout) {
            decision.sendUnmute = true;
        }
        state.lockoutMuteActive = false;
        state.muteWasActiveBeforeLockout = false;
    }

    if (!suppressionRequested || !enforceMode) {
        state.lockoutGuardBlockedLogged = false;
    }

    return decision;
}

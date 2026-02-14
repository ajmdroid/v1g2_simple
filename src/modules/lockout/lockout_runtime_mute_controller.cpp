#include "lockout_runtime_mute_controller.h"

#include "../../settings.h"

LockoutRuntimeMuteDecision evaluateLockoutRuntimeMute(const LockoutEnforcerResult& lockRes,
                                                      const GpsLockoutCoreGuardStatus& lockoutGuard,
                                                      bool bleConnected,
                                                      LockoutRuntimeMuteState& state) {
    LockoutRuntimeMuteDecision decision;

    const bool enforceMode =
        lockRes.mode == static_cast<uint8_t>(LOCKOUT_RUNTIME_ENFORCE);
    const bool enforceAllowed = enforceMode && !lockoutGuard.tripped;

    if (lockRes.evaluated &&
        lockRes.shouldMute &&
        enforceAllowed &&
        !state.lockoutMuteActive &&
        bleConnected) {
        decision.sendMute = true;
        state.lockoutMuteActive = true;
        state.lockoutGuardBlockedLogged = false;
    }

    if (lockRes.evaluated &&
        lockRes.shouldMute &&
        enforceMode &&
        !enforceAllowed &&
        !state.lockoutGuardBlockedLogged) {
        decision.logGuardBlocked = true;
        state.lockoutGuardBlockedLogged = true;
    }

    if (!lockRes.shouldMute) {
        state.lockoutMuteActive = false;
        state.lockoutGuardBlockedLogged = false;
    }

    return decision;
}

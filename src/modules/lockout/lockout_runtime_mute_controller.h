#pragma once

#include "lockout_enforcer.h"
#include "../gps/gps_lockout_safety.h"

struct LockoutRuntimeMuteState {
    bool lockoutMuteActive = false;
    bool lockoutGuardBlockedLogged = false;
    bool muteWasActiveBeforeLockout = false;
};

struct LockoutRuntimeMuteDecision {
    bool sendMute = false;
    bool sendUnmute = false;
    bool logGuardBlocked = false;
};

LockoutRuntimeMuteDecision evaluateLockoutRuntimeMute(const LockoutEnforcerResult& lockRes,
                                                      const GpsLockoutCoreGuardStatus& lockoutGuard,
                                                      bool bleConnected,
                                                      bool v1Muted,
                                                      bool overrideBandActive,
                                                      LockoutRuntimeMuteState& state);

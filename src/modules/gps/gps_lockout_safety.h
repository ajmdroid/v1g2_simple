#pragma once

#include <Arduino.h>

enum LockoutRuntimeMode : uint8_t;

struct GpsLockoutCoreGuardStatus {
    bool enabled = false;
    bool tripped = false;
    const char* reason = "none";
};

// Parses lockout runtime mode from either integer strings ("0".."3")
// or symbolic names ("off", "shadow", "advisory", "enforce").
// Returns fallback for empty/invalid values.
LockoutRuntimeMode gpsLockoutParseRuntimeModeArg(const String& raw,
                                                 LockoutRuntimeMode fallback);

// Evaluates lockout core guardrails against current system drop counters.
// This helper is intentionally side-effect free so call sites can keep
// policy decisions out of hot paths and web handlers.
GpsLockoutCoreGuardStatus gpsLockoutEvaluateCoreGuard(bool guardEnabled,
                                                      uint16_t maxQueueDrops,
                                                      uint16_t maxPerfDrops,
                                                      uint16_t maxEventBusDrops,
                                                      uint32_t queueDrops,
                                                      uint32_t perfDrops,
                                                      uint32_t eventBusDrops);

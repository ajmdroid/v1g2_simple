#pragma once

#include <ArduinoJson.h>

#include "modules/wifi/wifi_auto_start_module.h"

namespace StatusObservabilityPayload {

struct LockoutStatusSnapshot {
    const char* mode = "off";
    uint8_t modeRaw = 0;
    bool coreGuardEnabled = false;
    bool coreGuardTripped = false;
    const char* coreGuardReason = "none";
    uint16_t maxQueueDrops = 0;
    uint16_t maxPerfDrops = 0;
    uint16_t maxEventBusDrops = 0;
    uint32_t queueDrops = 0;
    uint32_t perfDrops = 0;
    uint32_t eventBusDrops = 0;
    bool enforceRequested = false;
    bool enforceAllowed = false;
};

struct WifiStatusSnapshot {
    uint32_t apLastTransitionReasonCode = 0;
    const char* apLastTransitionReason = "unknown";
    uint32_t lowDmaCooldownRemainingMs = 0;
    WifiAutoStartDecisionSnapshot autoStart;
};

void appendStatusObservability(JsonObject root,
                               const LockoutStatusSnapshot& lockout,
                               const WifiStatusSnapshot& wifi);

}  // namespace StatusObservabilityPayload

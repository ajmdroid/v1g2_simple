#pragma once

#include <stdint.h>

namespace ObdStatePolicy {

struct DisconnectedDecision {
    bool transitionToIdle = false;
    bool transitionToConnecting = false;
    bool clearTargetDevice = false;
    bool resetConnectionFailures = false;
    uint8_t nextReconnectCycleCount = 0;
    uint32_t waitThresholdMs = 0;
};

inline uint32_t computeRetryDelayMs(uint8_t connectionFailures,
                                    uint32_t baseRetryDelayMs,
                                    uint32_t maxRetryDelayMs) {
    uint32_t retryDelay = baseRetryDelayMs * (1u << connectionFailures);
    if (retryDelay > maxRetryDelayMs) {
        retryDelay = maxRetryDelayMs;
    }
    return retryDelay;
}

inline uint32_t computeReconnectCooldownMs(uint8_t reconnectCycleCount,
                                           uint32_t maxReconnectCooldownMs) {
    uint32_t cooldown = 60000u * (1u << (reconnectCycleCount < 3 ? reconnectCycleCount : 3));
    if (cooldown > maxReconnectCooldownMs) {
        cooldown = maxReconnectCooldownMs;
    }
    return cooldown;
}

inline uint8_t bumpReconnectCycle(uint8_t reconnectCycleCount) {
    return (reconnectCycleCount < 10) ? static_cast<uint8_t>(reconnectCycleCount + 1u) : 10u;
}

inline DisconnectedDecision evaluateDisconnected(bool hasTargetDevice,
                                                 uint8_t connectionFailures,
                                                 uint8_t maxConnectionFailures,
                                                 uint8_t reconnectCycleCount,
                                                 uint32_t elapsedSinceDisconnectMs,
                                                 uint32_t baseRetryDelayMs,
                                                 uint32_t maxRetryDelayMs,
                                                 uint32_t maxReconnectCooldownMs) {
    DisconnectedDecision decision;
    decision.nextReconnectCycleCount = reconnectCycleCount;

    if (!hasTargetDevice) {
        return decision;
    }

    if (connectionFailures >= maxConnectionFailures) {
        decision.waitThresholdMs =
            computeReconnectCooldownMs(reconnectCycleCount, maxReconnectCooldownMs);
        if (elapsedSinceDisconnectMs >= decision.waitThresholdMs) {
            decision.transitionToIdle = true;
            decision.clearTargetDevice = true;
            decision.resetConnectionFailures = true;
            decision.nextReconnectCycleCount = bumpReconnectCycle(reconnectCycleCount);
        }
        return decision;
    }

    decision.waitThresholdMs =
        computeRetryDelayMs(connectionFailures, baseRetryDelayMs, maxRetryDelayMs);
    if (elapsedSinceDisconnectMs >= decision.waitThresholdMs) {
        decision.transitionToConnecting = true;
    }

    return decision;
}

}  // namespace ObdStatePolicy

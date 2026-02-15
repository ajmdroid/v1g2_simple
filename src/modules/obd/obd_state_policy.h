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

// ─── OBD connect-gating policy (pure functions, no BLE/RTOS deps) ───
// These mirror the runtime guards in obd_handler.cpp so the decisions
// can be verified in native tests without hardware.

namespace ObdConnectPolicy {

// OBD state enum values (mirrors OBDState for policy use).
enum class State : uint8_t {
    IDLE, SCANNING, CONNECTING, INITIALIZING, READY, POLLING, DISCONNECTED, FAILED
};

// Should the IDLE state attempt auto-connect?
// Returns true only when V1 is connected and retry interval has elapsed.
inline bool shouldIdleAutoConnect(bool linkReady,
                                  uint32_t elapsedSinceLastAttemptMs,
                                  uint32_t autoConnectRetryMs,
                                  bool hasAutoConnectTarget,
                                  bool autoConnectSuppressed) {
    if (!linkReady) return false;
    if (autoConnectSuppressed) return false;
    if (elapsedSinceLastAttemptMs < autoConnectRetryMs) return false;
    return hasAutoConnectTarget;
}

// Should handleConnecting() proceed or defer?
// Returns true when it is safe to start a BLE connect attempt.
inline bool shouldProceedWithConnect(bool linkReady, bool hasTargetDevice) {
    if (!hasTargetDevice) return false;
    return linkReady;
}

// Should tryAutoConnect() proceed?
// Returns false for states that are already busy, or when V1 is not connected.
inline bool shouldTryAutoConnect(State currentState, bool linkReady) {
    switch (currentState) {
        case State::CONNECTING:
        case State::INITIALIZING:
        case State::READY:
        case State::POLLING:
        case State::SCANNING:
            return false;
        default:
            break;
    }
    return linkReady;
}

// Should the main loop activate WiFi-priority BLE suppression for OBD?
// Only relevant when WiFi AP is actually on.
inline bool shouldActivateWifiPriorityForObd(bool wifiApOn,
                                             bool obdEnabled,
                                             bool obdScanActive,
                                             State obdState) {
    if (!wifiApOn) return false;
    if (!obdEnabled) return false;
    return obdScanActive ||
           obdState == State::CONNECTING ||
           obdState == State::INITIALIZING;
}

}  // namespace ObdConnectPolicy

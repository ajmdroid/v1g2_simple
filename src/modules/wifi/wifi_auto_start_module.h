#pragma once

#include <Arduino.h>

#include <cstdint>
#include <functional>

enum class WifiAutoStartGate : uint8_t {
    Unknown = 0,
    AlreadyDone = 1,
    WifiDisabled = 2,
    WifiAtBootDisabled = 3,
    WaitingBleSettle = 4,
    WaitingBootTimeout = 5,
    WaitingDma = 6,
    Starting = 7,
    StartFailed = 8,
};

const char* wifiAutoStartGateName(WifiAutoStartGate gate);

struct WifiAutoStartDecisionSnapshot {
    uint32_t nowMs = 0;
    uint32_t v1ConnectedAtMs = 0;
    uint32_t msSinceV1Connect = 0;
    uint32_t settleMs = 0;
    uint32_t bootTimeoutMs = 0;
    bool enableWifi = false;
    bool enableWifiAtBoot = false;
    bool bleConnected = false;
    bool canStartDma = false;
    bool wifiAutoStartDone = false;
    bool bleSettled = false;
    bool bootTimeoutReached = false;
    bool shouldAutoStart = false;
    bool startTriggered = false;
    bool startSucceeded = false;
    WifiAutoStartGate gate = WifiAutoStartGate::Unknown;
};

// Owns deferred WiFi auto-start gate decisions and one-shot triggering.
class WifiAutoStartModule {
public:
    bool process(unsigned long nowMs,
                 unsigned long v1ConnectedAtMs,
                 bool enableWifi,
                 bool enableWifiAtBoot,
                 bool bleConnected,
                 bool canStartDma,
                 bool& wifiAutoStartDone,
                 const std::function<bool(bool autoStarted)>& startWifi);

    const WifiAutoStartDecisionSnapshot& getLastDecision() const { return lastDecision_; }

private:
    WifiAutoStartDecisionSnapshot buildDecisionSnapshot(unsigned long nowMs,
                                                        unsigned long v1ConnectedAtMs,
                                                        bool enableWifi,
                                                        bool enableWifiAtBoot,
                                                        bool bleConnected,
                                                        bool canStartDma,
                                                        bool wifiAutoStartDone,
                                                        bool startTriggered,
                                                        bool startSucceeded) const;
    void logDecisionIfChanged(const WifiAutoStartDecisionSnapshot& snapshot);

    WifiAutoStartDecisionSnapshot lastDecision_{};
    WifiAutoStartGate lastLoggedGate_ = WifiAutoStartGate::Unknown;
    bool lastLoggedShouldAutoStart_ = false;
    bool lastLoggedStartTriggered_ = false;
    bool lastLoggedAutoStartDone_ = false;
    bool hasLoggedDecision_ = false;
};

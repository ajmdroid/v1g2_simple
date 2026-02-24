#pragma once

#include <Arduino.h>

class OBDHandler;
class V1BLEClient;
class WiFiManager;

// Returns true when WiFi processing should run this loop iteration.
bool isWifiProcessingEnabledPolicy(const WiFiManager& wifiManager,
                                   bool enableWifiAtBoot,
                                   bool wifiAutoStartDone);

// Manages BLE WiFi-priority transitions with hysteresis/hold semantics.
class WifiPriorityPolicyModule {
public:
    void reset();

    void apply(unsigned long nowMs,
               bool obdServiceEnabled,
               V1BLEClient& bleClient,
               WiFiManager& wifiManager,
               OBDHandler& obdHandler);

private:
    unsigned long wifiPriorityLastTransitionMs = 0;
    unsigned long obdBleCriticalHoldUntilMs = 0;
};

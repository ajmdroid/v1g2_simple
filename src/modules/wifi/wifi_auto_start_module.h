#pragma once

#include <Arduino.h>

#include <functional>

// Owns deferred WiFi auto-start gate decisions and one-shot triggering.
class WifiAutoStartModule {
public:
    bool process(unsigned long nowMs,
                 unsigned long v1ConnectedAtMs,
                 bool enableWifiAtBoot,
                 bool bleConnected,
                 bool canStartDma,
                 bool& wifiAutoStartDone,
                 const std::function<void()>& startWifi,
                 const std::function<void()>& markAutoStarted);
};

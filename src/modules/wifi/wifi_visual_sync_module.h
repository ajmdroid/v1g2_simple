#pragma once

#include <Arduino.h>

#include <functional>

// Owns WiFi icon refresh cadence/state so main loop keeps no static UI state.
class WifiVisualSyncModule {
public:
    void reset();

    void process(unsigned long nowMs,
                 bool wifiVisualActiveNow,
                 bool displayPreviewRunning,
                 bool bootSplashHoldActive,
                 const std::function<void()>& drawAndFlush);

private:
    bool lastWifiVisualActive_ = false;
    unsigned long lastWifiIconRefreshMs_ = 0;
};

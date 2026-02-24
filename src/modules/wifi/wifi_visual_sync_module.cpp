#include "wifi_visual_sync_module.h"

void WifiVisualSyncModule::reset() {
    lastWifiVisualActive_ = false;
    lastWifiIconRefreshMs_ = 0;
}

void WifiVisualSyncModule::process(unsigned long nowMs,
                                   bool wifiVisualActiveNow,
                                   bool displayPreviewRunning,
                                   bool bootSplashHoldActive,
                                   const std::function<void()>& drawAndFlush) {
    bool refreshWifiIcon = false;
    if (wifiVisualActiveNow != lastWifiVisualActive_) {
        refreshWifiIcon = true;
        lastWifiVisualActive_ = wifiVisualActiveNow;
    } else if (wifiVisualActiveNow && (nowMs - lastWifiIconRefreshMs_) >= 2000UL) {
        // Periodic refresh keeps WiFi/AP client-connect color current.
        refreshWifiIcon = true;
    }

    if (refreshWifiIcon && !displayPreviewRunning && !bootSplashHoldActive) {
        if (drawAndFlush) {
            drawAndFlush();
        }
        lastWifiIconRefreshMs_ = nowMs;
    }
}

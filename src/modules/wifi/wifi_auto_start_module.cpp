#include "wifi_auto_start_module.h"

#include "wifi_boot_policy.h"
#include "modules/perf/debug_macros.h"

bool WifiAutoStartModule::process(unsigned long nowMs,
                                  unsigned long v1ConnectedAtMs,
                                  bool enableWifiAtBoot,
                                  bool bleConnected,
                                  bool canStartDma,
                                  bool& wifiAutoStartDone,
                                  const std::function<void()>& startWifi,
                                  const std::function<void()>& markAutoStarted) {
    if (wifiAutoStartDone || !enableWifiAtBoot) {
        return false;
    }

    constexpr uint32_t WIFI_SETTLE_MS = 3000;
    constexpr uint32_t WIFI_BOOT_TIMEOUT_MS = 30000;

    // `nowMs` is captured near loop start; V1 connect callback can stamp a slightly
    // newer `v1ConnectedAtMs` later in the same loop. Saturate to avoid underflow.
    const uint32_t msSinceV1 =
        (v1ConnectedAtMs > 0 && nowMs >= v1ConnectedAtMs)
            ? static_cast<uint32_t>(nowMs - v1ConnectedAtMs)
            : 0;

    if (!WifiBootPolicy::shouldAutoStartWifi(
            true, false, bleConnected,
            msSinceV1, WIFI_SETTLE_MS,
            nowMs, WIFI_BOOT_TIMEOUT_MS, canStartDma)) {
        return false;
    }

    SerialLog.printf("[WiFi] Deferred auto-start at %lu ms (v1Connect=%lu ms ago)\n",
                     nowMs, msSinceV1);
    if (startWifi) {
        startWifi();
    }
    if (markAutoStarted) {
        markAutoStarted();
    }
    wifiAutoStartDone = true;
    return true;
}

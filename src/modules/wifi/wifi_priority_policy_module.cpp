#include "wifi_priority_policy_module.h"

#include "../../ble_client.h"
#include "../../wifi_manager.h"

bool isWifiProcessingEnabledPolicy(const WiFiManager& wifiManager,
                                   bool enableWifiAtBoot,
                                   bool wifiAutoStartDone) {
    return wifiManager.isWifiServiceActive() ||
           wifiManager.isConnected() ||
           (enableWifiAtBoot && !wifiAutoStartDone);
}

void WifiPriorityPolicyModule::reset() {
    wifiPriorityLastTransitionMs = 0;
}

void WifiPriorityPolicyModule::apply(unsigned long nowMs,
                                     V1BLEClient& bleClient,
                                     WiFiManager& wifiManager) {
    constexpr unsigned long WIFI_PRIORITY_ENABLE_TIMEOUT_MS = 3500;
    constexpr unsigned long WIFI_PRIORITY_DISABLE_TIMEOUT_MS = 20000;
    constexpr unsigned long WIFI_PRIORITY_MIN_HOLD_MS = 12000;

    const bool wifiPriorityAllowed = bleClient.isConnected();
    const bool wifiPriorityCurrent = bleClient.isWifiPriority();
    const unsigned long uiTimeoutMs = wifiPriorityCurrent ? WIFI_PRIORITY_DISABLE_TIMEOUT_MS
                                                          : WIFI_PRIORITY_ENABLE_TIMEOUT_MS;
    const bool uiActive = wifiManager.isUiActive(uiTimeoutMs);
    const bool wifiPriority = wifiPriorityAllowed && uiActive;
    const bool holdActive = (nowMs - wifiPriorityLastTransitionMs) < WIFI_PRIORITY_MIN_HOLD_MS;
    if (wifiPriority != wifiPriorityCurrent && !holdActive) {
        bleClient.setWifiPriority(wifiPriority);
        wifiPriorityLastTransitionMs = nowMs;
    }
}

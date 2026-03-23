#include "wifi_priority_policy_module.h"

#include "../../ble_client.h"
#include "../../wifi_manager.h"

bool isWifiProcessingEnabledPolicy(const WiFiManager& wifiManager,
                                   bool enableWifi,
                                   bool enableWifiAtBoot,
                                   bool wifiAutoStartDone) {
    return wifiManager.isWifiServiceActive() ||
           wifiManager.hasPendingLifecycleWork() ||
           wifiManager.isConnected() ||
           (enableWifi && enableWifiAtBoot && !wifiAutoStartDone);
}

void WifiPriorityPolicyModule::reset() {
    wifiPriorityLastTransitionMs = 0;
    pendingPriorityValid = false;
    pendingPriorityState = false;
    pendingPrioritySinceMs = 0;
}

void WifiPriorityPolicyModule::apply(unsigned long nowMs,
                                     V1BLEClient& bleClient,
                                     WiFiManager& wifiManager) {
    constexpr unsigned long WIFI_PRIORITY_ENABLE_TIMEOUT_MS = 3500;
    constexpr unsigned long WIFI_PRIORITY_DISABLE_TIMEOUT_MS = 20000;
    constexpr unsigned long WIFI_PRIORITY_MIN_HOLD_MS = 12000;
    constexpr unsigned long WIFI_PRIORITY_TOGGLE_DEBOUNCE_MS = 1500;

    const bool wifiPriorityAllowed = bleClient.isConnected();
    const bool wifiPriorityCurrent = bleClient.isWifiPriority();
    const unsigned long uiTimeoutMs = wifiPriorityCurrent ? WIFI_PRIORITY_DISABLE_TIMEOUT_MS
                                                          : WIFI_PRIORITY_ENABLE_TIMEOUT_MS;
    const bool uiActive = wifiManager.isUiActive(uiTimeoutMs);
    const bool wifiPriorityDesired = wifiPriorityAllowed && uiActive;
    const bool holdActive = (nowMs - wifiPriorityLastTransitionMs) < WIFI_PRIORITY_MIN_HOLD_MS;

    if (wifiPriorityDesired == wifiPriorityCurrent) {
        pendingPriorityValid = false;
        return;
    }
    if (holdActive) {
        pendingPriorityValid = false;
        return;
    }

    if (!pendingPriorityValid || pendingPriorityState != wifiPriorityDesired) {
        pendingPriorityValid = true;
        pendingPriorityState = wifiPriorityDesired;
        pendingPrioritySinceMs = nowMs;
        return;
    }

    if ((nowMs - pendingPrioritySinceMs) < WIFI_PRIORITY_TOGGLE_DEBOUNCE_MS) {
        return;
    }

    bleClient.setWifiPriority(wifiPriorityDesired);
    wifiPriorityLastTransitionMs = nowMs;
    pendingPriorityValid = false;
}

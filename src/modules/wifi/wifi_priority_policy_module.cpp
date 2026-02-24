#include "wifi_priority_policy_module.h"

#include "../../ble_client.h"
#include "../../obd_handler.h"
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
    obdBleCriticalHoldUntilMs = 0;
}

void WifiPriorityPolicyModule::apply(unsigned long nowMs,
                                     bool obdServiceEnabled,
                                     V1BLEClient& bleClient,
                                     WiFiManager& wifiManager,
                                     OBDHandler& obdHandler) {
    // WiFi priority mode: web UI can deprioritize BLE background work, but never
    // at the expense of establishing/maintaining V1 connectivity.
    // Hysteresis: different timeouts for enable vs disable prevent oscillation.
    // Min-hold guard: once a transition fires, suppress further transitions for
    // a minimum hold period to avoid sub-second flapping caused by HTTP request
    // arrival racing with this check within the same loop iteration.
    constexpr unsigned long WIFI_PRIORITY_ENABLE_TIMEOUT_MS = 3500;
    constexpr unsigned long WIFI_PRIORITY_DISABLE_TIMEOUT_MS = 20000;
    constexpr unsigned long WIFI_PRIORITY_MIN_HOLD_MS = 12000;
    constexpr unsigned long OBD_BLE_CRITICAL_HOLD_MS = 8000;

    const bool wifiPriorityAllowed = bleClient.isConnected();
    const bool wifiPriorityCurrent = bleClient.isWifiPriority();
    const unsigned long uiTimeoutMs = wifiPriorityCurrent ? WIFI_PRIORITY_DISABLE_TIMEOUT_MS
                                                          : WIFI_PRIORITY_ENABLE_TIMEOUT_MS;
    const bool uiActive = wifiManager.isUiActive(uiTimeoutMs);
    // OBD BLE-critical suppression only matters when WiFi AP is actually on.
    // When WiFi is off, the OBD handler already stops the V1 scan before
    // connecting (connectToDevice) — piggybacking on WiFi priority mode is
    // redundant and causes harmful flapping (stop/restart proxy advertising
    // on every OBD retry cycle, confusing log spam about "WiFi priority"
    // when WiFi is completely off).
    const bool wifiApOn = wifiManager.isSetupModeActive();
    const OBDState obdState = obdHandler.getState();
    const bool obdBleCriticalNow =
        wifiApOn &&
        obdServiceEnabled &&
        (obdHandler.isScanActive() ||
         obdState == OBDState::CONNECTING ||
         obdState == OBDState::INITIALIZING);
    if (obdBleCriticalNow) {
        obdBleCriticalHoldUntilMs = nowMs + OBD_BLE_CRITICAL_HOLD_MS;
    }
    const bool obdBleCriticalHeld =
        wifiApOn &&
        obdServiceEnabled &&
        (obdBleCriticalHoldUntilMs != 0) &&
        (static_cast<int32_t>(obdBleCriticalHoldUntilMs - nowMs) > 0);
    const bool obdBleCritical = obdBleCriticalNow || obdBleCriticalHeld;
    // Keep BLE background suppression active through OBD scan/connect/init so
    // proxy advertising or scan resumes do not interrupt OBD pairing flow
    // (only relevant when WiFi AP is on and could cause radio contention).
    const bool wifiPriority = wifiPriorityAllowed && (uiActive || obdBleCritical);
    const bool holdActive = (nowMs - wifiPriorityLastTransitionMs) < WIFI_PRIORITY_MIN_HOLD_MS;
    if (wifiPriority != wifiPriorityCurrent && !holdActive) {
        bleClient.setWifiPriority(wifiPriority);
        wifiPriorityLastTransitionMs = nowMs;
    }
}

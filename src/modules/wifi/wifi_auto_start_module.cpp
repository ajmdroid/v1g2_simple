#include "wifi_auto_start_module.h"

#include "wifi_boot_policy.h"
#include "modules/perf/debug_macros.h"

namespace {

constexpr uint32_t WIFI_SETTLE_MS = 3000;
constexpr uint32_t WIFI_BOOT_TIMEOUT_MS = 30000;
constexpr bool WIFI_AUTOSTART_DECISION_LOGS = false;

}  // namespace

const char* wifiAutoStartGateName(WifiAutoStartGate gate) {
    switch (gate) {
        case WifiAutoStartGate::AlreadyDone:
            return "already_done";
        case WifiAutoStartGate::WifiDisabled:
            return "wifi_disabled";
        case WifiAutoStartGate::WifiAtBootDisabled:
            return "wifi_at_boot_disabled";
        case WifiAutoStartGate::WaitingBleSettle:
            return "waiting_ble_settle";
        case WifiAutoStartGate::WaitingBootTimeout:
            return "waiting_boot_timeout";
        case WifiAutoStartGate::WaitingDma:
            return "waiting_dma";
        case WifiAutoStartGate::Starting:
            return "starting";
        case WifiAutoStartGate::StartFailed:
            return "start_failed";
        case WifiAutoStartGate::Unknown:
        default:
            return "unknown";
    }
}

WifiAutoStartDecisionSnapshot WifiAutoStartModule::buildDecisionSnapshot(unsigned long nowMs,
                                                                        unsigned long v1ConnectedAtMs,
                                                                        bool enableWifi,
                                                                        bool enableWifiAtBoot,
                                                                        bool bleConnected,
                                                                        bool canStartDma,
                                                                        bool wifiAutoStartDone,
                                                                        bool startTriggered,
                                                                        bool startSucceeded) const {
    WifiAutoStartDecisionSnapshot snapshot;
    snapshot.nowMs = nowMs;
    snapshot.v1ConnectedAtMs = v1ConnectedAtMs;
    snapshot.settleMs = WIFI_SETTLE_MS;
    snapshot.bootTimeoutMs = WIFI_BOOT_TIMEOUT_MS;
    snapshot.enableWifi = enableWifi;
    snapshot.enableWifiAtBoot = enableWifiAtBoot;
    snapshot.bleConnected = bleConnected;
    snapshot.canStartDma = canStartDma;
    snapshot.wifiAutoStartDone = wifiAutoStartDone;
    snapshot.startTriggered = startTriggered;
    snapshot.startSucceeded = startSucceeded;

    snapshot.msSinceV1Connect =
        (v1ConnectedAtMs > 0 && static_cast<int32_t>(nowMs - v1ConnectedAtMs) >= 0)
            ? static_cast<uint32_t>(nowMs - v1ConnectedAtMs)
            : 0;
    snapshot.bleSettled = bleConnected && (snapshot.msSinceV1Connect >= WIFI_SETTLE_MS);
    snapshot.bootTimeoutReached = nowMs >= WIFI_BOOT_TIMEOUT_MS;

    if (startTriggered && !startSucceeded) {
        snapshot.gate = WifiAutoStartGate::StartFailed;
    } else if (startTriggered) {
        snapshot.gate = WifiAutoStartGate::Starting;
    } else if (wifiAutoStartDone) {
        snapshot.gate = WifiAutoStartGate::AlreadyDone;
    } else if (!enableWifi) {
        snapshot.gate = WifiAutoStartGate::WifiDisabled;
    } else if (!enableWifiAtBoot) {
        snapshot.gate = WifiAutoStartGate::WifiAtBootDisabled;
    } else if (!snapshot.bleSettled && !snapshot.bootTimeoutReached) {
        snapshot.gate = bleConnected ? WifiAutoStartGate::WaitingBleSettle
                                     : WifiAutoStartGate::WaitingBootTimeout;
    } else if (!canStartDma) {
        snapshot.gate = WifiAutoStartGate::WaitingDma;
    } else {
        snapshot.gate = WifiAutoStartGate::Starting;
    }

    snapshot.shouldAutoStart =
        (snapshot.gate == WifiAutoStartGate::Starting || snapshot.gate == WifiAutoStartGate::StartFailed);
    return snapshot;
}

void WifiAutoStartModule::logDecisionIfChanged(const WifiAutoStartDecisionSnapshot& snapshot) {
    if (!WIFI_AUTOSTART_DECISION_LOGS) {
        return;
    }

    if (hasLoggedDecision_ &&
        snapshot.gate == lastLoggedGate_ &&
        snapshot.shouldAutoStart == lastLoggedShouldAutoStart_ &&
        snapshot.startTriggered == lastLoggedStartTriggered_ &&
        snapshot.wifiAutoStartDone == lastLoggedAutoStartDone_) {
        return;
    }

    SerialLog.printf(
        "[WiFiAutoStart] gate=%s enableWifi=%s enableWifiAtBoot=%s bleConnected=%s "
        "msSinceV1Connect=%lu canStartDma=%s wifiAutoStartDone=%s bleSettled=%s "
        "bootTimeoutReached=%s shouldAutoStart=%s startTriggered=%s\n",
        wifiAutoStartGateName(snapshot.gate),
        snapshot.enableWifi ? "on" : "off",
        snapshot.enableWifiAtBoot ? "on" : "off",
        snapshot.bleConnected ? "yes" : "no",
        static_cast<unsigned long>(snapshot.msSinceV1Connect),
        snapshot.canStartDma ? "yes" : "no",
        snapshot.wifiAutoStartDone ? "yes" : "no",
        snapshot.bleSettled ? "yes" : "no",
        snapshot.bootTimeoutReached ? "yes" : "no",
        snapshot.shouldAutoStart ? "yes" : "no",
        snapshot.startTriggered ? "yes" : "no");

    hasLoggedDecision_ = true;
    lastLoggedGate_ = snapshot.gate;
    lastLoggedShouldAutoStart_ = snapshot.shouldAutoStart;
    lastLoggedStartTriggered_ = snapshot.startTriggered;
    lastLoggedAutoStartDone_ = snapshot.wifiAutoStartDone;
}

bool WifiAutoStartModule::process(unsigned long nowMs,
                                  unsigned long v1ConnectedAtMs,
                                  bool enableWifi,
                                  bool enableWifiAtBoot,
                                  bool bleConnected,
                                  bool canStartDma,
                                  bool& wifiAutoStartDone,
                                  bool (*startWifi)(bool autoStarted, void* ctx),
                                  void* ctx) {
    lastDecision_ = buildDecisionSnapshot(nowMs,
                                          v1ConnectedAtMs,
                                          enableWifi,
                                          enableWifiAtBoot,
                                          bleConnected,
                                          canStartDma,
                                          wifiAutoStartDone,
                                          false,
                                          false);
    logDecisionIfChanged(lastDecision_);

    if (wifiAutoStartDone || !enableWifi || !enableWifiAtBoot) {
        return false;
    }

    if (!WifiBootPolicy::shouldAutoStartWifi(
            true, false, bleConnected,
            lastDecision_.msSinceV1Connect, WIFI_SETTLE_MS,
            nowMs, WIFI_BOOT_TIMEOUT_MS, canStartDma)) {
        return false;
    }

    SerialLog.printf("[WiFi] Deferred auto-start at %lu ms (v1Connect=%lu ms ago)\n",
                     nowMs, static_cast<unsigned long>(lastDecision_.msSinceV1Connect));
    bool startSucceeded = true;
    if (startWifi) {
        startSucceeded = startWifi(true, ctx);
    }
    if (!startSucceeded) {
        lastDecision_ = buildDecisionSnapshot(nowMs,
                                              v1ConnectedAtMs,
                                              enableWifi,
                                              enableWifiAtBoot,
                                              bleConnected,
                                              canStartDma,
                                              wifiAutoStartDone,
                                              true,
                                              false);
        logDecisionIfChanged(lastDecision_);
        return false;
    }
    wifiAutoStartDone = true;
    lastDecision_ = buildDecisionSnapshot(nowMs,
                                          v1ConnectedAtMs,
                                          enableWifi,
                                          enableWifiAtBoot,
                                          bleConnected,
                                          canStartDma,
                                          wifiAutoStartDone,
                                          true,
                                          true);
    logDecisionIfChanged(lastDecision_);
    return true;
}

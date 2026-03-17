#include "status_observability_payload.h"

namespace StatusObservabilityPayload {

namespace {

JsonObject ensureObject(JsonObject root, const char* key) {
    JsonObject obj = root[key].as<JsonObject>();
    if (obj.isNull()) {
        obj = root[key].to<JsonObject>();
    }
    return obj;
}

}  // namespace

void appendStatusObservability(JsonObject root,
                               const LockoutStatusSnapshot& lockoutSnapshot,
                               const WifiStatusSnapshot& wifiSnapshot) {
    JsonObject lockout = ensureObject(root, "lockout");
    lockout["mode"] = lockoutSnapshot.mode;
    lockout["modeRaw"] = lockoutSnapshot.modeRaw;
    lockout["coreGuardEnabled"] = lockoutSnapshot.coreGuardEnabled;
    lockout["coreGuardTripped"] = lockoutSnapshot.coreGuardTripped;
    lockout["coreGuardReason"] = lockoutSnapshot.coreGuardReason;
    lockout["maxQueueDrops"] = lockoutSnapshot.maxQueueDrops;
    lockout["maxPerfDrops"] = lockoutSnapshot.maxPerfDrops;
    lockout["maxEventBusDrops"] = lockoutSnapshot.maxEventBusDrops;
    lockout["queueDrops"] = lockoutSnapshot.queueDrops;
    lockout["perfDrops"] = lockoutSnapshot.perfDrops;
    lockout["eventBusDrops"] = lockoutSnapshot.eventBusDrops;
    lockout["enforceRequested"] = lockoutSnapshot.enforceRequested;
    lockout["enforceAllowed"] = lockoutSnapshot.enforceAllowed;

    JsonObject wifi = ensureObject(root, "wifi");
    wifi["ap_last_transition_reason_code"] = wifiSnapshot.apLastTransitionReasonCode;
    wifi["ap_last_transition_reason"] = wifiSnapshot.apLastTransitionReason;
    wifi["low_dma_cooldown_ms"] = wifiSnapshot.lowDmaCooldownRemainingMs;

    JsonObject autoStart = ensureObject(wifi, "auto_start");
    autoStart["gate"] = wifiAutoStartGateName(wifiSnapshot.autoStart.gate);
    autoStart["gateCode"] = static_cast<uint8_t>(wifiSnapshot.autoStart.gate);
    autoStart["enableWifi"] = wifiSnapshot.autoStart.enableWifi;
    autoStart["enableWifiAtBoot"] = wifiSnapshot.autoStart.enableWifiAtBoot;
    autoStart["bleConnected"] = wifiSnapshot.autoStart.bleConnected;
    autoStart["v1ConnectedAtMs"] = wifiSnapshot.autoStart.v1ConnectedAtMs;
    autoStart["msSinceV1Connect"] = wifiSnapshot.autoStart.msSinceV1Connect;
    autoStart["settleMs"] = wifiSnapshot.autoStart.settleMs;
    autoStart["bootTimeoutMs"] = wifiSnapshot.autoStart.bootTimeoutMs;
    autoStart["canStartDma"] = wifiSnapshot.autoStart.canStartDma;
    autoStart["wifiAutoStartDone"] = wifiSnapshot.autoStart.wifiAutoStartDone;
    autoStart["bleSettled"] = wifiSnapshot.autoStart.bleSettled;
    autoStart["bootTimeoutReached"] = wifiSnapshot.autoStart.bootTimeoutReached;
    autoStart["shouldAutoStart"] = wifiSnapshot.autoStart.shouldAutoStart;
    autoStart["startTriggered"] = wifiSnapshot.autoStart.startTriggered;
    autoStart["startSucceeded"] = wifiSnapshot.autoStart.startSucceeded;
}

}  // namespace StatusObservabilityPayload

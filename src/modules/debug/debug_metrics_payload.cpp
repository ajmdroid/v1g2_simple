#include "debug_metrics_payload.h"

#include "../../../include/main_globals.h"
#include "../../ble_client.h"
#include "../../perf_metrics.h"
#include "../wifi/wifi_auto_start_module.h"

namespace DebugApiService {

void appendCameraMetricsPayload(JsonDocument& doc) {
    doc["cameraDisplayActive"] = perfCounters.cameraDisplayActive.load();
    doc["cameraDebugOverrideActive"] = perfCounters.cameraDebugOverrideActive.load();
    doc["cameraDisplayFrames"] = perfCounters.cameraDisplayFrames.load();
    doc["cameraDebugDisplayFrames"] = perfCounters.cameraDebugDisplayFrames.load();
    doc["cameraDisplayMaxUs"] = perfExtended.cameraDisplayMaxUs;
    doc["cameraDebugDisplayMaxUs"] = perfExtended.cameraDebugDisplayMaxUs;
    doc["cameraProcessMaxUs"] = perfExtended.cameraProcessMaxUs;
}

void appendBleRuntimeMetricsPayload(JsonDocument& doc) {
    const BLEState bleState = bleClient.getBLEState();
    doc["bleState"] = bleStateToString(bleState);
    doc["bleStateCode"] = bleClient.getBLEStateCode();
    doc["subscribeStep"] = bleClient.getSubscribeStepName();
    doc["subscribeStepCode"] = bleClient.getSubscribeStepCode();
    doc["connectInProgress"] = bleClient.isConnectInProgress();
    doc["asyncConnectPending"] = bleClient.isAsyncConnectPending();
    doc["pendingDisconnectCleanup"] = bleClient.hasPendingDisconnectCleanup();
    doc["proxyAdvertising"] = bleClient.isProxyAdvertising();
    const uint32_t proxyReasonCode = perfGetProxyAdvertisingLastTransitionReason();
    doc["proxyAdvertisingLastTransitionReason"] =
        perfProxyAdvertisingTransitionReasonName(proxyReasonCode);
    doc["proxyAdvertisingLastTransitionReasonCode"] = proxyReasonCode;
    doc["wifiPriorityMode"] = bleClient.isWifiPriority();
}

void appendWifiAutoStartMetricsPayload(JsonDocument& doc) {
    const WifiAutoStartDecisionSnapshot& snapshot = wifiAutoStartModule.getLastDecision();
    JsonObject wifiAutoStart = doc["wifiAutoStart"].to<JsonObject>();
    wifiAutoStart["gate"] = wifiAutoStartGateName(snapshot.gate);
    wifiAutoStart["gateCode"] = static_cast<uint8_t>(snapshot.gate);
    wifiAutoStart["enableWifi"] = snapshot.enableWifi;
    wifiAutoStart["enableWifiAtBoot"] = snapshot.enableWifiAtBoot;
    wifiAutoStart["bleConnected"] = snapshot.bleConnected;
    wifiAutoStart["v1ConnectedAtMs"] = snapshot.v1ConnectedAtMs;
    wifiAutoStart["msSinceV1Connect"] = snapshot.msSinceV1Connect;
    wifiAutoStart["settleMs"] = snapshot.settleMs;
    wifiAutoStart["bootTimeoutMs"] = snapshot.bootTimeoutMs;
    wifiAutoStart["canStartDma"] = snapshot.canStartDma;
    wifiAutoStart["wifiAutoStartDone"] = snapshot.wifiAutoStartDone;
    wifiAutoStart["bleSettled"] = snapshot.bleSettled;
    wifiAutoStart["bootTimeoutReached"] = snapshot.bootTimeoutReached;
    wifiAutoStart["shouldAutoStart"] = snapshot.shouldAutoStart;
    wifiAutoStart["startTriggered"] = snapshot.startTriggered;
}

}  // namespace DebugApiService

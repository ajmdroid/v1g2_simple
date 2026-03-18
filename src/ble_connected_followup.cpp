#include "ble_client.h"

#include "../include/config.h"
#include "../include/ble_internals.h"
#include "perf_metrics.h"

void V1BLEClient::processConnectedFollowup() {
    switch (connectedFollowupStep) {
        case ConnectedFollowupStep::NONE:
            return;
        case ConnectedFollowupStep::REQUEST_ALERT_DATA:
            {
                const uint32_t startUs = micros();
                const bool ok = requestAlertData();
                perfRecordBleFollowupRequestAlertUs(micros() - startUs);
                if (!ok) {
                    Serial.println("[BLE] Failed to request alert data (non-critical)");
                }
            }
            connectBurstStableLoopCount = 0;
            connectedFollowupStep = ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE;
            return;
        case ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE: {
            const uint32_t bleProcessUs =
                lastBleProcessDurationUs.load(std::memory_order_relaxed);
            const uint32_t displayPipeUs =
                lastDisplayPipelineDurationUs.load(std::memory_order_relaxed);
            const bool bleStable = bleProcessUs <= CONNECT_BURST_STABLE_BLE_MAX_US;
            const bool displayStable =
                (displayPipeUs == 0u || displayPipeUs <= CONNECT_BURST_STABLE_DISP_MAX_US);
            if (bleStable && displayStable) {
                if (connectBurstStableLoopCount < 0xFF) {
                    ++connectBurstStableLoopCount;
                }
            } else {
                connectBurstStableLoopCount = 0;
            }

            const uint32_t nowMs = millis();
            const uint32_t connectedAtMs =
                connectCompletedAtMs.load(std::memory_order_relaxed);
            const uint32_t firstRxMs =
                firstRxAfterConnectMs.load(std::memory_order_relaxed);
            const bool firstRxSeen =
                firstRxMs != 0u &&
                connectedAtMs != 0u &&
                static_cast<int32_t>(firstRxMs - connectedAtMs) >= 0;
            const uint32_t settleStartMs = firstRxSeen ? firstRxMs : connectedAtMs;
            const uint32_t settleBudgetMs =
                firstRxSeen ? CONNECT_BURST_SETTLE_AFTER_FIRST_RX_MS
                            : CONNECT_BURST_SETTLE_AFTER_CONNECTED_MS;
            const bool timedOut =
                settleStartMs != 0u &&
                static_cast<int32_t>(nowMs - (settleStartMs + settleBudgetMs)) >= 0;

            if (connectBurstStableLoopCount >= CONNECT_BURST_STABLE_CONSECUTIVE_LOOPS || timedOut) {
                connectedFollowupStep = ConnectedFollowupStep::REQUEST_VERSION;
            }
            return;
        }
        case ConnectedFollowupStep::REQUEST_VERSION:
            {
                const uint32_t startUs = micros();
                const bool ok = requestVersion();
                perfRecordBleFollowupRequestVersionUs(micros() - startUs);
                if (!ok) {
                    Serial.println("[BLE] Failed to request version (non-critical)");
                }
            }
            connectedFollowupStep = ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK;
            return;
        case ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK:
            if (connectStableCallback) {
                const uint32_t startUs = micros();
                connectStableCallback();
                perfRecordBleConnectStableCallbackUs(micros() - startUs);
            }
            connectedFollowupStep = ConnectedFollowupStep::SCHEDULE_PROXY_ADVERTISING;
            return;
        case ConnectedFollowupStep::SCHEDULE_PROXY_ADVERTISING:
            if (proxyEnabled && proxyServerInitialized) {
                proxyAdvertisingStartMs = millis() + PROXY_STABILIZE_MS;
                proxyAdvertisingStartReasonCode =
                    static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartConnected);
            }
            connectedFollowupStep = ConnectedFollowupStep::BACKUP_BONDS;
            return;
        case ConnectedFollowupStep::BACKUP_BONDS: {
            const uint8_t currentBondCount = static_cast<uint8_t>(NimBLEDevice::getNumBonds());
            if (lastBondBackupCount != currentBondCount) {
                pendingBondBackup = true;
                pendingBondBackupCount = currentBondCount;
                pendingBondBackupRetryAtMs = 0;
            }
            connectedFollowupStep = ConnectedFollowupStep::NONE;
            return;
        }
    }
}

void V1BLEClient::serviceDeferredBondBackup(uint32_t nowMs) {
    if (!pendingBondBackup) {
        return;
    }

    if (pendingBondBackupCount == lastBondBackupCount) {
        pendingBondBackup = false;
        pendingBondBackupRetryAtMs = 0;
        return;
    }

    if (pendingBondBackupRetryAtMs != 0 &&
        static_cast<int32_t>(nowMs - pendingBondBackupRetryAtMs) < 0) {
        return;
    }

    const int backed = tryBackupBondsToSD();
    if (backed >= 0) {
        lastBondBackupCount = pendingBondBackupCount;
        pendingBondBackup = false;
        pendingBondBackupRetryAtMs = 0;
        return;
    }

    pendingBondBackupRetryAtMs = nowMs + DEFERRED_BOND_BACKUP_RETRY_MS;
}

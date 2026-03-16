#include "ble_client.h"

#include "../include/config.h"
#include "../include/ble_internals.h"

void V1BLEClient::processConnectedFollowup() {
    switch (connectedFollowupStep) {
        case ConnectedFollowupStep::NONE:
            return;
        case ConnectedFollowupStep::REQUEST_ALERT_DATA:
            if (!requestAlertData()) {
                Serial.println("[BLE] Failed to request alert data (non-critical)");
            }
            connectedFollowupStep = ConnectedFollowupStep::REQUEST_VERSION;
            return;
        case ConnectedFollowupStep::REQUEST_VERSION:
            if (!requestVersion()) {
                Serial.println("[BLE] Failed to request version (non-critical)");
            }
            connectedFollowupStep = ConnectedFollowupStep::NOTIFY_CALLBACK;
            return;
        case ConnectedFollowupStep::NOTIFY_CALLBACK:
            if (connectCallback) {
                connectCallback();
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

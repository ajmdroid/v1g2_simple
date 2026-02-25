/**
 * BLE runtime state machine and scan/priority control.
 * Extracted from ble_client.cpp for modularity.
 */

#include "ble_client.h"

#include <cstring>
#include <string>
#include <WiFi.h>
#include "settings.h"
#include "obd_handler.h"
#include "../include/config.h"
#include "../include/ble_internals.h"

void V1BLEClient::process() {
    // Handle deferred BLE callback updates without blocking in callbacks
    if (pendingConnectStateUpdate) {
        if (bleMutex && xSemaphoreTake(bleMutex, 0) == pdTRUE) {
            pendingConnectStateUpdate = false;
            connected = true;
            // Don't set CONNECTED state here - async state machine handles transitions
            // Just set the connected flag; state machine will transition via asyncConnectSuccess
            xSemaphoreGive(bleMutex);
        }
    }
    if (pendingDisconnectCleanup) {
        if (bleMutex && xSemaphoreTake(bleMutex, 0) == pdTRUE) {
            pendingDisconnectCleanup = false;
            connected = false;
            connectInProgress = false;
            connectStartMs = 0;
            proxyClientConnected = false;
            pRemoteService = nullptr;
            pDisplayDataChar = nullptr;
            pCommandChar = nullptr;
            pCommandCharLong = nullptr;
            notifyShortChar.store(nullptr, std::memory_order_relaxed);
            notifyShortCharId.store(0, std::memory_order_relaxed);
            notifyLongChar.store(nullptr, std::memory_order_relaxed);
            notifyLongCharId.store(0, std::memory_order_relaxed);
            verifyPending = false;
            verifyComplete = false;
            verifyMatch = false;
            setBLEState(BLEState::DISCONNECTED, "deferred onDisconnect");
            xSemaphoreGive(bleMutex);
        }
    }
    // Deferred bond deletion (NVS write moved out of BLE callback)
    if (pendingDeleteBond) {
        pendingDeleteBond = false;
        if (NimBLEDevice::isBonded(pendingDeleteBondAddr)) {
            NimBLEDevice::deleteBond(pendingDeleteBondAddr);
        }
    }
    if (pendingScanEndUpdate) {
        if (bleMutex && xSemaphoreTake(bleMutex, 0) == pdTRUE) {
            pendingScanEndUpdate = false;
            if (bleState == BLEState::SCANNING) {
                setBLEState(BLEState::DISCONNECTED, "scan ended without finding V1 (deferred)");
            }
            xSemaphoreGive(bleMutex);
        }
    }

    if (proxyCmdPending) {
        uint8_t packetId = 0;
        uint8_t packetLen = 0;
        uint8_t packetBuf[8] = {0};
        portENTER_CRITICAL(&proxyCmdMux);
        if (proxyCmdPending) {
            proxyCmdPending = false;
            packetId = proxyCmdId;
            packetLen = proxyCmdLen;
            memcpy(packetBuf, proxyCmdBuf, sizeof(packetBuf));
        }
        portEXIT_CRITICAL(&proxyCmdMux);

        if (packetId == PACKET_ID_WRITE_USER_BYTES ||
            packetId == PACKET_ID_REQ_WRITE_VOLUME ||
            packetId == PACKET_ID_TURN_OFF_DISPLAY ||
            packetId == PACKET_ID_TURN_ON_DISPLAY ||
            packetId == PACKET_ID_MUTE_ON ||
            packetId == PACKET_ID_MUTE_OFF ||
            packetId == 0x36) {
            Serial.printf("[ProxyCmd] id=0x%02X len=%u bytes=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                           packetId,
                           packetLen,
                           packetBuf[0], packetBuf[1], packetBuf[2], packetBuf[3],
                           packetBuf[4], packetBuf[5], packetBuf[6], packetBuf[7]);
        }
    }

    if (pendingScanTargetUpdate) {
        if (bleMutex && xSemaphoreTake(bleMutex, 0) == pdTRUE) {
            char addrCopy[sizeof(pendingScanTargetAddress)] = {0};
            uint8_t addrTypeCopy = BLE_ADDR_PUBLIC;
            bool havePending = false;
            portENTER_CRITICAL(&pendingAddrMux);
            if (pendingScanTargetUpdate) {
                pendingScanTargetUpdate = false;
                memcpy(addrCopy, pendingScanTargetAddress, sizeof(pendingScanTargetAddress));
                addrCopy[sizeof(addrCopy) - 1] = '\0';
                addrTypeCopy = pendingScanTargetAddressType;
                havePending = true;
            }
            portEXIT_CRITICAL(&pendingAddrMux);
            if (havePending) {
                targetAddress = NimBLEAddress(std::string(addrCopy), addrTypeCopy);
                targetAddressType = addrTypeCopy;
                hasTargetDevice = true;
                shouldConnect = true;
                scanStopRequestedMs = millis();
                setBLEState(BLEState::SCAN_STOPPING, "V1 found (deferred)");
            }
            xSemaphoreGive(bleMutex);
        }
    }
    if (pendingLastV1AddressValid) {
        char addrCopy[sizeof(pendingLastV1Address)] = {0};
        bool shouldWrite = false;
        portENTER_CRITICAL(&pendingAddrMux);
        if (pendingLastV1AddressValid) {
            pendingLastV1AddressValid = false;
            memcpy(addrCopy, pendingLastV1Address, sizeof(pendingLastV1Address));
            addrCopy[sizeof(addrCopy) - 1] = '\0';
            shouldWrite = true;
        }
        portEXIT_CRITICAL(&pendingAddrMux);
        if (shouldWrite) {
            settingsManager.setLastV1Address(addrCopy);
        }
    }
    if (pendingObdScanComplete) {
        pendingObdScanComplete = false;
        obdHandler.onScanComplete();
    }
    // Drain deferred OBD scan results in main loop context.
    while (obdScanCount > 0) {
        ObdScanItem item;
        bool haveItem = false;
        portENTER_CRITICAL(&obdScanMux);
        if (obdScanCount > 0) {
            item = obdScanQueue[obdScanTail];
            obdScanTail = (obdScanTail + 1) % OBD_SCAN_QUEUE_SIZE;
            obdScanCount--;
            haveItem = true;
        }
        portEXIT_CRITICAL(&obdScanMux);
        if (!haveItem) {
            break;
        }
        obdHandler.onDeviceFoundDeferred(item.name, item.addr, item.rssi);
    }
    // Process phone->V1 commands (up to queue size per loop to drain any backlog)
    // Each call processes one command to minimize mutex hold time during BLE writes
    for (int i = 0; i < MAX_PHONE_CMDS_PER_LOOP; i++) {
        if (processPhoneCommandQueue() == 0) {
            break;
        }
    }

    // Enforce boot-lifetime proxy no-client timeout.
    if (proxyEnabled && proxyServerInitialized && !proxyNoClientTimeoutLatched &&
        !proxyClientConnectedOnceThisBoot && proxyNoClientDeadlineMs != 0) {
        const unsigned long nowMs = millis();
        if (static_cast<int32_t>(nowMs - proxyNoClientDeadlineMs) >= 0) {
            proxyNoClientTimeoutLatched = true;
            proxyAdvertisingStartMs = 0;
            proxyAdvertisingWindowStartMs = 0;
            proxyAdvertisingRetryAtMs = 0;
            NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
            if (pAdv && pAdv->isAdvertising()) {
                NimBLEDevice::stopAdvertising();
            }
            Serial.printf("[BLE] Proxy disabled until reboot (no client connected within %lus)\n",
                          static_cast<unsigned long>(PROXY_NO_CLIENT_TIMEOUT_MS / 1000));
        }
    }

    // Handle deferred proxy advertising start (non-blocking replacement for delay(1500))
    if (!proxyNoClientTimeoutLatched &&
        proxyAdvertisingStartMs != 0 && millis() >= proxyAdvertisingStartMs) {
        proxyAdvertisingStartMs = 0;  // Clear pending flag

        if (isConnected() && proxyEnabled && proxyServerInitialized) {
            // Advertising data already configured in initProxyServer() with proper flags
            startProxyAdvertising();
        }
    }

    // Throttle idle proxy advertising while STA is connected:
    // advertise for a bounded window, then pause before retrying.
    if (isConnected() && proxyEnabled && proxyServerInitialized &&
        !wifiPriorityMode && !proxyNoClientTimeoutLatched) {
        const bool staConnected = (WiFi.status() == WL_CONNECTED);
        const bool proxyConnected = proxyClientConnected.load();
        NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
        const bool advertising = pAdv && pAdv->isAdvertising();

        if (proxyConnected) {
            proxyAdvertisingWindowStartMs = 0;
            proxyAdvertisingRetryAtMs = 0;
        } else if (staConnected) {
            const unsigned long nowMs = millis();
            if (advertising) {
                if (proxyAdvertisingWindowStartMs == 0) {
                    proxyAdvertisingWindowStartMs = nowMs;
                } else if ((nowMs - proxyAdvertisingWindowStartMs) >= PROXY_ADVERTISING_WINDOW_MS) {
                    NimBLEDevice::stopAdvertising();
                    proxyAdvertisingWindowStartMs = 0;
                    proxyAdvertisingRetryAtMs = nowMs + PROXY_ADVERTISING_RETRY_MS;
                    Serial.println("[BLE] Proxy idle window elapsed; pausing advertising");
                }
            } else if (proxyAdvertisingRetryAtMs != 0 && nowMs >= proxyAdvertisingRetryAtMs) {
                proxyAdvertisingRetryAtMs = 0;
                proxyAdvertisingStartMs = nowMs + 200;
                Serial.println("[BLE] Proxy retry window opened; resuming advertising");
            }
        } else {
            proxyAdvertisingWindowStartMs = 0;
            proxyAdvertisingRetryAtMs = 0;
        }
    }

    unsigned long now = millis();
    NimBLEScan* pScan = NimBLEDevice::getScan();

    // Boot readiness gate: keep state machine idle until setup opens the gate.
    if (!bootReadyFlag) {
        return;
    }

    // ========== BLE STATE MACHINE ==========
    switch (bleState) {
        case BLEState::DISCONNECTED: {
            // Skip scanning if WiFi priority mode is active
            if (wifiPriorityMode) {
                return;
            }

            // Not connected - start scanning (with backoff check)
            if (consecutiveConnectFailures > 0 && now < nextConnectAllowedMs) {
                // Still in backoff - don't scan yet
                return;
            }

            if (!pScan->isScanning() && (now - lastScanStart >= RECONNECT_DELAY)) {
                lastScanStart = now;
                pScan->clearResults();
                bool started = pScan->start(SCAN_DURATION, false, false);
                if (started) {
                    setBLEState(BLEState::SCANNING, "scan started");
                }
            }
            break;
        }

        case BLEState::SCANNING: {
            // Check if scan found a device (shouldConnect flag set by callback)
            bool wantConnect = false;
            {
                // HOT PATH: try-lock only, skip if busy
                SemaphoreGuard lock(bleMutex, 0);
                if (lock.locked()) {
                    wantConnect = shouldConnect;
                }
            }

            if (wantConnect) {
                // V1 found - stop scan and transition to SCAN_STOPPING
                if (pScan->isScanning()) {
                    pScan->stop();
                    scanStopRequestedMs = now;
                    setBLEState(BLEState::SCAN_STOPPING, "V1 found during scan");
                } else {
                    // Scan already stopped, proceed directly
                    scanStopRequestedMs = now;
                    setBLEState(BLEState::SCAN_STOPPING, "scan already stopped");
                }
            }
            // Note: Scan ending without finding device will restart via scan callbacks
            break;
        }

        case BLEState::SCAN_STOPPING: {
            // Wait for scan to fully stop and radio to settle
            unsigned long elapsed = now - scanStopRequestedMs;

            // Ensure scan is actually stopped
            if (pScan->isScanning()) {
                if (elapsed > 1000) {  // Force stop if taking too long
                    pScan->stop();
                }
                return;  // Wait more
            }

            // Clear scan results once scan has stopped
            static bool resultsCleared = false;
            if (!resultsCleared && elapsed > 100) {  // Clear after brief delay
                pScan->clearResults();
                resultsCleared = true;
            }

            // Check if settle time has elapsed
            // Use longer settle on first scan after boot (radio is "cold")
            unsigned long settleTime = firstScanAfterBoot ? SCAN_STOP_SETTLE_FRESH_MS : SCAN_STOP_SETTLE_MS;
            if (elapsed >= settleTime) {
                if (firstScanAfterBoot) {
                    Serial.println("[BLE] First scan settle complete (extended)");
                    firstScanAfterBoot = false;
                }
                resultsCleared = false;  // Reset for next time
                // Ready to connect
                bool wantConnect = false;
                {
                    // HOT PATH: try-lock only, skip if busy
                    SemaphoreGuard lock(bleMutex, 0);
                    if (lock.locked()) {
                        wantConnect = shouldConnect;
                        shouldConnect = false;  // Clear flag
                    }
                }

                if (wantConnect) {
                    connectToServer();  // This will set state to CONNECTING
                } else {
                    setBLEState(BLEState::DISCONNECTED, "no connect pending");
                }
            }
            break;
        }

        case BLEState::CONNECTING: {
            if (nextConnectAllowedMs != 0 && now < nextConnectAllowedMs) {
                break;
            }

            // Initiate at most one async connect attempt per loop iteration.
            // startAsyncConnect() transitions to CONNECTING_WAIT on successful initiation.
            if (!asyncConnectPending && !asyncConnectSuccess) {
                startAsyncConnect();
                if (bleState != BLEState::CONNECTING) {
                    break;
                }
            }

            // If we're stuck here for too long, something is wrong.
            if (connectStartMs > 0 && (now - connectStartMs) > 5000) {
                Serial.println("[BLE] Connect initiation stuck for 5s - resetting");
                connectInProgress = false;
                connectStartMs = 0;
                asyncConnectPending = false;
                setBLEState(BLEState::DISCONNECTED, "connect initiation timeout");
            }
            break;
        }

        case BLEState::CONNECTING_WAIT: {
            // Waiting for async connect callback
            processConnectingWait();
            break;
        }

        case BLEState::DISCOVERING: {
            // Performing service discovery
            processDiscovering();
            break;
        }

        case BLEState::SUBSCRIBING: {
            // Subscribing to characteristics (step machine)
            processSubscribing();
            break;
        }

        case BLEState::SUBSCRIBE_YIELD: {
            // Brief yield between subscribe steps to let loop() run
            processSubscribeYield();
            break;
        }

        case BLEState::CONNECTED: {
            // All good - nothing to do in state machine
            // Verify we're actually still connected
            if (!pClient || !pClient->isConnected()) {
                connected = false;
                connectInProgress = false;
                setBLEState(BLEState::DISCONNECTED, "connection lost");
            }
            break;
        }

        case BLEState::BACKOFF: {
            // Waiting for backoff period to expire
            if (now >= nextConnectAllowedMs) {
                setBLEState(BLEState::DISCONNECTED, "backoff expired");
            }
            break;
        }
    }
}

void V1BLEClient::startScanning() {
    if (!isConnected() && bleState == BLEState::DISCONNECTED) {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (!pScan->isScanning()) {
            lastScanStart = millis();
            pScan->clearResults();
            bool started = pScan->start(SCAN_DURATION, false, false);
            if (started) {
                setBLEState(BLEState::SCANNING, "manual scan start");
            }
        }
    }
}

void V1BLEClient::startOBDScan() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan) {
        return;
    }

    if (!pScan->isScanning()) {
        Serial.println("[BLE] Starting OBD device scan (30 seconds)...");
        pScan->clearResults();
        pScan->start(30000, false, false);
        return;
    }

    Serial.println("[BLE] Scan already in progress - OBD devices will be collected");
}

bool V1BLEClient::isScanning() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    return pScan && pScan->isScanning();
}

NimBLEAddress V1BLEClient::getConnectedAddress() const {
    if (pClient && pClient->isConnected()) {
        return pClient->getPeerAddress();
    }
    return NimBLEAddress();  // Default constructor for empty address
}

void V1BLEClient::disconnect() {
    if (pClient && pClient->isConnected()) {
        pClient->disconnect();
    }
}

// ==================== WiFi Priority Mode ====================
// Deprioritize BLE when web UI is active to maximize responsiveness

void V1BLEClient::setBootReady(bool ready) {
    bootReadyFlag = ready;
}

void V1BLEClient::setWifiPriority(bool enabled) {
    if (wifiPriorityMode == enabled) return;  // No change

    wifiPriorityMode = enabled;

    // Rate-limit transition logs to avoid serial spam if caller oscillates.
    static unsigned long lastLogMs = 0;
    const unsigned long nowMs = millis();
    const bool shouldLog = (nowMs - lastLogMs) >= 10000;  // At most once per 10s
    if (shouldLog) lastLogMs = nowMs;

    if (enabled) {
        if (shouldLog) Serial.println("[BLE] WiFi priority ENABLED - suppressing scans/reconnects/proxy");

        // Stop any active scan
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan && pScan->isScanning()) {
            Serial.println("[BLE] Stopping scan for WiFi priority mode");
            pScan->stop();
            pScan->clearResults();
        }

        // Stop proxy advertising if running
        if (proxyEnabled && NimBLEDevice::getAdvertising()->isAdvertising()) {
            if (shouldLog) Serial.println("[BLE] Stopping proxy advertising for WiFi priority mode");
            NimBLEDevice::stopAdvertising();
        }

        // Cancel any pending deferred advertising start
        proxyAdvertisingStartMs = 0;
        proxyAdvertisingWindowStartMs = 0;

        // Note: We keep existing V1 connection if already connected
        // to avoid disrupting active radar detection

    } else {
        if (shouldLog) Serial.println("[BLE] WiFi priority DISABLED - resuming normal BLE operation");

        // Resume proxy advertising if we're connected and proxy is enabled
        if (isConnected() && proxyEnabled && proxyServerInitialized && !proxyNoClientTimeoutLatched) {
            if (shouldLog) Serial.println("[BLE] Resuming proxy advertising after WiFi priority mode");
            // Defer advertising start by 500ms to avoid stall
            proxyAdvertisingStartMs = millis() + 500;
            proxyAdvertisingWindowStartMs = 0;
        }

        // Resume scanning if disconnected
        if (!isConnected() && bleState == BLEState::DISCONNECTED) {
            Serial.println("[BLE] Resuming scan after WiFi priority mode");
            startScanning();
        }
    }
}

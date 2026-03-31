/**
 * BLE runtime state machine and scan/priority control.
 * Extracted from ble_client.cpp for modularity.
 */

#include "ble_client.h"

#include <cstring>
#include <string>
#include <WiFi.h>
#include "settings.h"
#include "perf_metrics.h"
#include "../include/config.h"
#include "../include/ble_internals.h"

void V1BLEClient::process() {
    // Handle deferred BLE callback updates without blocking in callbacks
    if (pendingConnectStateUpdate_) {
        SemaphoreGuard lock(bleMutex_, 0);
        if (lock.locked()) {
            pendingConnectStateUpdate_ = false;
            connected_.store(true, std::memory_order_relaxed);
            // Don't set CONNECTED state here - async state machine handles transitions
            // Just set the connected_ flag; state machine will transition via asyncConnectSuccess_
        }
    }
    if (pendingDisconnectCleanup_) {
        SemaphoreGuard lock(bleMutex_, 0);
        if (lock.locked()) {
            pendingDisconnectCleanup_ = false;
            connected_.store(false, std::memory_order_relaxed);
            connectInProgress_ = false;
            connectStartMs_ = 0;
            connectedFollowupStep_ = ConnectedFollowupStep::NONE;
            connectCompletedAtMs_.store(0, std::memory_order_relaxed);
            firstRxAfterConnectMs_.store(0, std::memory_order_relaxed);
            lastBleProcessDurationUs_.store(0, std::memory_order_relaxed);
            lastDisplayPipelineDurationUs_.store(0, std::memory_order_relaxed);
            connectBurstStableLoopCount_ = 0;
            proxyClientConnected_ = false;
            proxyDisconnectRequestedForObdPreempt_ = false;
            pRemoteService_ = nullptr;
            pDisplayDataChar_ = nullptr;
            pCommandChar_ = nullptr;
            pCommandCharLong_ = nullptr;
            notifyShortChar_.store(nullptr, std::memory_order_relaxed);
            notifyShortCharId_.store(0, std::memory_order_relaxed);
            notifyLongChar_.store(nullptr, std::memory_order_relaxed);
            notifyLongCharId_.store(0, std::memory_order_relaxed);
            verifyPending_ = false;
            verifyComplete_ = false;
            verifyMatch_ = false;
            setBLEState(BLEState::DISCONNECTED, "deferred onDisconnect");
        }
    }
    // Deferred bond deletion (NVS write moved out of BLE callback)
    if (pendingDeleteBond_) {
        pendingDeleteBond_ = false;
        if (NimBLEDevice::isBonded(pendingDeleteBondAddr_)) {
            NimBLEDevice::deleteBond(pendingDeleteBondAddr_);
        }
    }
    if (pendingScanEndUpdate_) {
        SemaphoreGuard lock(bleMutex_, 0);
        if (lock.locked()) {
            pendingScanEndUpdate_ = false;
            if (bleState_ == BLEState::SCANNING) {
                setBLEState(BLEState::DISCONNECTED, "scan ended without finding V1 (deferred)");
            }
        }
    }

    drainProxyCallbackEvents();

    if (proxyCmdPending_) {
        uint8_t packetId = 0;
        uint8_t packetLen = 0;
        uint8_t packetBuf[8] = {0};
        portENTER_CRITICAL(&proxyCmdMux);
        if (proxyCmdPending_) {
            proxyCmdPending_ = false;
            packetId = proxyCmdId_;
            packetLen = proxyCmdLen_;
            memcpy(packetBuf, proxyCmdBuf_, sizeof(packetBuf));
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

    if (pendingScanTargetUpdate_) {
        SemaphoreGuard lock(bleMutex_, 0);
        if (lock.locked()) {
            char addrCopy[sizeof(pendingScanTargetAddress_)] = {0};
            uint8_t addrTypeCopy = BLE_ADDR_PUBLIC;
            bool havePending = false;
            portENTER_CRITICAL(&pendingAddrMux);
            if (pendingScanTargetUpdate_) {
                pendingScanTargetUpdate_ = false;
                memcpy(addrCopy, pendingScanTargetAddress_, sizeof(pendingScanTargetAddress_));
                addrCopy[sizeof(addrCopy) - 1] = '\0';
                addrTypeCopy = pendingScanTargetAddressType_;
                havePending = true;
            }
            portEXIT_CRITICAL(&pendingAddrMux);
            if (havePending) {
                targetAddress_ = NimBLEAddress(std::string(addrCopy), addrTypeCopy);
                targetAddressType_ = addrTypeCopy;
                hasTargetDevice_ = true;
                shouldConnect_ = true;
                scanStopRequestedMs_ = millis();
                setBLEState(BLEState::SCAN_STOPPING, "V1 found (deferred)");
            }
        }
    }
    if (pendingLastV1AddressValid_) {
        char addrCopy[sizeof(pendingLastV1Address_)] = {0};
        bool shouldWrite = false;
        portENTER_CRITICAL(&pendingAddrMux);
        if (pendingLastV1AddressValid_) {
            pendingLastV1AddressValid_ = false;
            memcpy(addrCopy, pendingLastV1Address_, sizeof(pendingLastV1Address_));
            addrCopy[sizeof(addrCopy) - 1] = '\0';
            shouldWrite = true;
        }
        portEXIT_CRITICAL(&pendingAddrMux);
        if (shouldWrite) {
            settingsManager.setLastV1Address(addrCopy);
        }
    }
    // Process phone->V1 commands (up to queue size per loop to drain any backlog)
    // Each call processes one command to minimize mutex hold time during BLE writes
    for (int i = 0; i < MAX_PHONE_CMDS_PER_LOOP; i++) {
        if (processPhoneCommandQueue() == 0) {
            break;
        }
    }
    if (connectedFollowupStep_ != ConnectedFollowupStep::NONE && isConnected()) {
        processConnectedFollowup();
    }

    const bool holdProxyForAutoObd =
        obdBleArbitrationRequest_ == ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD;
    const bool preemptProxyForManualScan =
        obdBleArbitrationRequest_ == ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN;
    const bool suppressPassiveProxy = holdProxyForAutoObd || preemptProxyForManualScan;
    const bool proxyConnected = proxyClientConnected_.load(std::memory_order_relaxed);
    NimBLEAdvertising* pProxyAdvertising =
        (proxyEnabled_ && proxyServerInitialized_) ? NimBLEDevice::getAdvertising() : nullptr;
    const bool proxyAdvertisingActive = pProxyAdvertising && pProxyAdvertising->isAdvertising();

    if (suppressPassiveProxy && isConnected() && proxyEnabled_ && proxyServerInitialized_) {
        if (!proxyConnected && proxyAdvertisingActive) {
            proxySuppressedForObdHold_ = true;
            if (proxySuppressedResumeReasonCode_ ==
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown)) {
                proxySuppressedResumeReasonCode_ =
                    static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow);
            }
            stopProxyAdvertisingFromMainLoop(
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopOther));
        }
        if (preemptProxyForManualScan &&
            proxyConnected &&
            !proxyDisconnectRequestedForObdPreempt_ &&
            pServer_ &&
            pServer_->getConnectedCount() > 0) {
            proxyDisconnectRequestedForObdPreempt_ = true;
            proxySuppressedForObdHold_ = true;
            if (proxySuppressedResumeReasonCode_ ==
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown)) {
                proxySuppressedResumeReasonCode_ =
                    static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow);
            }
            // Use getPeerDevices() instead of getPeerInfo(0) to avoid a
            // TOCTOU race: if the phone disconnects between the count
            // check above and the handle lookup, getPeerInfo(0) returns
            // a default NimBLEConnInfo with conn_handle=0 which would
            // inadvertently terminate the V1 client connection.
            for (uint16_t h : pServer_->getPeerDevices()) {
                pServer_->disconnect(h);
            }
        }
    } else if (!suppressPassiveProxy &&
               proxySuppressedForObdHold_ &&
               isConnected() &&
               proxyEnabled_ &&
               proxyServerInitialized_ &&
               !wifiPriorityMode_ &&
               !proxyNoClientTimeoutLatched_ &&
               !proxyConnected &&
               !proxyAdvertisingActive &&
               proxyAdvertisingStartMs_ == 0 &&
               proxyAdvertisingRetryAtMs_ == 0) {
        proxyAdvertisingStartMs_ = millis() + PROXY_STABILIZE_MS;
        proxyAdvertisingStartReasonCode_ =
            proxySuppressedResumeReasonCode_ == 0
                ? static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow)
                : proxySuppressedResumeReasonCode_;
        proxySuppressedForObdHold_ = false;
        proxySuppressedResumeReasonCode_ =
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
    }

    // Enforce boot-lifetime proxy no-client timeout.
    if (proxyEnabled_ && proxyServerInitialized_ && !proxyNoClientTimeoutLatched_ &&
        !proxyClientConnectedOnceThisBoot_ && proxyNoClientDeadlineMs_ != 0) {
        const unsigned long nowMs = millis();
        if (static_cast<int32_t>(nowMs - proxyNoClientDeadlineMs_) >= 0) {
            proxyNoClientTimeoutLatched_ = true;
            proxyAdvertisingStartMs_ = 0;
            proxyAdvertisingStartReasonCode_ =
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
            proxyAdvertisingWindowStartMs_ = 0;
            proxyAdvertisingRetryAtMs_ = 0;
            NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
            if (pAdv && pAdv->isAdvertising()) {
                NimBLEDevice::stopAdvertising();
                perfRecordProxyAdvertisingTransition(
                    false,
                    static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopNoClientTimeout),
                    nowMs);
            }
            Serial.printf("[BLE] Proxy disabled until reboot (no client connected_ within %lus)\n",
                          static_cast<unsigned long>(PROXY_NO_CLIENT_TIMEOUT_MS / 1000));
        }
    }

    // Handle deferred proxy advertising start (non-blocking replacement for delay(1500))
    if (!proxyNoClientTimeoutLatched_ &&
        proxyAdvertisingStartMs_ != 0 && static_cast<int32_t>(millis() - proxyAdvertisingStartMs_) >= 0) {
        if (suppressPassiveProxy && !proxyConnected) {
            proxySuppressedForObdHold_ = true;
            if (proxySuppressedResumeReasonCode_ ==
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown)) {
                proxySuppressedResumeReasonCode_ = proxyAdvertisingStartReasonCode_;
            }
        } else if (isConnected() && proxyEnabled_ && proxyServerInitialized_) {
            const uint8_t startReason = proxyAdvertisingStartReasonCode_;
            proxyAdvertisingStartMs_ = 0;  // Clear pending flag
            proxyAdvertisingStartReasonCode_ =
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
            // Advertising data already configured in initProxyServer() with proper flags
            startProxyAdvertising(startReason);
        } else {
            proxyAdvertisingStartMs_ = 0;
            proxyAdvertisingStartReasonCode_ =
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
        }
    }

    // Throttle idle proxy advertising while STA is connected_:
    // advertise for a bounded window, then pause before retrying.
    if (isConnected() && proxyEnabled_ && proxyServerInitialized_ &&
        !wifiPriorityMode_ && !proxyNoClientTimeoutLatched_) {
        const bool staConnected = (WiFi.status() == WL_CONNECTED);
        NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
        const bool advertising = pAdv && pAdv->isAdvertising();

        if (proxyConnected) {
            proxyAdvertisingWindowStartMs_ = 0;
            proxyAdvertisingRetryAtMs_ = 0;
        } else if (staConnected) {
            const unsigned long nowMs = millis();
            if (advertising) {
                if (proxyAdvertisingWindowStartMs_ == 0) {
                    proxyAdvertisingWindowStartMs_ = nowMs;
                } else if ((nowMs - proxyAdvertisingWindowStartMs_) >= PROXY_ADVERTISING_WINDOW_MS) {
                    NimBLEDevice::stopAdvertising();
                    perfRecordProxyAdvertisingTransition(
                        false,
                        static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopIdleWindow),
                        nowMs);
                    proxyAdvertisingWindowStartMs_ = 0;
                    proxyAdvertisingRetryAtMs_ = nowMs + PROXY_ADVERTISING_RETRY_MS;
                    Serial.println("[BLE] Proxy idle window elapsed; pausing advertising");
                }
            } else if (proxyAdvertisingRetryAtMs_ != 0 && static_cast<int32_t>(nowMs - proxyAdvertisingRetryAtMs_) >= 0) {
                proxyAdvertisingRetryAtMs_ = 0;
                proxyAdvertisingStartMs_ = nowMs + 200;
                proxyAdvertisingStartReasonCode_ =
                    static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow);
                Serial.println("[BLE] Proxy retry window opened; resuming advertising");
            }
        } else {
            proxyAdvertisingWindowStartMs_ = 0;
            proxyAdvertisingRetryAtMs_ = 0;
        }
    }

    unsigned long now = millis();
    NimBLEScan* pScan = NimBLEDevice::getScan();

    // Boot readiness gate: keep state machine idle until setup opens the gate.
    if (!bootReadyFlag_) {
        return;
    }

    // ============================================================================
    // BLE STATE MACHINE
    // ============================================================================
    switch (bleState_) {
        case BLEState::DISCONNECTED: {
            // Skip scanning if WiFi priority mode is active
            if (wifiPriorityMode_) {
                return;
            }

            // Not connected_ - start scanning (with backoff check)
            if (consecutiveConnectFailures_ > 0 && static_cast<int32_t>(now - nextConnectAllowedMs_) < 0) {
                // Still in backoff - don't scan yet
                return;
            }

            if (!pScan->isScanning() && (now - lastScanStart_ >= RECONNECT_DELAY)) {
                lastScanStart_ = now;
                pScan->clearResults();
                bool started = pScan->start(SCAN_DURATION, false, false);
                if (started) {
                    setBLEState(BLEState::SCANNING, "scan started");
                }
            }
            break;
        }

        case BLEState::SCANNING: {
            // Check if scan found a device (shouldConnect_ flag set by callback)
            bool wantConnect = false;
            {
                // HOT PATH: try-lock only, skip if busy
                SemaphoreGuard lock(bleMutex_, 0);
                if (lock.locked()) {
                    wantConnect = shouldConnect_;
                }
            }

            if (wantConnect) {
                // V1 found - stop scan and transition to SCAN_STOPPING
                if (pScan->isScanning()) {
                    pScan->stop();
                    scanStopRequestedMs_ = now;
                    setBLEState(BLEState::SCAN_STOPPING, "V1 found during scan");
                } else {
                    // Scan already stopped, proceed directly
                    scanStopRequestedMs_ = now;
                    setBLEState(BLEState::SCAN_STOPPING, "scan already stopped");
                }
            }
            // Note: Scan ending without finding device will restart via scan callbacks
            break;
        }

        case BLEState::SCAN_STOPPING: {
            // Wait for scan to fully stop and radio to settle
            unsigned long elapsed = now - scanStopRequestedMs_;

            // Ensure scan is actually stopped
            if (pScan->isScanning()) {
                if (elapsed > 1000) {  // Force stop if taking too long
                    pScan->stop();
                }
                return;  // Wait more
            }

            // Clear scan results once scan has stopped
            if (!scanStopResultsCleared_ && elapsed > 100) {  // Clear after brief delay
                pScan->clearResults();
                scanStopResultsCleared_ = true;
            }

            // Check if settle time has elapsed
            // Use longer settle on first scan after boot (radio is "cold")
            unsigned long settleTime = firstScanAfterBoot_ ? SCAN_STOP_SETTLE_FRESH_MS : SCAN_STOP_SETTLE_MS;
            if (elapsed >= settleTime) {
                if (firstScanAfterBoot_) {
                    Serial.println("[BLE] First scan settle complete (extended)");
                    firstScanAfterBoot_ = false;
                }
                // Ready to connect
                bool wantConnect = false;
                {
                    // HOT PATH: try-lock only, skip if busy
                    SemaphoreGuard lock(bleMutex_, 0);
                    if (lock.locked()) {
                        wantConnect = shouldConnect_;
                        shouldConnect_ = false;  // Clear flag
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
            if (nextConnectAllowedMs_ != 0 && static_cast<int32_t>(now - nextConnectAllowedMs_) < 0) {
                break;
            }

            // Initiate at most one async connect attempt per loop iteration.
            // startAsyncConnect() transitions to CONNECTING_WAIT on successful initiation.
            if (!asyncConnectPending_ && !asyncConnectSuccess_) {
                startAsyncConnect();
                if (bleState_ != BLEState::CONNECTING) {
                    break;
                }
            }

            // If we're stuck here for too long, something is wrong.
            if (connectStartMs_ > 0 && (now - connectStartMs_) > 5000) {
                Serial.println("[BLE] Connect initiation stuck for 5s - resetting");
                connectInProgress_ = false;
                connectStartMs_ = 0;
                asyncConnectPending_ = false;
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
            // Verify we're actually still connected_
            if (!pClient_ || !pClient_->isConnected()) {
                connected_.store(false, std::memory_order_relaxed);
                connectInProgress_ = false;
                setBLEState(BLEState::DISCONNECTED, "connection lost");
            }
            break;
        }

        case BLEState::BACKOFF: {
            // Waiting for backoff period to expire
            if (static_cast<int32_t>(now - nextConnectAllowedMs_) >= 0) {
                setBLEState(BLEState::DISCONNECTED, "backoff expired");
            }
            break;
        }
    }
}

void V1BLEClient::startScanning() {
    if (!isConnected() && bleState_ == BLEState::DISCONNECTED) {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (!pScan->isScanning()) {
            lastScanStart_ = millis();
            pScan->clearResults();
            bool started = pScan->start(SCAN_DURATION, false, false);
            if (started) {
                setBLEState(BLEState::SCANNING, "manual scan start");
            }
        }
    }
}

bool V1BLEClient::isScanning() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    return pScan && pScan->isScanning();
}

NimBLEAddress V1BLEClient::getConnectedAddress() const {
    if (pClient_ && pClient_->isConnected()) {
        return pClient_->getPeerAddress();
    }
    return NimBLEAddress();  // Default constructor for empty address
}

void V1BLEClient::disconnect() {
    if (pClient_ && pClient_->isConnected()) {
        pClient_->disconnect();
    }
}

// ============================================================================
// WiFi Priority Mode
// ============================================================================
// Deprioritize BLE when web UI is active to maximize responsiveness

void V1BLEClient::setBootReady(bool ready) {
    bootReadyFlag_ = ready;
}

void V1BLEClient::setWifiPriority(bool enabled) {
    if (wifiPriorityMode_ == enabled) return;  // No change

    wifiPriorityMode_ = enabled;

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
        if (proxyEnabled_ && NimBLEDevice::getAdvertising()->isAdvertising()) {
            if (shouldLog) Serial.println("[BLE] Stopping proxy advertising for WiFi priority mode");
            NimBLEDevice::stopAdvertising();
            perfRecordProxyAdvertisingTransition(
                false,
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopWifiPriority),
                nowMs);
        }

        // Cancel any pending deferred advertising start
        proxyAdvertisingStartMs_ = 0;
        proxyAdvertisingStartReasonCode_ =
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
        proxyAdvertisingWindowStartMs_ = 0;

        // Note: We keep existing V1 connection if already connected_
        // to avoid disrupting active radar detection

    } else {
        if (shouldLog) Serial.println("[BLE] WiFi priority DISABLED - resuming normal BLE operation");

        // Resume proxy advertising if we're connected_ and proxy is enabled
        if (isConnected() && proxyEnabled_ && proxyServerInitialized_ && !proxyNoClientTimeoutLatched_) {
            if (shouldLog) Serial.println("[BLE] Resuming proxy advertising after WiFi priority mode");
            // Defer advertising start by 500ms to avoid stall
            proxyAdvertisingStartMs_ = millis() + 500;
            proxyAdvertisingStartReasonCode_ =
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartWifiPriorityResume);
            proxyAdvertisingWindowStartMs_ = 0;
        }

        // Resume scanning if disconnected
        if (!isConnected() && bleState_ == BLEState::DISCONNECTED) {
            Serial.println("[BLE] Resuming scan after WiFi priority mode");
            startScanning();
        }
    }
}

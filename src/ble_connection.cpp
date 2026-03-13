/**
 * BLE connection state machine for Valentine1 Gen2
 *
 * Extracted from ble_client.cpp — scanning, connecting, service discovery,
 * characteristic subscription, and NimBLE callbacks.
 */

#include "ble_client.h"
#include "../include/ble_internals.h"
#include "../include/config.h"
#include "perf_metrics.h"
#include <cstring>
#include <esp_heap_caps.h>

// Bond backup helper defined in ble_client.cpp (non-static)
extern int backupBondsToSD();

// ---- scan callbacks ---------------------------------------------------

void V1BLEClient::ScanCallbacks::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    if (!bleClient) {
        return;
    }

    const std::string& name = advertisedDevice->getName();
    const std::string& addrStr = advertisedDevice->getAddress().toString();
    int rssi = advertisedDevice->getRSSI();
    
    // Ignore our own proxy advertisement to avoid self-connect loops
    if (bleClient->proxyEnabled) {
        NimBLEAddress selfAddr = NimBLEDevice::getAddress();
        if (advertisedDevice->getAddress() == selfAddr) {
            return;
        }
    }
    
    // V1 discovery should tolerate missing scan-response names: some stacks expose
    // the V1 service UUID without a readable name in the advertisement payload.
    bool nameLooksV1 = false;
    if (name.length() >= 3) {
        const char c0 = static_cast<char>(name[0] | 0x20);  // lowercase
        const char c1 = static_cast<char>(name[1] | 0x20);
        const char c2 = static_cast<char>(name[2] | 0x20);
        // Field variants: V1G..., V1C..., and V1-... have all been observed.
        nameLooksV1 = (c0 == 'v' && c1 == '1' && (c2 == 'g' || c2 == 'c' || c2 == '-'));
    }
    static const NimBLEUUID kV1ServiceUuid(V1_SERVICE_UUID);
    const bool serviceLooksV1 = advertisedDevice->isAdvertisingService(kV1ServiceUuid);
    const bool isV1 = nameLooksV1 || serviceLooksV1;
    
    if (!isV1) {
        // Not a V1 device, keep scanning
        return;
    }
    
    // *** FOUND V1! Stop scan and queue connection ***
    int advAddrType = advertisedDevice->getAddressType();
    
    // Check if we're already connecting or connected
    if (bleClient->bleState == BLEState::CONNECTING || 
        bleClient->bleState == BLEState::CONNECTED) {
        return;
    }
    
    // Save this address for future fast reconnects (deferred to main loop)
    bleClient->deferLastV1Address(addrStr.c_str());
    
    // Stop scanning - state machine will handle the connection after settle time
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) {
        pScan->stop();
    }
    
    // Queue connection to this V1 device (non-blocking lock to avoid BLE callback stalls)
    // IMPORTANT: Don't copy full NimBLEAdvertisedDevice - it allocates memory which can fail
    // during heap pressure. Just store the address and type.
    if (bleClient->bleMutex && xSemaphoreTake(bleClient->bleMutex, 0) == pdTRUE) {
        // Store just the address (no heap allocation)
        bleClient->targetAddress = advertisedDevice->getAddress();
        bleClient->targetAddressType = advAddrType;  // Save for reconnect
        bleClient->hasTargetDevice = true;
        bleClient->shouldConnect = true;
        bleClient->scanStopRequestedMs = millis();
        bleClient->setBLEState(BLEState::SCAN_STOPPING, "V1 found");
        xSemaphoreGive(bleClient->bleMutex);
    } else {
        // Defer update to main loop if mutex is busy
        portENTER_CRITICAL(&pendingAddrMux);
        snprintf(bleClient->pendingScanTargetAddress, sizeof(bleClient->pendingScanTargetAddress), "%s", addrStr.c_str());
        bleClient->pendingScanTargetAddressType = static_cast<uint8_t>(advAddrType);
        bleClient->pendingScanTargetUpdate = true;
        portEXIT_CRITICAL(&pendingAddrMux);
    }
}

void V1BLEClient::ScanCallbacks::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
    // If we were SCANNING and scan ended without finding V1, go back to DISCONNECTED
    // to allow process() to restart the scan
    if (instancePtr) {
        if (instancePtr->bleMutex && xSemaphoreTake(instancePtr->bleMutex, 0) == pdTRUE) {
            if (instancePtr->bleState == BLEState::SCANNING) {
                // Scan ended without finding V1, go back to DISCONNECTED
                instancePtr->setBLEState(BLEState::DISCONNECTED, "scan ended without finding V1");
            }
            // If SCAN_STOPPING, process() will handle the transition
            xSemaphoreGive(instancePtr->bleMutex);
        } else {
            instancePtr->pendingScanEndUpdate = true;
        }
    }
}

// ---- client callbacks -------------------------------------------------

void V1BLEClient::ClientCallbacks::onConnect(NimBLEClient* pClient) {
    // NOTE: BLE callback - keep fast, no blocking operations
    if (instancePtr) {
        // Signal async connect success (non-blocking atomic write)
        instancePtr->asyncConnectSuccess = true;
        instancePtr->asyncConnectPending = false;
        
        if (instancePtr->bleMutex && xSemaphoreTake(instancePtr->bleMutex, 0) == pdTRUE) {
            instancePtr->connected = true;
            // Don't set CONNECTED state here - let state machine handle it
            // The async state machine will transition through DISCOVERING -> SUBSCRIBING -> CONNECTED
            xSemaphoreGive(instancePtr->bleMutex);
        } else {
            instancePtr->pendingConnectStateUpdate = true;
        }
    }
}

void V1BLEClient::ClientCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
    // NOTE: BLE callback - minimize blocking. Log disconnect reason for diagnostics.
    PERF_INC(disconnects);  // Count V1 disconnections
    if (BLE_CALLBACK_LOGS) {
        BLE_LOGF("[BLE] V1 disconnected (reason: %d)\n", reason);
    }
    
    // If the disconnect was unexpected (e.g., V1 powered off), clear bonding info
    // to ensure a clean reconnect next time.
    // NOTE: deleteBond() does NVS flash write — defer to main loop to avoid
    // blocking NimBLE host task.
    if (reason != 0 && reason != BLE_HS_ETIMEOUT) {
        if (instancePtr) {
            instancePtr->pendingDeleteBondAddr = pClient->getPeerAddress();
            instancePtr->pendingDeleteBond = true;
        }
    }

    if (instancePtr) {
        // Stop proxy advertising FIRST before any state changes
        if (instancePtr->proxyEnabled && NimBLEDevice::getAdvertising()->isAdvertising()) {
            NimBLEDevice::stopAdvertising();
            perfRecordProxyAdvertisingTransition(
                false,
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopV1Disconnect),
                millis());
            // No delay here - callback must return quickly
        }
        instancePtr->proxyAdvertisingStartMs = 0;
        instancePtr->proxyAdvertisingStartReasonCode =
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
        instancePtr->proxyAdvertisingWindowStartMs = 0;
        instancePtr->proxyAdvertisingRetryAtMs = 0;
        
        if (instancePtr->bleMutex && xSemaphoreTake(instancePtr->bleMutex, 0) == pdTRUE) {
            instancePtr->connected = false;
            instancePtr->connectInProgress = false;  // Clear connection guard
            instancePtr->connectStartMs = 0;  // Clear async connect timer
            instancePtr->connectedFollowupStep = ConnectedFollowupStep::NONE;
            // Clear proxy client connection state too - can't proxy without V1 connection
            instancePtr->proxyClientConnected = false;
            // Do NOT clear pClient - we reuse it to prevent memory leaks
            instancePtr->pRemoteService = nullptr;
            instancePtr->pDisplayDataChar = nullptr;
            instancePtr->pCommandChar = nullptr;
            instancePtr->pCommandCharLong = nullptr;
            instancePtr->notifyShortChar.store(nullptr, std::memory_order_relaxed);
            instancePtr->notifyShortCharId.store(0, std::memory_order_relaxed);
            instancePtr->notifyLongChar.store(nullptr, std::memory_order_relaxed);
            instancePtr->notifyLongCharId.store(0, std::memory_order_relaxed);
            // Reset verification state in case a write-verify was in progress
            instancePtr->verifyPending = false;
            instancePtr->verifyComplete = false;
            instancePtr->verifyMatch = false;
            // Set state to DISCONNECTED - will trigger scan restart in process()
            instancePtr->setBLEState(BLEState::DISCONNECTED, "onDisconnect callback");
            xSemaphoreGive(instancePtr->bleMutex);
        } else {
            instancePtr->pendingDisconnectCleanup = true;
        }
    }
}

// ---- connection state machine -----------------------------------------

bool V1BLEClient::connectToServer() {
    std::string addrStr = targetAddress.toString();
    [[maybe_unused]] int addrType = hasTargetDevice ? targetDevice.getAddressType() : targetAddressType;
    
    // ========== CONNECTION GUARDS ==========
    // Prevent overlapping connection attempts which cause EBUSY errors
    
    // Guard 1: Check if already connecting
    if (connectInProgress) {
        return false;
    }
    
    // Guard 2: Check if scanning is still active - must be fully stopped
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
        scanStopRequestedMs = millis();
        setBLEState(BLEState::SCAN_STOPPING, "connectToServer guard");
        return false;
    }
    
    // Guard 3: Check exponential backoff
    unsigned long now = millis();
    if (consecutiveConnectFailures > 0 && static_cast<int32_t>(now - nextConnectAllowedMs) < 0) {
        {
            SemaphoreGuard lock(bleMutex, pdMS_TO_TICKS(20));  // COLD: backoff check
            shouldConnect = false;
        }
        setBLEState(BLEState::BACKOFF, "backoff active");
        return false;
    }
    
    // Set connection guard; CONNECTING state will initiate one async attempt
    // per loop() pass to avoid monopolizing a single iteration.
    connectInProgress = true;
    connectStartMs = millis();
    connectAttemptNumber = 0;  // Reset for new connection sequence
    asyncConnectPending = false;
    asyncConnectSuccess = false;
    connectPhaseStartUs = micros();  // Start timing connect phase
    setBLEState(BLEState::CONNECTING, "connectToServer");
    return true;
}

bool V1BLEClient::startAsyncConnect() {
    std::string addrStr = targetAddress.toString();
    connectAttemptNumber++;
    
    BLE_SM_LOGF("[BLE] Async connect attempt %d/%d to %s\n", 
                connectAttemptNumber, MAX_CONNECT_ATTEMPTS, addrStr.c_str());
    
    // Clear any stale bonding info (quick operation)
    if (NimBLEDevice::isBonded(targetAddress)) {
        NimBLEDevice::deleteBond(targetAddress);
        // No delay - deleteBond is quick
    }
    
    // CRITICAL: Stop proxy advertising - this competes with client connect!
    if (proxyEnabled && NimBLEDevice::getAdvertising()->isAdvertising()) {
        BLE_SM_LOGF("[BLE] Stopping proxy advertising before connect\n");
        NimBLEDevice::stopAdvertising();
        perfRecordProxyAdvertisingTransition(
            false,
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopBeforeV1Connect),
            millis());
        // No delay - stopAdvertising is quick, radio will settle during connect
    }
    
    // Extra verify scan is stopped (should already be from SCAN_STOPPING state)
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        BLE_SM_LOGF("[BLE] WARNING: Scan still active, stopping\n");
        pScan->stop();
    }
    
    // DON'T delete/recreate client - causes heap corruption and callback issues
    // Create client only if it doesn't exist
    if (!pClient) {
        pClient = NimBLEDevice::createClient();
        if (!pClient) {
            Serial.println("[BLE] ERROR: Failed to create client");
            connectInProgress = false;
            connectStartMs = 0;
            setBLEState(BLEState::DISCONNECTED, "client creation failed");
            return false;
        }
        // Create client callbacks if not already created
        if (!pClientCallbacks) {
            pClientCallbacks.reset(new ClientCallbacks());
        }
        pClient->setClientCallbacks(pClientCallbacks.get());
    }
    
    // Connection parameters: 12-24 (15-30ms interval), balanced for stability
    pClient->setConnectionParams(NIMBLE_CONN_INTERVAL_MIN,
                                 NIMBLE_CONN_INTERVAL_MAX,
                                 NIMBLE_CONN_LATENCY,
                                 NIMBLE_CONN_SUPERVISION_TIMEOUT);
    // Preserve current active-connect timeout behavior.
    pClient->setConnectTimeout(NIMBLE_CONNECT_TIMEOUT_ACTIVE_MS);

    // Ensure client is disconnected before attempting
    if (pClient->isConnected()) {
        // Never block loop() waiting for disconnect. Stage a short retry delay.
        Serial.println("[BLE] Client thinks it's connected; staging reconnect retry");
        pClient->disconnect();
        nextConnectAllowedMs = millis() + 100;
        connectInProgress = false;
        connectStartMs = 0;
        setBLEState(BLEState::BACKOFF, "waiting stale disconnect");
        return false;
    }
    
    // Clear async state before initiating connect
    asyncConnectPending = true;
    asyncConnectSuccess = false;
    
    // Use ASYNCHRONOUS connect - returns immediately, callback will set asyncConnectSuccess
    // NimBLE 2.x: connect(address, deleteAttributes, asyncConnect, exchangeMTU)
    bool initiated = pClient->connect(targetAddress, true, true);
    
    if (!initiated) {
        int err = pClient->getLastError();
        Serial.printf("[BLE] Async connect initiation failed (error: %d)\n", err);
        BLE_SM_LOGF("[BLE] Async connect initiation failed (error: %d)\n", err);
        asyncConnectPending = false;
        
        // Check if we should retry
        if (connectAttemptNumber < MAX_CONNECT_ATTEMPTS) {
            // Retry next loop pass; don't spin multiple attempts in one process() call.
            return true;  // Keep state machine going
        }
        
        // All attempts exhausted
        consecutiveConnectFailures++;
        perfRecordBleConnectUs(micros() - connectPhaseStartUs);
        
        if (hitsV1BleHardResetThreshold(consecutiveConnectFailures)) {
            hardResetBLEClient();
            return false;
        }
        
        nextConnectAllowedMs = millis() + computeV1BleBackoffMs(consecutiveConnectFailures);
        
        connectInProgress = false;
        connectStartMs = 0;
        setBLEState(BLEState::BACKOFF, "connect initiation failed");
        return false;
    }
    
    // Async connect initiated - transition to CONNECTING_WAIT
    setBLEState(BLEState::CONNECTING_WAIT, "async connect initiated");
    return true;
}

// Called from CONNECTING_WAIT state when async connect succeeds
// Now handles just the post-connect setup before discovery
bool V1BLEClient::finishConnection() {
    // Success!
    consecutiveConnectFailures = 0;
    nextConnectAllowedMs = 0;
    
    // Record connect phase time
    perfRecordBleConnectUs(micros() - connectPhaseStartUs);
    PERF_INC(reconnects);  // Count successful (re)connections
    
    // Log the negotiated connection parameters (interval units = 1.25ms, timeout units = 10ms)
    logConnParams("post-connect");
    
    // Transition to discovery phase
    connectPhaseStartUs = micros();  // Reset timer for discovery phase
    discoveryTaskRunning.store(false);
    discoveryTaskDone.store(false);
    discoveryTaskResult.store(false);
    setBLEState(BLEState::DISCOVERING, "ready for discovery");
    return true;
}

// Process CONNECTING_WAIT state - polls for async connect completion
void V1BLEClient::processConnectingWait() {
    unsigned long now = millis();
    unsigned long elapsed = now - connectStartMs;
    
    // Check for async connect success (set by onConnect callback)
    if (asyncConnectSuccess) {
        BLE_SM_LOGF("[BLE] Async connect succeeded after %lu ms\n", elapsed);
        finishConnection();
        return;
    }
    
    // Check if still pending
    if (asyncConnectPending) {
        // Check for overall timeout
        if (elapsed > CONNECT_TIMEOUT_MS) {
            BLE_SM_LOGF("[BLE] Async connect timeout after %lu ms\n", elapsed);
            asyncConnectPending = false;
            
            // Try to abort the pending connect
            if (pClient) {
                pClient->disconnect();
            }
            
            consecutiveConnectFailures++;
            perfRecordBleConnectUs(micros() - connectPhaseStartUs);
            
            if (hitsV1BleHardResetThreshold(consecutiveConnectFailures)) {
                hardResetBLEClient();
                return;
            }
            
            nextConnectAllowedMs = millis() + computeV1BleBackoffMs(consecutiveConnectFailures);
            
            connectInProgress = false;
            connectStartMs = 0;
            setBLEState(BLEState::BACKOFF, "connect timeout");
        }
        return;  // Still waiting
    }
    
    // Async connect failed (asyncConnectPending cleared without asyncConnectSuccess)
    int err = pClient ? pClient->getLastError() : -1;
    Serial.printf("[BLE] Async connect attempt %d failed (error: %d)\n", connectAttemptNumber, err);
    BLE_SM_LOGF("[BLE] Async connect attempt %d failed (error: %d)\n", connectAttemptNumber, err);
    
    // Check if we should retry
    if (connectAttemptNumber < MAX_CONNECT_ATTEMPTS) {
        if (err == 13) {  // EBUSY - defer via backoff instead of blocking main loop
            nextConnectAllowedMs = millis() + 150;  // 150ms non-blocking deferral
            connectInProgress = false;
            connectStartMs = 0;
            setBLEState(BLEState::BACKOFF, "EBUSY retry");
            return;
        }
        
        // Retry on the next loop iteration to keep each process() slice bounded.
        nextConnectAllowedMs = millis() + 20;
        setBLEState(BLEState::CONNECTING, "async connect retry");
        return;
    }
    
    // All attempts exhausted
    consecutiveConnectFailures++;
    perfRecordBleConnectUs(micros() - connectPhaseStartUs);
    
    if (hitsV1BleHardResetThreshold(consecutiveConnectFailures)) {
        hardResetBLEClient();
        return;
    }
    
    nextConnectAllowedMs = millis() + computeV1BleBackoffMs(consecutiveConnectFailures);
    
    connectInProgress = false;
    connectStartMs = 0;
    setBLEState(BLEState::BACKOFF, "all connect attempts failed");
}

// ---- discovery --------------------------------------------------------

// Static trampoline for async discovery task
void V1BLEClient::discoveryTaskFunc(void* param) {
    V1BLEClient* self = static_cast<V1BLEClient*>(param);
    bool result = self->pClient->discoverAttributes();
    self->discoveryTaskResult.store(result);
    self->discoveryTaskDone.store(true);
    self->discoveryTaskRunning.store(false);
    // Self-delete: IDF recommends external deletion, but this fire-and-forget
    // task has no external owner.  Deferred cleanup (prvTaskDeleteWithCapsTask)
    // handles the stack free safely.
    vTaskDeleteWithCaps(nullptr);
}

// Process DISCOVERING state - spawns discovery in a short-lived task
// so the main loop stays responsive during the ~2s GATT discovery
void V1BLEClient::processDiscovering() {
    unsigned long elapsed = millis() - connectStartMs;
    
    // Check for timeout
    // Safe even if discovery task is blocked: disconnect() sends HCI terminate,
    // NimBLE host task wakes the blocked task with BLE_HS_ENOTCONN, task exits cleanly
    if (elapsed > CONNECT_TIMEOUT_MS + DISCOVERY_TIMEOUT_MS) {
        Serial.println("[BLE] Discovery timeout");
        perfRecordBleDiscoveryUs(micros() - connectPhaseStartUs);
        disconnect();
        connectInProgress = false;
        connectStartMs = 0;
        setBLEState(BLEState::DISCONNECTED, "discovery timeout");
        return;
    }
    
    // Spawn discovery task on first entry
    // Guard: wait for any prior discovery task to finish (e.g. after timeout/disconnect)
    if (discoveryTaskRunning.load()) {
        return;  // Previous task still winding down — yield
    }
    if (!discoveryTaskDone.load()) {
        discoveryTaskRunning.store(true);
        BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
            discoveryTaskFunc, "disc", 4096, this, 1, nullptr, tskNO_AFFINITY,
            MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (rc != pdPASS) {
            // Never run discovery synchronously on the main loop; back off and retry.
            Serial.println("[BLE] disc task create failed - backing off");
            PERF_INC(bleDiscTaskCreateFail);
            discoveryTaskResult.store(false);
            discoveryTaskDone.store(true);
            discoveryTaskRunning.store(false);
            consecutiveConnectFailures++;
            if (hitsV1BleHardResetThreshold(consecutiveConnectFailures)) {
                hardResetBLEClient();
                return;
            }

            nextConnectAllowedMs = millis() + computeV1BleBackoffMs(consecutiveConnectFailures);

            connectInProgress = false;
            connectStartMs = 0;
            disconnect();
            setBLEState(BLEState::BACKOFF, "discovery task create failed");
        }
        return;  // Yield to loop while task runs
    }
    
    // Poll for completion — yield until task finishes
    if (!discoveryTaskDone.load()) {
        return;
    }
    
    perfRecordBleDiscoveryUs(micros() - connectPhaseStartUs);
    
    if (!discoveryTaskResult.load()) {
        Serial.println("[BLE] FAIL discovery");
        disconnect();
        connectInProgress = false;
        connectStartMs = 0;
        setBLEState(BLEState::DISCONNECTED, "discovery failed");
        return;
    }
    
    // Transition to subscribe phase (uses step machine to break up CCCD writes)
    connectPhaseStartUs = micros();  // Reset timer for subscribe phase
    subscribeStep = SubscribeStep::GET_SERVICE;
    subscribeStepStartUs = micros();
    setBLEState(BLEState::SUBSCRIBING, "discovery complete");
}

// ---- subscribe step machine -------------------------------------------

// Process SUBSCRIBING state - non-blocking step machine
// Each call executes one step then yields to allow loop() to run
void V1BLEClient::processSubscribing() {
    unsigned long elapsed = millis() - connectStartMs;
    
    // Check for overall timeout
    if (elapsed > CONNECT_TIMEOUT_MS + DISCOVERY_TIMEOUT_MS + SUBSCRIBE_TIMEOUT_MS) {
        Serial.println("[BLE] Subscribe timeout");
        perfRecordBleSubscribeUs(micros() - connectPhaseStartUs);
        disconnect();
        {
            SemaphoreGuard lock(bleMutex, pdMS_TO_TICKS(20));  // COLD: subscribe timeout
            shouldConnect = false;
            hasTargetDevice = false;
        }
        connectInProgress = false;
        connectStartMs = 0;
        setBLEState(BLEState::DISCONNECTED, "subscribe timeout");
        return;
    }
    
    // Execute one subscribe step
    subscribeStepStartUs = micros();
    bool done = executeSubscribeStep();
    uint32_t stepDuration = micros() - subscribeStepStartUs;
    
    // Record step timing for attribution
    if (perfExtended.bleSubscribeMaxUs < stepDuration) {
        perfExtended.bleSubscribeMaxUs = stepDuration;
    }
    
    if (done) {
        // All steps complete - success!
        {
            SemaphoreGuard lock(bleMutex, pdMS_TO_TICKS(20));  // COLD: subscribe complete
            connected = true;
        }
        connectedFollowupStep = ConnectedFollowupStep::REQUEST_ALERT_DATA;
        perfRecordBleSubscribeUs(micros() - connectPhaseStartUs);
        connectInProgress = false;
        connectStartMs = 0;
        setBLEState(BLEState::CONNECTED, "subscribe complete");
        Serial.println("[BLE] OK");
        return;
    }
    
    // Step completed but more to do - yield to loop()
    subscribeYieldUntilMs = millis() + SUBSCRIBE_YIELD_MS;
    setBLEState(BLEState::SUBSCRIBE_YIELD, "yield between steps");
}

// Process SUBSCRIBE_YIELD state - wait briefly then resume subscribing
void V1BLEClient::processSubscribeYield() {
    if (static_cast<int32_t>(millis() - subscribeYieldUntilMs) >= 0) {
        setBLEState(BLEState::SUBSCRIBING, "resuming subscribe");
    }
}

// Execute one subscribe step, return true when all steps complete
bool V1BLEClient::executeSubscribeStep() {
    switch (subscribeStep) {
        case SubscribeStep::GET_SERVICE: {
            pRemoteService = pClient->getService(V1_SERVICE_UUID);
            if (!pRemoteService) {
                Serial.println("[BLE] FAIL service");
                return false;  // Will trigger failure handling
            }
            subscribeStep = SubscribeStep::GET_DISPLAY_CHAR;
            return false;  // More steps to do
        }
        
        case SubscribeStep::GET_DISPLAY_CHAR: {
            pDisplayDataChar = pRemoteService->getCharacteristic(V1_DISPLAY_DATA_UUID);
            if (!pDisplayDataChar) {
                Serial.println("[BLE] FAIL display char");
                return false;
            }
            notifyShortChar.store(pDisplayDataChar, std::memory_order_relaxed);
            notifyShortCharId.store(shortUuid(pDisplayDataChar->getUUID()), std::memory_order_relaxed);
            subscribeStep = SubscribeStep::GET_COMMAND_CHAR;
            return false;
        }
        
        case SubscribeStep::GET_COMMAND_CHAR: {
            pCommandChar = pRemoteService->getCharacteristic(V1_COMMAND_WRITE_UUID);
            NimBLERemoteCharacteristic* altCommandChar = pRemoteService->getCharacteristic(V1_COMMAND_WRITE_ALT_UUID);
            
            // Prefer primary, fall back to alt if needed
            if (!pCommandChar || (!pCommandChar->canWrite() && !pCommandChar->canWriteNoResponse())) {
                if (altCommandChar && (altCommandChar->canWrite() || altCommandChar->canWriteNoResponse())) {
                    pCommandChar = altCommandChar;
                } else {
                    Serial.println("[BLE] FAIL command char");
                    return false;
                }
            }
            subscribeStep = SubscribeStep::GET_COMMAND_LONG;
            return false;
        }
        
        case SubscribeStep::GET_COMMAND_LONG: {
            pCommandCharLong = pRemoteService->getCharacteristic("92A0B8D2-9E05-11E2-AA59-F23C91AEC05E");
            // B8D2 is optional - don't log either way
            subscribeStep = SubscribeStep::SUBSCRIBE_DISPLAY;
            return false;
        }
        
        case SubscribeStep::SUBSCRIBE_DISPLAY: {
            bool subscribed = false;
            if (pDisplayDataChar->canNotify()) {
                subscribed = pDisplayDataChar->subscribe(true, notifyCallback, true);
            } else if (pDisplayDataChar->canIndicate()) {
                subscribed = pDisplayDataChar->subscribe(false, notifyCallback);
            }
            
            if (!subscribed) {
                Serial.println("[BLE] FAIL subscribe B2CE");
                return false;
            }
            subscribeStep = SubscribeStep::GET_DISPLAY_LONG;
            return false;
        }
        
        case SubscribeStep::WRITE_DISPLAY_CCCD: {
            NimBLERemoteDescriptor* cccd = pDisplayDataChar->getDescriptor(NimBLEUUID((uint16_t)0x2902));
            if (cccd) {
                uint8_t notifOn[] = {0x01, 0x00};
                if (!cccd->writeValue(notifOn, sizeof(notifOn), true)) {
                    Serial.println("[BLE] FAIL CCCD B2CE");
                    return false;
                }
            }
            subscribeStep = SubscribeStep::GET_DISPLAY_LONG;
            return false;
        }
        
        case SubscribeStep::GET_DISPLAY_LONG: {
            // Get B4E0 characteristic (non-critical, used for voltage passthrough)
            NimBLERemoteCharacteristic* pDisplayLong = pRemoteService->getCharacteristic(V1_DISPLAY_DATA_LONG_UUID);
            notifyLongChar.store(pDisplayLong, std::memory_order_relaxed);
            notifyLongCharId.store(pDisplayLong ? shortUuid(pDisplayLong->getUUID()) : 0,
                                   std::memory_order_relaxed);
            if (pDisplayLong && pDisplayLong->canNotify()) {
                subscribeStep = SubscribeStep::SUBSCRIBE_LONG;
            } else {
                subscribeStep = SubscribeStep::REQUEST_ALERT_DATA;  // Skip LONG subscribe
            }
            return false;
        }
        
        case SubscribeStep::SUBSCRIBE_LONG: {
            NimBLERemoteCharacteristic* pDisplayLong = pRemoteService->getCharacteristic(V1_DISPLAY_DATA_LONG_UUID);
            if (pDisplayLong && pDisplayLong->subscribe(true, notifyCallback, true)) {
                subscribeStep = SubscribeStep::REQUEST_ALERT_DATA;
            } else {
                subscribeStep = SubscribeStep::REQUEST_ALERT_DATA;
            }
            return false;
        }
        
        case SubscribeStep::WRITE_LONG_CCCD: {
            NimBLERemoteCharacteristic* pDisplayLong = pRemoteService->getCharacteristic(V1_DISPLAY_DATA_LONG_UUID);
            if (pDisplayLong) {
                NimBLERemoteDescriptor* cccdLong = pDisplayLong->getDescriptor(NimBLEUUID((uint16_t)0x2902));
                if (cccdLong) {
                    uint8_t notifOn[] = {0x01, 0x00};
                    cccdLong->writeValue(notifOn, sizeof(notifOn), true);
                }
            }
            subscribeStep = SubscribeStep::REQUEST_ALERT_DATA;
            return false;
        }
        
        case SubscribeStep::REQUEST_ALERT_DATA: {
            subscribeStep = SubscribeStep::REQUEST_VERSION;
            return false;
        }
        
        case SubscribeStep::REQUEST_VERSION: {
            subscribeStep = SubscribeStep::COMPLETE;
            return true;  // All done!
        }
        
        case SubscribeStep::COMPLETE:
            return true;  // Already complete
    }
    
    return true;  // Shouldn't reach here
}

// ---- helpers ----------------------------------------------------------

void V1BLEClient::logConnParams(const char* tag) {
    if (!pClient) {
        return;
    }

    NimBLEConnInfo info = pClient->getConnInfo();
    float intervalMs = info.getConnInterval() * 1.25f;

    Serial.printf("[BLE] Conn params (%s): interval=%.2f ms latency=%u\\n",
                  tag ? tag : "n/a",
                  intervalMs,
                  info.getConnLatency());
}

// ---- notify callback --------------------------------------------------

// NOTE: setupCharacteristics() has been replaced by the step machine (executeSubscribeStep)
void V1BLEClient::notifyCallback(NimBLERemoteCharacteristic* pChar, 
                                  uint8_t* pData, 
                                  size_t length, 
                                  bool isNotify) {
    if (!pData || !instancePtr || !pChar) {
        return;
    }
    
    uint16_t charId = 0;
    NimBLERemoteCharacteristic* shortChar = instancePtr->notifyShortChar.load(std::memory_order_relaxed);
    NimBLERemoteCharacteristic* longChar = instancePtr->notifyLongChar.load(std::memory_order_relaxed);
    if (pChar == shortChar) {
        charId = instancePtr->notifyShortCharId.load(std::memory_order_relaxed);
    } else if (pChar == longChar) {
        charId = instancePtr->notifyLongCharId.load(std::memory_order_relaxed);
    }
    if (charId == 0) {
        charId = shortUuid(pChar->getUUID());
    }
    
    if (charId == 0) {
        charId = 0xB2CE; // sensible fallback
    }

    // Check if this is a response packet that should go to B4E0
    // V1 sends responses on B2CE but some apps expect certain responses on B4E0
    // Testing: Keep voltage (0x63) on B2CE since Kenny's code receives it there
    uint16_t routeCharId = charId;
    if (charId == 0xB2CE && length >= 5) {
        uint8_t packetId = pData[3];  // Packet ID is at offset 3 in V1 protocol
        // Route ONLY sweep/alert response packets to B4E0
        // Keep voltage (0x63), version (0x01), serial (0x03) on B2CE
        if (packetId == 0x41 ||  // respAlertData
            packetId == 0x42) {  // respSweepSection
            routeCharId = 0xB4E0;
        }
    }

    // PERFORMANCE: Forward to proxy IMMEDIATELY - zero latency path to app
    // NimBLE handles thread safety for server notifications
    instancePtr->forwardToProxyImmediate(pData, length, routeCharId);
    
    // Call user callback for display processing (queued to main loop for SPI safety)
    if (instancePtr->dataCallback) {
        instancePtr->dataCallback(pData, length, charId);
    }
}

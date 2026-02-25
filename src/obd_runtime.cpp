#include "obd_internals.h"
#include "modules/obd/obd_state_policy.h"

#include <NimBLEDevice.h>

#include <string>

bool OBDHandler::runStateMachine() {
    // Check for asynchronous disconnect signalled from the NimBLE callback.
    if (s_obdDisconnectPending.exchange(false, std::memory_order_acq_rel)) {
        if (state == OBDState::READY ||
            state == OBDState::POLLING ||
            state == OBDState::INITIALIZING ||
            state == OBDState::CONNECTING) {
            Serial.println("[OBD] Async disconnect detected - transitioning to DISCONNECTED");
            {
                ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
                if (lock.ok()) {
                    lastData.valid = false;
                }
            }
            pNUSService = nullptr;
            pRXChar = nullptr;
            pTXChar = nullptr;
            consecutivePollFailures = 0;
            state = OBDState::DISCONNECTED;
            lastPollMs = millis();
            return false;
        }
    }

    if (scanActive && (millis() - scanStartMs) > 35000) {
        onScanComplete();
    }

    switch (state) {
        case OBDState::IDLE:
        {
            // V1 must be connected and settled before OBD touches the radio.
            if (!isLinkReady()) {
                return false;
            }
            // Attempt auto-connect to a remembered device when idle.
            const uint32_t now = millis();
            OBDRememberedDevice target;
            const bool hasAutoConnectTarget = findAutoConnectTarget(target);
            if (ObdConnectPolicy::shouldIdleAutoConnect(
                    true,
                    now - lastAutoConnectAttemptMs,
                    AUTO_CONNECT_RETRY_MS,
                    hasAutoConnectTarget,
                    autoConnectSuppressedAdapterOff)) {
                lastAutoConnectAttemptMs = now;
                targetAddress = NimBLEAddress(std::string(target.address.c_str()), BLE_ADDR_PUBLIC);
                targetDeviceName = target.name.length() ? target.name : target.address;
                targetPin = target.pin;
                targetIsObdLink = isObdLinkName(std::string(targetDeviceName.c_str()));
                hasTargetDevice = true;
                rememberTargetOnConnect = true;
                targetAutoConnect = true;
                connectionFailures = 0;
                state = OBDState::CONNECTING;
                Serial.printf("[OBD] Auto-connect queued: %s (%s)\n",
                              targetDeviceName.c_str(), target.address.c_str());
            }
            return false;
        }
        case OBDState::SCANNING:
        case OBDState::FAILED:
            return false;

        case OBDState::CONNECTING:
            handleConnecting();
            return false;

        case OBDState::INITIALIZING:
            handleInitializing();
            return false;

        case OBDState::READY:
            state = OBDState::POLLING;
            lastPollMs = millis();
            return false;

        case OBDState::POLLING:
            handlePolling();
            return lastData.valid;

        case OBDState::DISCONNECTED:
        {
            const uint32_t elapsedSinceDisconnectMs = millis() - lastPollMs;
            const ObdStatePolicy::DisconnectedDecision decision =
                ObdStatePolicy::evaluateDisconnected(
                    hasTargetDevice,
                    connectionFailures,
                    MAX_CONNECTION_FAILURES,
                    reconnectCycleCount,
                    elapsedSinceDisconnectMs,
                    BASE_RETRY_DELAY_MS,
                    MAX_RETRY_DELAY_MS,
                    MAX_RECONNECT_COOLDOWN_MS);

            if (decision.transitionToIdle) {
                reconnectCycleCount = decision.nextReconnectCycleCount;
                Serial.printf("[OBD] Reconnect cooldown elapsed (%lus) - returning to IDLE (cycle %u)\n",
                              (unsigned long)(decision.waitThresholdMs / 1000),
                              (unsigned)reconnectCycleCount);
                if (decision.resetConnectionFailures) {
                    connectionFailures = 0;
                }
                if (decision.clearTargetDevice) {
                    hasTargetDevice = false;
                }
                state = OBDState::IDLE;
                return false;
            }

            if (decision.transitionToConnecting) {
                state = OBDState::CONNECTING;
            }
            return false;
        }
    }

    return false;
}

void OBDHandler::handleConnecting() {
    if (!hasTargetDevice) {
        state = OBDState::FAILED;
        return;
    }

    // Abort if V1 dropped while we were queued — protect V1 priority.
    if (!isLinkReady()) {
        Serial.println("[OBD] Connect deferred: V1 not connected");
        state = OBDState::IDLE;
        return;
    }

    Serial.printf("[OBD] Connecting to %s...\n", targetDeviceName.c_str());

    // ── Scan-gate (auto-connect only) ──────────────────────────────
    // Scan for the CX up to SCAN_GATE_ATTEMPTS times with short pauses.
    // If advertising is found, proceed to connect.  If not, suppress
    // immediately — much lighter on the radio than cycling through full
    // connect/disconnect state-machine retries with exponential backoff.
    bool skipPreScan = false;
    if (targetAutoConnect && targetIsObdLink) {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        bool cxFound = false;
        if (pScan) {
            for (uint8_t i = 0; i < SCAN_GATE_ATTEMPTS; i++) {
                // Protect V1 priority — abort if link dropped mid-gate.
                if (!isLinkReady()) {
                    Serial.println("[OBD] Scan-gate aborted: V1 disconnected");
                    state = OBDState::IDLE;
                    return;
                }

                Serial.printf("[OBD] Scan-gate (%u/%u)...\n",
                              (unsigned)(i + 1), (unsigned)SCAN_GATE_ATTEMPTS);

                pScan->setDuplicateFilter(true);
                pScan->setMaxResults(20);
                pScan->setActiveScan(true);
                NimBLEScanResults results =
                    pScan->getResults(SCAN_GATE_DURATION_MS, false);

                for (int j = 0; j < results.getCount(); j++) {
                    const NimBLEAdvertisedDevice* dev = results.getDevice(j);
                    if (dev && isObdLinkName(dev->getName())) {
                        cxFound = true;
                        break;
                    }
                }

                pScan->setMaxResults(0);
                pScan->setActiveScan(true);
                pScan->clearResults();

                if (cxFound) {
                    Serial.println("[OBD] Scan-gate: OBDLink CX advertising, proceeding to connect");
                    break;
                }

                // Brief pause between scans to let other BLE events drain.
                if (i < SCAN_GATE_ATTEMPTS - 1) {
                    vTaskDelay(pdMS_TO_TICKS(SCAN_GATE_PAUSE_MS));
                }
            }
        }

        if (!cxFound) {
            Serial.println("[OBD] Auto-connect suppressed: OBDLink CX not advertising");
            autoConnectSuppressedAdapterOff = true;
            hasTargetDevice = false;
            connectionFailures = 0;
            reconnectCycleCount = 0;
            consecutivePollFailures = 0;
            state = OBDState::IDLE;
            return;
        }

        skipPreScan = true; // Scan-gate already confirmed advertising.
    }

    if (connectToDevice(skipPreScan) && discoverServices()) {
        // Log negotiated connection parameters for diagnostics.
        if (pOBDClient && pOBDClient->isConnected()) {
            NimBLEConnInfo ci = pOBDClient->getConnInfo();
            Serial.printf("[OBD] Negotiated conn params: interval=%.1fms latency=%u timeout=%ums\n",
                          ci.getConnInterval() * 1.25f,
                          (unsigned)ci.getConnLatency(),
                          (unsigned)(ci.getConnTimeout() * 10));
        }
        connectionFailures = 0;
        reconnectCycleCount = 0;
        state = OBDState::INITIALIZING;
        return;
    }

    connectionFailures++;
    Serial.printf("[OBD] Connect failed (%u/%u)\n",
                  (unsigned)connectionFailures,
                  (unsigned)MAX_CONNECTION_FAILURES);

    // For manual connects the pre-scan inside connectToDevice() still
    // tracks lastConnectFailureNoAdvertising — honour it here.
    if (!targetAutoConnect &&
        lastConnectFailureNoAdvertising &&
        connectionFailures >= MAX_CONNECTION_FAILURES) {
        Serial.println("[OBD] Auto-connect suppressed: adapter not advertising after max attempts");
        autoConnectSuppressedAdapterOff = true;
        disconnect();
        hasTargetDevice = false;
        connectionFailures = 0;
        reconnectCycleCount = 0;
        consecutivePollFailures = 0;
        state = OBDState::IDLE;
        return;
    }

    disconnect();
    state = OBDState::DISCONNECTED;
    lastPollMs = millis();
}

void OBDHandler::handleInitializing() {
    if (initializeAdapter()) {
        Serial.println("[OBD] Adapter initialized");
        connectionFailures = 0;
        reconnectCycleCount = 0;
        consecutivePollFailures = 0;
        state = OBDState::READY;

        if (rememberTargetOnConnect) {
            const String addr = String(targetAddress.toString().c_str());
            upsertRemembered(addr, targetDeviceName, targetPin, targetAutoConnect, millis());
            saveRememberedDevices();
        }
        return;
    }

    connectionFailures++;
    Serial.printf("[OBD] Init failed (%u/%u)\n",
                  (unsigned)connectionFailures,
                  (unsigned)MAX_CONNECTION_FAILURES);
    disconnect();
    state = OBDState::DISCONNECTED;
    lastPollMs = millis();
}

void OBDHandler::handlePolling() {
    if (!pOBDClient || !pOBDClient->isConnected()) {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (lock.ok()) {
            lastData.valid = false;
        }
        consecutivePollFailures = 0;
        state = OBDState::DISCONNECTED;
        lastPollMs = millis();
        return;
    }

    if ((millis() - lastPollMs) < POLL_INTERVAL_MS) {
        return;
    }
    lastPollMs = millis();

    static uint32_t pollCounter = 0;
    pollCounter++;

    // Keep speed as the primary metric at 1 Hz.
    // Track consecutive failures to detect adapter/ECU going offline
    // and proactively disconnect before the BLE supervision timeout.
    if (requestSpeed()) {
        consecutivePollFailures = 0;
    } else {
        consecutivePollFailures++;
        if (consecutivePollFailures >= MAX_CONSECUTIVE_POLL_FAILURES) {
            Serial.printf("[OBD] %u consecutive poll failures - proactive disconnect\n",
                          (unsigned)consecutivePollFailures);
            ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
            if (lock.ok()) {
                lastData.valid = false;
            }
            consecutivePollFailures = 0;
            disconnect();
            state = OBDState::DISCONNECTED;
            lastPollMs = millis();
            return;
        }
    }

    // Secondary metrics are polled at lower rates to avoid BLE/UART saturation.
    if ((pollCounter % RPM_POLL_DIV) == 0) {
        requestRPM();
    }

    if ((pollCounter % IAT_POLL_DIV) == 0) {
        requestIntakeAirTemp();
    }

    if (vwDataEnabled.load() && (pollCounter % OIL_TEMP_POLL_DIV) == 0) {
        requestOilTemp();
    }

    if ((pollCounter % 30) == 0) {
        requestVoltage();
    }
}

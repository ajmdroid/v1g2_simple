// OBD-II Handler implementation

#include "obd_internals.h"
#include "storage_manager.h"
#include "modules/obd/obd_state_policy.h"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#include <algorithm>
#include <cstring>

// Definitions of shared globals declared in obd_internals.h
const char* const OBD_SD_BACKUP_PATH = "/v1simple_obd_devices.json";

OBDHandler obdHandler;

OBDHandler* s_obdInstance = nullptr;
std::atomic<uint32_t> s_activePinCode{1234};
std::atomic<bool> s_obdDisconnectPending{false};

OBDSecurityCallbacks obdSecurityCallbacks;

OBDHandler::OBDHandler()
    : targetAddress() {
    lastData.speed_kph = 0;
    lastData.speed_mph = 0;
    lastData.rpm = 0;
    lastData.voltage = 0;
    lastData.oil_temp_c = INT16_MIN;
    lastData.dsg_temp_c = -128;
    lastData.intake_air_temp_c = -128;
    lastData.valid = false;
    lastData.timestamp_ms = 0;

    foundDevices.reserve(12);
    rememberedDevices.reserve(MAX_REMEMBERED_DEVICES);

    s_obdInstance = this;
}

OBDHandler::~OBDHandler() {
    stopTask();

    if (pOBDClient) {
        if (pOBDClient->isConnected()) {
            pOBDClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        NimBLEDevice::deleteClient(pOBDClient);
        pOBDClient = nullptr;
    }

    if (notifyStream) {
        vStreamBufferDelete(notifyStream);
        notifyStream = nullptr;
    }

    if (obdMutex) {
        vSemaphoreDelete(obdMutex);
        obdMutex = nullptr;
    }
}

void OBDHandler::setLinkReadyCallback(BoolCallback cb) {
    isLinkReadyCb = std::move(cb);
}

void OBDHandler::setStartScanCallback(VoidCallback cb) {
    startScanCb = std::move(cb);
}

void OBDHandler::setVwDataEnabled(bool enabled) {
    const bool previous = vwDataEnabled.exchange(enabled);
    if (previous == enabled) {
        return;
    }

    if (!enabled) {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (lock.ok()) {
            lastData.oil_temp_c = INT16_MIN;
            lastData.dsg_temp_c = -128;
        }
    }

    Serial.printf("[OBD] VW data polling %s\n", enabled ? "enabled" : "disabled");
}

bool OBDHandler::isLinkReady() const {
    return isLinkReadyCb ? isLinkReadyCb() : false;
}

void OBDHandler::requestScanStart() const {
    if (startScanCb) {
        startScanCb();
    }
}

void OBDHandler::begin() {
    if (!obdMutex) {
        obdMutex = xSemaphoreCreateMutex();
    }

    if (!notifyStream) {
        notifyStream = xStreamBufferCreate(RESPONSE_BUFFER_SIZE, 1);
    }

    // Ensure namespace exists to avoid repeated NOT_FOUND reads on clean NVS.
    Preferences p;
    if (p.begin("obd_store", false)) {
        p.end();
    } else {
        Serial.println("[OBD] WARN: Failed to initialize obd_store namespace");
    }

    loadRememberedDevices();

    state = OBDState::IDLE;
    scanActive = false;
    hasTargetDevice = false;
    rememberTargetOnConnect = false;
    targetAutoConnect = false;
    lastAutoConnectAttemptMs = 0;
    connectionFailures = 0;

    startTask();

    // Diagnostic: log remembered devices at boot
    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (lock.ok()) {
            Serial.printf("[OBD] Handler ready (remembered=%u)\n", (unsigned)rememberedDevices.size());
            for (size_t i = 0; i < rememberedDevices.size(); ++i) {
                Serial.printf("[OBD]   [%u] %s (%s) autoConnect=%s\n",
                              (unsigned)i,
                              rememberedDevices[i].name.c_str(),
                              rememberedDevices[i].address.c_str(),
                              rememberedDevices[i].autoConnect ? "yes" : "no");
            }
        }
    }
}

bool OBDHandler::update() {
    if (taskRunning.load(std::memory_order_relaxed)) {
        return hasValidData();
    }
    return runStateMachine();
}

void OBDHandler::tryAutoConnect() {
    if (state == OBDState::CONNECTING ||
        state == OBDState::INITIALIZING ||
        state == OBDState::READY ||
        state == OBDState::POLLING ||
        state == OBDState::SCANNING) {
        return;
    }

    // V1 must be connected before OBD touches the BLE radio.
    if (!isLinkReady()) {
        return;
    }

    if (autoConnectSuppressedAdapterOff) {
        Serial.println("[OBD] tryAutoConnect: re-arming auto-connect after adapter-off suppression");
        autoConnectSuppressedAdapterOff = false;
        lastAutoConnectAttemptMs = 0;
    }

    // If we're in DISCONNECTED with active cooldown/backoff, reset it.
    // This path is triggered by V1 connecting (car turned on), which is
    // a strong signal that the OBD adapter should be reachable now.
    if (state == OBDState::DISCONNECTED) {
        Serial.println("[OBD] tryAutoConnect: resetting DISCONNECTED cooldown (V1 trigger)");
        connectionFailures = 0;
        reconnectCycleCount = 0;
        consecutivePollFailures = 0;
        state = OBDState::IDLE;
    }

    const uint32_t now = millis();
    if (now - lastAutoConnectAttemptMs < AUTO_CONNECT_RETRY_MS) {
        return;
    }

    OBDRememberedDevice target;
    if (!findAutoConnectTarget(target)) {
        return;
    }

    targetAddress = NimBLEAddress(std::string(target.address.c_str()), BLE_ADDR_PUBLIC);
    targetDeviceName = target.name.length() ? target.name : target.address;
    targetPin = target.pin;
    targetIsObdLink = isObdLinkName(std::string(targetDeviceName.c_str()));
    hasTargetDevice = true;
    rememberTargetOnConnect = true;
    targetAutoConnect = true;

    connectionFailures = 0;
    state = OBDState::CONNECTING;
    lastAutoConnectAttemptMs = now;

    Serial.printf("[OBD] Auto-connect queued: %s (%s)\n",
                  targetDeviceName.c_str(),
                  target.address.c_str());
}

const char* OBDHandler::getStateString() const {
    switch (state) {
        case OBDState::IDLE: return "IDLE";
        case OBDState::SCANNING: return "SCANNING";
        case OBDState::CONNECTING: return "CONNECTING";
        case OBDState::INITIALIZING: return "INITIALIZING";
        case OBDState::READY: return "READY";
        case OBDState::POLLING: return "POLLING";
        case OBDState::DISCONNECTED: return "DISCONNECTED";
        case OBDState::FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

String OBDHandler::getConnectedDeviceAddress() const {
    ObdLock lock(obdMutex, 0);
    if (!lock.ok() || !hasTargetDevice) {
        return "";
    }
    return String(targetAddress.toString().c_str());
}

std::vector<OBDDeviceInfo> OBDHandler::getFoundDevices() const {
    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return {};
    }
    return foundDevices;
}

void OBDHandler::clearFoundDevices() {
    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return;
    }
    foundDevices.clear();
}

void OBDHandler::startScan() {
    if (!isLinkReady()) {
        Serial.println("[OBD] Scan blocked: connect V1 first");
        return;
    }

    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return;
        }
        foundDevices.clear();
        scanActive = true;
        scanStartMs = millis();
        if (!isConnected()) {
            state = OBDState::SCANNING;
        }
    }

    Serial.println("[OBD] Manual scan started");
    requestScanStart();
}

void OBDHandler::stopScan() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
    }
    onScanComplete();
}

void OBDHandler::onScanComplete() {
    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return;
    }
    if (!scanActive) {
        return;
    }

    scanActive = false;
    if (state == OBDState::SCANNING) {
        state = OBDState::IDLE;
    }

    Serial.printf("[OBD] Scan complete (%u devices)\n", (unsigned)foundDevices.size());
}

void OBDHandler::onDeviceFoundDeferred(const char* name, const char* addr, int rssi) {
    if (!name || !addr || name[0] == '\0' || addr[0] == '\0') {
        return;
    }
    if (strlen(name) < 2) {
        return;
    }
    if (!isObdLinkName(std::string(name))) {
        return;
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok() || !scanActive) {
        return;
    }

    String addrStr(addr);
    for (const auto& d : foundDevices) {
        if (d.address.equalsIgnoreCase(addrStr)) {
            return;
        }
    }

    OBDDeviceInfo info;
    info.address = addrStr;
    info.name = String(name);
    info.rssi = rssi;
    foundDevices.push_back(info);

    Serial.printf("[OBD] Found BLE device #%u: '%s' [%s] RSSI:%d\n",
                  (unsigned)foundDevices.size(),
                  info.name.c_str(),
                  info.address.c_str(),
                  info.rssi);
}

bool OBDHandler::connectToAddress(const String& address,
                                  const String& name,
                                  const String& pin,
                                  bool remember,
                                  bool autoConnect) {
    if (address.length() == 0) {
        return false;
    }
    const bool addrIsNull = isNullAddressString(address);

    if (!obdMutex) {
        obdMutex = xSemaphoreCreateMutex();
    }

    bool alreadyActiveForTarget = false;
    String effectiveName = name.length() ? name : address;
    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return false;
        }

        if (hasTargetDevice) {
            const String currentAddr = String(targetAddress.toString().c_str());
            const bool sameTarget = !addrIsNull && currentAddr.equalsIgnoreCase(address);
            const bool activeState =
                (state == OBDState::CONNECTING ||
                 state == OBDState::INITIALIZING ||
                 state == OBDState::READY ||
                 state == OBDState::POLLING);
            if (sameTarget && activeState) {
                alreadyActiveForTarget = true;
                if (name.length()) {
                    targetDeviceName = name;
                    effectiveName = name;
                } else {
                    effectiveName = targetDeviceName.length() ? targetDeviceName : address;
                }
                if (pin.length()) {
                    targetPin = pin;
                }
                rememberTargetOnConnect = rememberTargetOnConnect || remember;
                targetAutoConnect = autoConnect;
            }
        }
    }

    if (alreadyActiveForTarget) {
        if (remember && !addrIsNull) {
            upsertRemembered(address, effectiveName, pin, autoConnect, millis());
            saveRememberedDevices();
        }
        Serial.printf("[OBD] Connect ignored: already active for %s (%s)\n",
                      effectiveName.c_str(),
                      address.c_str());
        return true;
    }

    // Connection attempts are unreliable while active scanning is running.
    // Ensure scan is fully stopped before transitioning to CONNECTING.
    stopScan();

    disconnect();

    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return false;
        }

        targetAddress = NimBLEAddress(std::string(address.c_str()), BLE_ADDR_PUBLIC);
        String adapterName = name;
        if (adapterName.length() == 0) {
            for (const auto& d : foundDevices) {
                if (d.address.equalsIgnoreCase(address) && d.name.length() > 0) {
                    adapterName = d.name;
                    break;
                }
            }
        }
        if (adapterName.length() == 0) {
            for (const auto& d : rememberedDevices) {
                if (d.address.equalsIgnoreCase(address) && d.name.length() > 0) {
                    adapterName = d.name;
                    break;
                }
            }
        }
        targetDeviceName = adapterName.length() ? adapterName : address;
        targetPin = pin;
        // Allow address-only connects from API clients; when a name is provided,
        // enforce CX-only support at queue time.
        targetIsObdLink = adapterName.length() == 0 ||
                          isObdLinkName(std::string(adapterName.c_str()));
        if (!targetIsObdLink) {
            Serial.printf("[OBD] Unsupported adapter '%s' - only OBDLink CX is supported\n",
                          targetDeviceName.c_str());
            return false;
        }
        hasTargetDevice = true;
        rememberTargetOnConnect = remember;
        targetAutoConnect = autoConnect;
        scanActive = false;
        connectionFailures = 0;
        reconnectCycleCount = 0;
        autoConnectSuppressedAdapterOff = false;
        lastConnectFailureNoAdvertising = false;
        state = OBDState::CONNECTING;
    }

    if (remember && !addrIsNull) {
        upsertRemembered(address, targetDeviceName, pin, autoConnect, millis());
        saveRememberedDevices();
    }

    if (addrIsNull) {
        Serial.printf("[OBD] Connect queued with NULL address for '%s' - will resolve via scan\n",
                      targetDeviceName.c_str());
    }
    Serial.printf("[OBD] Connect queued: %s (%s)\n", targetDeviceName.c_str(), address.c_str());
    return true;
}

void OBDHandler::disconnect() {
    if (pOBDClient && pOBDClient->isConnected()) {
        pOBDClient->disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return;
        }

        pNUSService = nullptr;
        pRXChar = nullptr;
        pTXChar = nullptr;
        lastData.valid = false;

        if (state != OBDState::SCANNING) {
            state = OBDState::DISCONNECTED;
        }
    }
}

// --- Persistence methods (forgetRemembered, setRememberedAutoConnect,
//     parseDevicesJson, loadRememberedDevices, saveRememberedDevices,
//     upsertRemembered, findAutoConnectTarget) moved to obd_persistence.cpp ---

std::vector<OBDRememberedDevice> OBDHandler::getRememberedDevices() const {
    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return {};
    }
    return rememberedDevices;
}

OBDData OBDHandler::getData() const {
    ObdLock lock(obdMutex, pdMS_TO_TICKS(5));
    if (!lock.ok()) {
        OBDData snapshot{};
        snapshot.oil_temp_c = INT16_MIN;
        snapshot.dsg_temp_c = -128;
        snapshot.intake_air_temp_c = -128;
        snapshot.valid = false;
        snapshot.timestamp_ms = 0;
        return snapshot;
    }
    return lastData;
}

OBDPerfSnapshot OBDHandler::getPerfSnapshot() const {
    OBDPerfSnapshot snapshot;
    snapshot.notifyDrops = notifyDropCount.load(std::memory_order_relaxed);

    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return snapshot;
    }

    snapshot.state = static_cast<uint8_t>(state);
    snapshot.connected = (state == OBDState::READY || state == OBDState::POLLING) ? 1 : 0;
    snapshot.scanActive = scanActive ? 1 : 0;
    snapshot.connectionFailures = connectionFailures;
    snapshot.consecutivePollFailures = consecutivePollFailures;

    const uint32_t nowMs = millis();
    const uint32_t ageMs =
        (lastData.timestamp_ms > 0 && nowMs >= lastData.timestamp_ms)
            ? (nowMs - lastData.timestamp_ms)
            : UINT32_MAX;
    snapshot.sampleAgeMs = ageMs;

    const bool hasFreshData = lastData.valid && ageMs <= 3000;
    snapshot.hasValidData = hasFreshData ? 1 : 0;
    if (lastData.valid) {
        snapshot.speedMphX10 = static_cast<int32_t>(lastData.speed_mph * 10.0f);
    }

    return snapshot;
}

bool OBDHandler::hasValidData() const {
    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return false;
    }
    const uint32_t age = millis() - lastData.timestamp_ms;
    return lastData.valid && age <= 3000;
}

bool OBDHandler::isDataStale(uint32_t maxAgeMs) const {
    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return true;
    }
    return (millis() - lastData.timestamp_ms) > maxAgeMs;
}

float OBDHandler::getSpeedKph() const {
    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return lastData.speed_kph;
    }
    return lastData.speed_kph;
}

float OBDHandler::getSpeedMph() const {
    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return lastData.speed_mph;
    }
    return lastData.speed_mph;
}

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

// --- Connection methods (connectToDevice, connectedPeerLooksLikeObd, discoverServices,
//     initializeAdapter, resolveTargetAddress, connectViaAdvertisedDevice,
//     notificationCallback, taskEntry, startTask, stopTask) moved to
//     obd_connection.cpp ---

// --- Protocol methods (sendATCommand, requestSpeed/RPM/Voltage/IntakeAirTemp/OilTemp,
//     parsers, isObdLinkName, isNullAddressString, normalizePin) moved to
//     obd_protocol.cpp ---

// --- Persistence methods (parseDevicesJson, loadRememberedDevices, saveRememberedDevices,
//     upsertRemembered, findAutoConnectTarget) moved to obd_persistence.cpp ---

// OBD-II Handler implementation

#include "obd_handler.h"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#include <algorithm>
#include <cstring>

OBDHandler obdHandler;

static OBDHandler* s_obdInstance = nullptr;
static volatile uint32_t s_activePinCode = 1234;

static bool isAllZeroAddress(const NimBLEAddress& address) {
    const uint8_t* raw = address.getVal();
    if (!raw) {
        return true;
    }
    for (size_t i = 0; i < 6; i++) {
        if (raw[i] != 0) {
            return false;
        }
    }
    return true;
}

class ObdLock {
public:
    explicit ObdLock(SemaphoreHandle_t mutex, TickType_t timeout = 0)
        : mutex_(mutex), locked_(false) {
        if (mutex_) {
            locked_ = (xSemaphoreTake(mutex_, timeout) == pdTRUE);
        }
    }

    ~ObdLock() {
        if (mutex_ && locked_) {
            xSemaphoreGive(mutex_);
        }
    }

    bool ok() const { return locked_; }

private:
    SemaphoreHandle_t mutex_;
    bool locked_;
};

class OBDSecurityCallbacks : public NimBLEClientCallbacks {
public:
    void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
        NimBLEDevice::injectPassKey(connInfo, s_activePinCode);
    }

    void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t passkey) override {
        (void)passkey;
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }

    void onDisconnect(NimBLEClient* pClient, int reason) override {
        (void)pClient;
        (void)reason;
    }
};

static OBDSecurityCallbacks obdSecurityCallbacks;

OBDHandler::OBDHandler()
    : targetAddress() {
    lastData.speed_kph = 0;
    lastData.speed_mph = 0;
    lastData.rpm = 0;
    lastData.voltage = 0;
    lastData.oil_temp_c = -128;
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
            lastData.oil_temp_c = -128;
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
    Serial.printf("[OBD] Handler ready (remembered=%u)\n", (unsigned)rememberedDevices.size());
}

bool OBDHandler::update() {
    if (taskRunning) {
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

bool OBDHandler::forgetRemembered(const String& address) {
    bool removed = false;
    bool wasTarget = false;

    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return false;
        }

        auto it = std::remove_if(rememberedDevices.begin(), rememberedDevices.end(),
                                 [&](const OBDRememberedDevice& d) {
                                     return d.address.equalsIgnoreCase(address);
                                 });
        if (it != rememberedDevices.end()) {
            rememberedDevices.erase(it, rememberedDevices.end());
            removed = true;
        }

        if (hasTargetDevice && String(targetAddress.toString().c_str()).equalsIgnoreCase(address)) {
            wasTarget = true;
            hasTargetDevice = false;
            targetDeviceName = "";
            targetPin = "";
            rememberTargetOnConnect = false;
            targetAutoConnect = false;
        }
    }

    if (removed) {
        saveRememberedDevices();
    }
    if (wasTarget) {
        disconnect();
    }
    return removed;
}

std::vector<OBDRememberedDevice> OBDHandler::getRememberedDevices() const {
    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return {};
    }
    return rememberedDevices;
}

bool OBDHandler::setRememberedAutoConnect(const String& address, bool enabled) {
    bool changed = false;

    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return false;
        }

        for (auto& d : rememberedDevices) {
            if (d.address.equalsIgnoreCase(address)) {
                if (d.autoConnect != enabled) {
                    d.autoConnect = enabled;
                    changed = true;
                }
                break;
            }
        }
    }

    if (changed) {
        saveRememberedDevices();
    }
    return changed;
}

OBDData OBDHandler::getData() const {
    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return lastData;
    }
    return lastData;
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
    if (scanActive && (millis() - scanStartMs) > 35000) {
        onScanComplete();
    }

    switch (state) {
        case OBDState::IDLE:
        {
            // Attempt auto-connect to a remembered device when idle.
            const uint32_t now = millis();
            if (now - lastAutoConnectAttemptMs >= AUTO_CONNECT_RETRY_MS) {
                lastAutoConnectAttemptMs = now;
                OBDRememberedDevice target;
                if (findAutoConnectTarget(target)) {
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
            if (!hasTargetDevice) {
                return false;
            }
            if (connectionFailures >= MAX_CONNECTION_FAILURES) {
                // After a cooldown, return to IDLE so auto-connect can
                // discover the adapter again.
                if ((millis() - lastPollMs) >= 60000) {
                    connectionFailures = 0;
                    hasTargetDevice = false;
                    state = OBDState::IDLE;
                }
                return false;
            }

            {
                uint32_t retryDelay = BASE_RETRY_DELAY_MS * (1u << connectionFailures);
                if (retryDelay > MAX_RETRY_DELAY_MS) {
                    retryDelay = MAX_RETRY_DELAY_MS;
                }
                if ((millis() - lastPollMs) >= retryDelay) {
                    state = OBDState::CONNECTING;
                }
            }
            return false;
    }

    return false;
}

void OBDHandler::handleConnecting() {
    if (!hasTargetDevice) {
        state = OBDState::FAILED;
        return;
    }

    Serial.printf("[OBD] Connecting to %s...\n", targetDeviceName.c_str());

    if (connectToDevice() && discoverServices()) {
        connectionFailures = 0;
        state = OBDState::INITIALIZING;
        return;
    }

    connectionFailures++;
    Serial.printf("[OBD] Connect failed (%u/%u)\n",
                  (unsigned)connectionFailures,
                  (unsigned)MAX_CONNECTION_FAILURES);
    disconnect();
    state = OBDState::DISCONNECTED;
    lastPollMs = millis();
}

void OBDHandler::handleInitializing() {
    if (initializeAdapter()) {
        Serial.println("[OBD] Adapter initialized");
        connectionFailures = 0;
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
    requestSpeed();

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

bool OBDHandler::connectToDevice() {
    // BLE controllers generally cannot sustain scan+connect reliably.
    // Guard here as well for auto-connect and retry paths.
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        Serial.println("[OBD] Stopping active scan before connect");
        pScan->stop();
        pScan->clearResults();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (pOBDClient && pOBDClient->isConnected()) {
        pOBDClient->disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!pOBDClient) {
        pOBDClient = NimBLEDevice::createClient();
        if (!pOBDClient) {
            return false;
        }
        pOBDClient->setClientCallbacks(&obdSecurityCallbacks);
        pOBDClient->setConnectionParams(12, 12, 0, 500);
        // NimBLE timeout is configured on the client, not via connect() args.
        pOBDClient->setConnectTimeout(10000);
    }

    if (!targetIsObdLink) {
        Serial.printf("[OBD] Connect aborted: unsupported adapter '%s' (CX only)\n",
                      targetDeviceName.c_str());
        return false;
    }

    if (targetAddress.isNull() || isAllZeroAddress(targetAddress)) {
        const int bondCount = NimBLEDevice::getNumBonds();
        if (bondCount > 0) {
            Serial.printf("[OBD] Target address unknown - trying %d bonded peer(s) first\n", bondCount);
            s_activePinCode = 0;
            NimBLEDevice::setSecurityAuth(false, false, false);
            NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

            for (int bi = 0; bi < bondCount; bi++) {
                NimBLEAddress bondedAddr = NimBLEDevice::getBondedAddress(bi);
                if (bondedAddr.isNull() || isAllZeroAddress(bondedAddr)) {
                    continue;
                }

                const std::string bondedStr = bondedAddr.toString();
                const uint8_t bondedPrimaryType = bondedAddr.getType();
                uint8_t bondedTypes[4] = {0};
                size_t bondedTypeCount = 0;
                auto pushBondedType = [&](uint8_t t) {
                    for (size_t k = 0; k < bondedTypeCount; k++) {
                        if (bondedTypes[k] == t) return;
                    }
                    if (bondedTypeCount < (sizeof(bondedTypes) / sizeof(bondedTypes[0]))) {
                        bondedTypes[bondedTypeCount++] = t;
                    }
                };
                pushBondedType(bondedPrimaryType);
                pushBondedType(BLE_ADDR_PUBLIC);
                pushBondedType(BLE_ADDR_RANDOM);
                pushBondedType(BLE_ADDR_PUBLIC_ID);
                pushBondedType(BLE_ADDR_RANDOM_ID);

                for (size_t bj = 0; bj < bondedTypeCount; bj++) {
                    NimBLEAddress candidate(bondedStr, bondedTypes[bj]);
                    Serial.printf("[OBD] Connect try (bonded-list#%d, addr=%s, addrType=%u)\n",
                                  bi,
                                  bondedStr.c_str(),
                                  (unsigned)bondedTypes[bj]);

                    if (!pOBDClient->connect(candidate, false, false, true)) {
                        Serial.printf("[OBD] Connect attempt failed (bonded-list#%d, addrType=%u, err=%d)\n",
                                      bi,
                                      (unsigned)bondedTypes[bj],
                                      pOBDClient->getLastError());
                        continue;
                    }

                    vTaskDelay(pdMS_TO_TICKS(250));
                    if (pOBDClient->isConnected()) {
                        if (!connectedPeerLooksLikeObd()) {
                            Serial.printf("[OBD] Bonded peer rejected (missing OBD UART): %s\n",
                                          bondedStr.c_str());
                            pOBDClient->disconnect();
                            vTaskDelay(pdMS_TO_TICKS(100));
                            continue;
                        }
                        targetAddress = candidate;
                        Serial.printf("[OBD] Bonded reconnect selected address: %s\n",
                                      targetAddress.toString().c_str());
                        return true;
                    }

                    pOBDClient->disconnect();
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        }

        Serial.printf("[OBD] Target address is NULL for '%s' - resolving...\n",
                      targetDeviceName.c_str());
        if (!resolveTargetAddress()) {
            Serial.println("[OBD] Address resolve failed - cannot connect");
            return false;
        }
    }

    const std::string targetAddrStr = targetAddress.toString();
    const uint8_t primaryAddrType = targetAddress.getType();

    struct SecurityProfile {
        bool bonding;
        bool mitm;
        bool sc;
        uint8_t ioCap;
        const char* label;
    };

    // Some adapters (or bonded identities) may resolve with ID address types.
    // Try a small set of valid NimBLE peer types in deterministic order.
    uint8_t addrTypes[4] = {0};
    size_t addrTypeCount = 0;
    auto pushAddrType = [&](uint8_t t) {
        for (size_t k = 0; k < addrTypeCount; k++) {
            if (addrTypes[k] == t) return;
        }
        if (addrTypeCount < (sizeof(addrTypes) / sizeof(addrTypes[0]))) {
            addrTypes[addrTypeCount++] = t;
        }
    };

    pushAddrType(primaryAddrType);
    pushAddrType(BLE_ADDR_PUBLIC);
    pushAddrType(BLE_ADDR_RANDOM);
    pushAddrType(BLE_ADDR_PUBLIC_ID);
    pushAddrType(BLE_ADDR_RANDOM_ID);

    auto tryProfile = [&](const SecurityProfile& profile, bool usingPin) {
        NimBLEDevice::setSecurityAuth(profile.bonding, profile.mitm, profile.sc);
        NimBLEDevice::setSecurityIOCap(profile.ioCap);

        if (connectViaAdvertisedDevice(profile.label, usingPin)) {
            return true;
        }

        for (size_t j = 0; j < addrTypeCount; j++) {
            const uint8_t addrType = addrTypes[j];
            NimBLEAddress candidate(targetAddrStr, addrType);

            if (usingPin) {
                Serial.printf("[OBD] Connect try (pin=%u, %s, addrType=%u)\n",
                              (unsigned)s_activePinCode,
                              profile.label,
                              (unsigned)addrType);
            } else {
                Serial.printf("[OBD] Connect try (%s, addrType=%u)\n",
                              profile.label,
                              (unsigned)addrType);
            }

            // NimBLE signature: connect(address, deleteAttributes, asyncConnect, exchangeMTU)
            if (!pOBDClient->connect(candidate, false, false, true)) {
                if (usingPin) {
                    Serial.printf("[OBD] Connect attempt failed (pin=%u, %s, addrType=%u, err=%d)\n",
                                  (unsigned)s_activePinCode,
                                  profile.label,
                                  (unsigned)addrType,
                                  pOBDClient->getLastError());
                } else {
                    Serial.printf("[OBD] Connect attempt failed (%s, addrType=%u, err=%d)\n",
                                  profile.label,
                                  (unsigned)addrType,
                                  pOBDClient->getLastError());
                }
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(250));
            if (pOBDClient->isConnected()) {
                if (!connectedPeerLooksLikeObd()) {
                    Serial.printf("[OBD] Connected peer rejected (%s, addrType=%u): missing OBD UART\n",
                                  profile.label,
                                  (unsigned)addrType);
                    pOBDClient->disconnect();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
                targetAddress = candidate;
                return true;
            }

            pOBDClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return false;
    };

    const bool userPinProvided = targetPin.length() > 0;

    // Phase 1: bonded reconnect only (no pairing).
    bool isBonded = NimBLEDevice::isBonded(targetAddress);
    if (!isBonded) {
        for (size_t j = 0; j < addrTypeCount; j++) {
            if (NimBLEDevice::isBonded(NimBLEAddress(targetAddrStr, addrTypes[j]))) {
                isBonded = true;
                break;
            }
        }
    }

    if (isBonded) {
        Serial.println("[OBD] Connect phase: bonded reconnect");
        s_activePinCode = 0;
        const SecurityProfile bondedProfile{false, false, false, BLE_HS_IO_NO_INPUT_OUTPUT, "bonded-reconnect"};
        if (tryProfile(bondedProfile, false)) {
            return true;
        }

        bool deletedBond = false;
        if (NimBLEDevice::isBonded(targetAddress)) {
            NimBLEDevice::deleteBond(targetAddress);
            deletedBond = true;
        }
        for (size_t j = 0; j < addrTypeCount; j++) {
            NimBLEAddress candidate(targetAddrStr, addrTypes[j]);
            if (NimBLEDevice::isBonded(candidate)) {
                NimBLEDevice::deleteBond(candidate);
                deletedBond = true;
            }
        }
        if (deletedBond) {
            Serial.println("[OBD] Bonded reconnect failed - cleared stale bond, moving to first-pair phase");
            vTaskDelay(pdMS_TO_TICKS(80));
        } else {
            Serial.println("[OBD] Bonded reconnect failed - moving to first-pair phase");
        }
    } else {
        Serial.println("[OBD] Connect phase: first pair/bond");
    }

    // Phase 2: first pairing + bond establishment.
    uint32_t pinCandidates[2] = {0};
    size_t pinCount = 0;
    if (userPinProvided) {
        pinCandidates[pinCount++] = normalizePin(targetPin, targetIsObdLink);
    } else {
        // CX typically pairs with 123456 on first bond; keep no-pin fallback.
        pinCandidates[pinCount++] = 123456u;
        pinCandidates[pinCount++] = 0u;
    }

    for (size_t pinIndex = 0; pinIndex < pinCount; pinIndex++) {
        s_activePinCode = pinCandidates[pinIndex];
        const bool usingPin = s_activePinCode != 0;

        SecurityProfile pairProfiles[2];
        size_t profileCount = 0;
        if (usingPin) {
            pairProfiles[profileCount++] = SecurityProfile{true, true, true, BLE_HS_IO_DISPLAY_YESNO, "sc-pin"};
            pairProfiles[profileCount++] = SecurityProfile{true, true, false, BLE_HS_IO_KEYBOARD_ONLY, "legacy-pin"};
        } else {
            pairProfiles[profileCount++] = SecurityProfile{true, false, true, BLE_HS_IO_NO_INPUT_OUTPUT, "sc-no-pin"};
            pairProfiles[profileCount++] = SecurityProfile{true, false, false, BLE_HS_IO_NO_INPUT_OUTPUT, "legacy-no-pin"};
        }

        for (size_t i = 0; i < profileCount; i++) {
            if (tryProfile(pairProfiles[i], usingPin)) {
                return true;
            }
        }

        if (userPinProvided) {
            break;
        }
    }

    return false;
}

bool OBDHandler::connectedPeerLooksLikeObd() {
    if (!pOBDClient || !pOBDClient->isConnected()) {
        return false;
    }

    // GATT table may not be populated yet after a bonded reconnect.
    // discoverAttributes() is idempotent and returns cached results
    // if already discovered.
    pOBDClient->discoverAttributes();

    NimBLERemoteService* service = pOBDClient->getService(NUS_SERVICE_UUID);
    if (!service) {
        return false;
    }

    NimBLERemoteCharacteristic* tx = service->getCharacteristic(NUS_TX_CHAR_UUID);
    NimBLERemoteCharacteristic* rx = service->getCharacteristic(NUS_RX_CHAR_UUID);
    return tx != nullptr && rx != nullptr;
}

bool OBDHandler::discoverServices() {
    if (!pOBDClient || !pOBDClient->isConnected()) {
        return false;
    }

    pOBDClient->discoverAttributes();

    pNUSService = pOBDClient->getService(NUS_SERVICE_UUID);
    if (!pNUSService) {
        pNUSService = pOBDClient->getService("FFF0");
    }
    if (!pNUSService) {
        pNUSService = pOBDClient->getService("FFE0");
    }
    if (!pNUSService) {
        return false;
    }

    pTXChar = pNUSService->getCharacteristic(NUS_TX_CHAR_UUID);
    pRXChar = pNUSService->getCharacteristic(NUS_RX_CHAR_UUID);

    if (!pTXChar || !pRXChar) {
        // Alternative UART characteristics
        pTXChar = pNUSService->getCharacteristic("FFF1");
        if (!pTXChar) {
            pTXChar = pNUSService->getCharacteristic("FFE1");
        }
        pRXChar = pNUSService->getCharacteristic("FFF2");
        if (!pRXChar) {
            pRXChar = pNUSService->getCharacteristic("FFE1");
        }
    }

    if (!pTXChar || !pRXChar) {
        return false;
    }

    if (!pTXChar->canNotify()) {
        return false;
    }

    return pTXChar->subscribe(true, notificationCallback);
}

bool OBDHandler::initializeAdapter() {
    String response;

    auto probeMode01 = [&](const char* label, uint32_t timeoutMs, int attempts) {
        for (int i = 0; i < attempts; i++) {
            if (sendATCommand("0100", response, timeoutMs)) {
                String normalized(response);
                normalized.toUpperCase();
                normalized.replace(" ", "");
                if (normalized.indexOf("4100") >= 0) {
                    Serial.printf("[OBD] Vehicle bus probe OK (%s): %s\n", label, response.c_str());
                    return true;
                }
                Serial.printf("[OBD] Vehicle bus probe inconclusive (%s): %s\n", label, response.c_str());
            }
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        Serial.printf("[OBD] Vehicle bus probe failed (%s)\n", label);
        return false;
    };

    if (!sendATCommand("ATZ", response, 3000)) return false;
    if (!sendATCommand("ATE0", response)) return false;
    if (!sendATCommand("ATL0", response)) return false;
    if (!sendATCommand("ATS0", response)) return false;
    if (!sendATCommand("ATH0", response)) return false;
    if (!sendATCommand("ATSP0", response, 5000)) return false;
    // Adaptive timing helps on slower ECUs and during ignition transitions.
    (void)sendATCommand("ATAT1", response, 500);

    // On some vehicles the first Mode 01 query after protocol select can take
    // several seconds while the adapter locks bus timing.
    if (probeMode01("auto", 5000, 3)) {
        return true;
    }

    // Fallbacks for CAN vehicles when auto-detect fails to lock quickly.
    // Always probe after selecting a protocol, even if the set command response
    // is odd/inconclusive on this adapter firmware.
    (void)sendATCommand("ATSP6", response, 2000);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (probeMode01("can11-500", 5000, 2)) {
        return true;
    }
    (void)sendATCommand("ATSP7", response, 2000);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (probeMode01("can29-500", 5000, 2)) {
        return true;
    }
    (void)sendATCommand("ATSP8", response, 2000);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (probeMode01("can11-250", 5000, 2)) {
        return true;
    }
    (void)sendATCommand("ATSP9", response, 2000);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (probeMode01("can29-250", 5000, 2)) {
        return true;
    }

    Serial.println("[OBD] Adapter init failed: no ECU response on supported CAN protocols");
    return false;
}

bool OBDHandler::sendATCommand(const char* cmd, String& response, uint32_t timeoutMs) {
    auto logCommandFailure = [&](const char* reason, const String* resp = nullptr) {
        static uint32_t lastFailLogMs = 0;
        static uint32_t suppressed = 0;
        const uint32_t now = millis();
        if ((now - lastFailLogMs) < 2000) {
            suppressed++;
            return;
        }

        if (suppressed > 0) {
            Serial.printf("[OBD] AT failure logs suppressed: %lu\n", (unsigned long)suppressed);
            suppressed = 0;
        }

        if (resp) {
            Serial.printf("[OBD] AT '%s' failed: %s, resp='%s'\n",
                          cmd ? cmd : "",
                          reason ? reason : "unknown",
                          resp->c_str());
        } else {
            Serial.printf("[OBD] AT '%s' failed: %s\n",
                          cmd ? cmd : "",
                          reason ? reason : "unknown");
        }
        lastFailLogMs = now;
    };

    if (!pRXChar || !pOBDClient || !pOBDClient->isConnected()) {
        logCommandFailure("link not ready");
        return false;
    }

    // Reset response state and flush any stale data from the stream buffer.
    // No mutex needed here — responseBuffer is only touched by this function
    // (running on the OBD task) now that the notification callback writes
    // exclusively to the stream buffer.
    responseLength = 0;
    responseBuffer[0] = '\0';
    responseComplete.store(false, std::memory_order_release);
    if (notifyStream) {
        xStreamBufferReset(notifyStream);
    }

    String cmdLine = String(cmd) + "\r";
    if (!pRXChar->writeValue((uint8_t*)cmdLine.c_str(), cmdLine.length(), false)) {
        logCommandFailure("writeValue failed");
        return false;
    }

    // Drain the stream buffer until the ELM327 prompt '>' is seen or we
    // time out.  The notification callback writes raw bytes into the stream
    // buffer lock-free, so there is zero mutex contention on this path.
    const uint32_t startMs = millis();
    uint8_t chunk[64];
    while (!responseComplete.load(std::memory_order_acquire)) {
        if ((millis() - startMs) >= timeoutMs) {
            break;
        }

        size_t received = 0;
        if (notifyStream) {
            received = xStreamBufferReceive(notifyStream, chunk, sizeof(chunk),
                                            pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        for (size_t i = 0; i < received; i++) {
            const char c = static_cast<char>(chunk[i]);
            if (c == '>') {
                responseComplete.store(true, std::memory_order_release);
                break;
            }
            if (c != '\r' && c != '\n' && responseLength < RESPONSE_BUFFER_SIZE) {
                responseBuffer[responseLength++] = c;
                responseBuffer[responseLength] = '\0';
            }
        }
    }

    response = responseBuffer;
    if (!responseComplete.load(std::memory_order_acquire)) {
        logCommandFailure("timeout waiting for prompt");
        return false;
    }

    String normalized(response);
    normalized.toUpperCase();
    String compact(normalized);
    compact.replace(" ", "");
    const bool hasError =
        normalized.indexOf("ERROR") >= 0 ||
        normalized.indexOf("UNABLE") >= 0 ||
        normalized.indexOf("NO DATA") >= 0 ||
        normalized.indexOf("STOPPED") >= 0 ||
        normalized.indexOf("BUS BUSY") >= 0 ||
        normalized.indexOf("NO RESPONSE") >= 0 ||
        compact == "?";
    if (hasError) {
        logCommandFailure("error response", &response);
        return false;
    }

    return true;
}

bool OBDHandler::requestSpeed() {
    String response;
    if (!sendATCommand("010D", response, 1000)) {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (lock.ok()) {
            lastData.valid = false;
        }
        return false;
    }

    uint8_t speedKph = 0;
    if (!parseSpeedResponse(response, speedKph)) {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (lock.ok()) {
            lastData.valid = false;
        }
        return false;
    }

    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return false;
    }

    lastData.speed_kph = speedKph;
    lastData.speed_mph = speedKph * 0.621371f;
    lastData.timestamp_ms = millis();
    lastData.valid = true;
    return true;
}

bool OBDHandler::requestRPM() {
    String response;
    if (!sendATCommand("010C", response, 250)) {
        return false;
    }

    uint16_t rpm = 0;
    if (!parseRPMResponse(response, rpm)) {
        return false;
    }

    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return false;
    }
    lastData.rpm = rpm;
    lastData.timestamp_ms = millis();
    return true;
}

bool OBDHandler::requestVoltage() {
    String response;
    if (!sendATCommand("ATRV", response, 250)) {
        return false;
    }

    float voltage = 0.0f;
    if (!parseVoltageResponse(response, voltage)) {
        return false;
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return false;
    }
    lastData.voltage = voltage;
    lastData.timestamp_ms = millis();
    return true;
}

bool OBDHandler::requestIntakeAirTemp() {
    String response;
    if (!sendATCommand("010F", response, 500)) {
        return false;
    }

    int8_t tempC = -128;
    if (!parseIntakeAirTempResponse(response, tempC)) {
        return false;
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return false;
    }
    lastData.intake_air_temp_c = tempC;
    lastData.timestamp_ms = millis();
    return true;
}

bool OBDHandler::requestOilTemp() {
    String response;
    if (!sendATCommand("ATSH7E0", response, 500)) {
        return false;
    }

    bool parsed = false;
    int8_t tempC = -128;

    if (sendATCommand("22F40C", response, 500)) {
        parsed = parseVwMode22TempResponse(response, "F40C", tempC);
    }

    // Always restore default functional header after VW-specific request.
    String restoreResponse;
    (void)sendATCommand("ATSH7DF", restoreResponse, 200);

    if (!parsed) {
        return false;
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return false;
    }
    lastData.oil_temp_c = tempC;
    lastData.timestamp_ms = millis();
    return true;
}

bool OBDHandler::isValidHexString(const String& str, size_t expectedLen) {
    if (str.length() == 0) return false;
    if (expectedLen > 0 && str.length() != expectedLen) return false;

    for (size_t i = 0; i < str.length(); i++) {
        const char c = str[i];
        const bool isHex = (c >= '0' && c <= '9') ||
                           (c >= 'A' && c <= 'F') ||
                           (c >= 'a' && c <= 'f');
        if (!isHex) {
            return false;
        }
    }
    return true;
}

bool OBDHandler::parseSpeedResponse(const String& response, uint8_t& speedKph) {
    String normalized(response);
    normalized.toUpperCase();
    normalized.replace(" ", "");

    int idx = normalized.indexOf("410D");
    if (idx < 0) return false;

    const String hexVal = normalized.substring(idx + 4, idx + 6);
    if (!isValidHexString(hexVal, 2)) return false;

    speedKph = static_cast<uint8_t>(strtoul(hexVal.c_str(), nullptr, 16));
    return true;
}

bool OBDHandler::parseRPMResponse(const String& response, uint16_t& rpm) {
    String normalized(response);
    normalized.toUpperCase();
    normalized.replace(" ", "");

    int idx = normalized.indexOf("410C");
    if (idx < 0) return false;

    const String hexA = normalized.substring(idx + 4, idx + 6);
    const String hexB = normalized.substring(idx + 6, idx + 8);

    if (!isValidHexString(hexA, 2) || !isValidHexString(hexB, 2)) {
        return false;
    }

    const uint8_t a = static_cast<uint8_t>(strtoul(hexA.c_str(), nullptr, 16));
    const uint8_t b = static_cast<uint8_t>(strtoul(hexB.c_str(), nullptr, 16));
    rpm = ((uint16_t)a * 256 + b) / 4;
    return true;
}

bool OBDHandler::parseVoltageResponse(const String& response, float& voltage) {
    voltage = response.toFloat();
    return voltage > 0.0f && voltage < 20.0f;
}

bool OBDHandler::parseIntakeAirTempResponse(const String& response, int8_t& tempC) {
    String normalized(response);
    normalized.toUpperCase();
    normalized.replace(" ", "");

    int idx = normalized.indexOf("410F");
    if (idx < 0) return false;

    const String hexVal = normalized.substring(idx + 4, idx + 6);
    if (!isValidHexString(hexVal, 2)) return false;

    const uint8_t raw = static_cast<uint8_t>(strtoul(hexVal.c_str(), nullptr, 16));
    tempC = static_cast<int8_t>(raw - 40);
    return true;
}

bool OBDHandler::parseVwMode22TempResponse(const String& response, const char* pidEcho, int8_t& tempC) {
    if (!pidEcho || pidEcho[0] == '\0') {
        return false;
    }

    String normalized(response);
    normalized.toUpperCase();

    String pattern = String("62") + String(pidEcho);
    pattern.toUpperCase();

    const int idx = normalized.indexOf(pattern);
    if (idx < 0) {
        return false;
    }

    const int dataStart = idx + pattern.length();
    if (dataStart + 2 > normalized.length()) {
        return false;
    }

    const String hexVal = normalized.substring(dataStart, dataStart + 2);
    if (!isValidHexString(hexVal, 2)) {
        return false;
    }

    const uint8_t raw = static_cast<uint8_t>(strtoul(hexVal.c_str(), nullptr, 16));
    if (raw == 0) {
        return false;
    }

    tempC = static_cast<int8_t>(raw - 40);
    return true;
}

bool OBDHandler::isObdLinkName(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    String upper(name.c_str());
    upper.toUpperCase();
    const bool isObdLinkBrand = (upper.indexOf("OBDLINK") >= 0) ||
                                (upper.indexOf("OBD LINK") >= 0) ||
                                (upper.indexOf("OBD-LINK") >= 0);
    const bool isCxModel = upper.indexOf("CX") >= 0;
    return isObdLinkBrand && isCxModel;
}

bool OBDHandler::isNullAddressString(const String& address) {
    if (address.length() == 0) {
        return true;
    }
    const NimBLEAddress parsed(std::string(address.c_str()), BLE_ADDR_PUBLIC);
    return parsed.isNull() || isAllZeroAddress(parsed);
}

uint32_t OBDHandler::normalizePin(const String& pin, bool obdLinkDefault) {
    String digits;
    digits.reserve(6);
    for (size_t i = 0; i < pin.length() && digits.length() < 6; i++) {
        const char c = pin[i];
        if (c >= '0' && c <= '9') {
            digits += c;
        }
    }

    if (digits.length() == 0) {
        return obdLinkDefault ? 123456u : 1234u;
    }

    uint32_t value = static_cast<uint32_t>(strtoul(digits.c_str(), nullptr, 10));
    if (value > 999999u) {
        value = 999999u;
    }
    return value;
}

bool OBDHandler::resolveTargetAddress() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan) {
        return false;
    }

    if (pScan->isScanning()) {
        pScan->stop();
        pScan->clearResults();
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    auto tryResolvePass = [&](bool activeScan, uint32_t durationMs, const char* label) {
        pScan->setActiveScan(activeScan);
        NimBLEScanResults results = pScan->getResults(durationMs, false);

        const NimBLEAdvertisedDevice* bestNamedDevice = nullptr;
        int bestNamedRssi = -127;
        size_t nonNullCount = 0;
        bool sawNamedCx = false;
        bool sawNamedCxNullAddr = false;

        const int count = results.getCount();
        for (int i = 0; i < count; i++) {
            const NimBLEAdvertisedDevice* dev = results.getDevice(i);
            if (!dev) {
                continue;
            }
            const NimBLEAddress& advAddr = dev->getAddress();
            const bool addrIsZero = advAddr.isNull() || isAllZeroAddress(advAddr);
            const bool namedCx = isObdLinkName(dev->getName());
            if (namedCx) {
                sawNamedCx = true;
                if (addrIsZero) {
                    sawNamedCxNullAddr = true;
                }
            }

            if (addrIsZero) {
                continue;
            }

            nonNullCount++;
            const int rssi = dev->getRSSI();
            if (namedCx && (!bestNamedDevice || rssi > bestNamedRssi)) {
                bestNamedDevice = dev;
                bestNamedRssi = rssi;
            }
        }

        if (bestNamedDevice) {
            targetAddress = bestNamedDevice->getAddress();
            if (targetDeviceName.length() == 0) {
                targetDeviceName = String(bestNamedDevice->getName().c_str());
            }
            Serial.printf("[OBD] Resolved CX address via %s scan: %s RSSI:%d\n",
                          label,
                          targetAddress.toString().c_str(),
                          bestNamedRssi);
            return true;
        }

        if (sawNamedCxNullAddr && nonNullCount > 0) {
            Serial.printf("[OBD] Resolve pass (%s) saw CX name on null-address record; no confident non-null CX match\n",
                          label);
        } else {
            Serial.printf("[OBD] Resolve pass (%s) found no usable CX address\n", label);
        }
        return false;
    };

    pScan->clearResults();
    pScan->setDuplicateFilter(true);
    pScan->setMaxResults(20);

    bool resolved = tryResolvePass(false, 1800, "passive");
    if (!resolved) {
        resolved = tryResolvePass(true, 2200, "active");
    }

    // Restore normal scan configuration for V1/OBD callback-driven scans.
    pScan->setMaxResults(0);
    pScan->setActiveScan(true);
    pScan->clearResults();
    return resolved;
}

bool OBDHandler::connectViaAdvertisedDevice(const char* profileLabel, bool usingPin) {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan || !pOBDClient) {
        return false;
    }

    if (pScan->isScanning()) {
        pScan->stop();
        pScan->clearResults();
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    auto restoreScanConfig = [&]() {
        pScan->setMaxResults(0);
        pScan->setActiveScan(true);
        pScan->clearResults();
    };

    const String currentTargetAddr = String(targetAddress.toString().c_str());
    pScan->setDuplicateFilter(true);
    pScan->setMaxResults(20);
    pScan->setActiveScan(true);

    NimBLEScanResults results = pScan->getResults(1800, false);

    const NimBLEAdvertisedDevice* preferred = nullptr;
    int preferredRssi = -127;
    const NimBLEAdvertisedDevice* fallback = nullptr;
    int fallbackRssi = -127;

    const int count = results.getCount();
    for (int i = 0; i < count; i++) {
        const NimBLEAdvertisedDevice* dev = results.getDevice(i);
        if (!dev) {
            continue;
        }
        const NimBLEAddress& advAddr = dev->getAddress();
        if (advAddr.isNull() || isAllZeroAddress(advAddr)) {
            continue;
        }
        if (!isObdLinkName(dev->getName())) {
            continue;
        }

        const int rssi = dev->getRSSI();
        const String devAddr = String(dev->getAddress().toString().c_str());
        const bool matchesCurrentTarget =
            !targetAddress.isNull() && devAddr.equalsIgnoreCase(currentTargetAddr);

        if (matchesCurrentTarget) {
            if (!preferred || rssi > preferredRssi) {
                preferred = dev;
                preferredRssi = rssi;
            }
        } else if (!fallback || rssi > fallbackRssi) {
            fallback = dev;
            fallbackRssi = rssi;
        }
    }

    const NimBLEAdvertisedDevice* bestDevice = preferred ? preferred : fallback;
    const int bestRssi = preferred ? preferredRssi : fallbackRssi;
    if (!bestDevice) {
        Serial.printf("[OBD] Advertised connect skip (%s): no usable CX found\n",
                      profileLabel ? profileLabel : "n/a");
        restoreScanConfig();
        return false;
    }

    if (usingPin) {
        Serial.printf("[OBD] Connect try (pin=%u, %s, advertised, RSSI:%d, addr=%s)\n",
                      (unsigned)s_activePinCode,
                      profileLabel ? profileLabel : "n/a",
                      bestRssi,
                      bestDevice->getAddress().toString().c_str());
    } else {
        Serial.printf("[OBD] Connect try (%s, advertised, RSSI:%d, addr=%s)\n",
                      profileLabel ? profileLabel : "n/a",
                      bestRssi,
                      bestDevice->getAddress().toString().c_str());
    }

    if (!pOBDClient->connect(bestDevice, false, false, true)) {
        if (usingPin) {
            Serial.printf("[OBD] Connect attempt failed (pin=%u, %s, advertised, err=%d)\n",
                          (unsigned)s_activePinCode,
                          profileLabel ? profileLabel : "n/a",
                          pOBDClient->getLastError());
        } else {
            Serial.printf("[OBD] Connect attempt failed (%s, advertised, err=%d)\n",
                          profileLabel ? profileLabel : "n/a",
                          pOBDClient->getLastError());
        }
        restoreScanConfig();
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(250));
    if (pOBDClient->isConnected()) {
        if (!connectedPeerLooksLikeObd()) {
            Serial.printf("[OBD] Advertised peer rejected (%s): missing OBD UART\n",
                          profileLabel ? profileLabel : "n/a");
            pOBDClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
            restoreScanConfig();
            return false;
        }
        targetAddress = bestDevice->getAddress();
        restoreScanConfig();
        return true;
    }

    pOBDClient->disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    restoreScanConfig();
    return false;
}

void OBDHandler::loadRememberedDevices() {
    std::vector<OBDRememberedDevice> loaded;
    loaded.reserve(MAX_REMEMBERED_DEVICES);

    Preferences p;
    if (!p.begin("obd_store", true)) {
        return;
    }

    String blob;
    // Avoid Preferences NOT_FOUND noise on fresh NVS when key does not exist yet.
    if (p.isKey("devices")) {
        blob = p.getString("devices", "");
    }
    p.end();

    if (blob.length() == 0) {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (lock.ok()) {
            rememberedDevices.clear();
        }
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, blob) != DeserializationError::Ok) {
        return;
    }

    JsonArray arr = doc["devices"].as<JsonArray>();
    if (arr.isNull()) {
        return;
    }

    for (JsonObject item : arr) {
        if (loaded.size() >= MAX_REMEMBERED_DEVICES) {
            break;
        }

        const char* addr = item["address"] | "";
        if (!addr || addr[0] == '\0') {
            continue;
        }

        OBDRememberedDevice d;
        d.address = String(addr);
        if (isNullAddressString(d.address)) {
            continue;
        }
        d.name = String((const char*)(item["name"] | ""));
        d.pin = String((const char*)(item["pin"] | ""));
        d.autoConnect = item["autoConnect"] | false;
        d.lastSeenMs = item["lastSeenMs"] | 0;
        loaded.push_back(d);
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (lock.ok()) {
        rememberedDevices = std::move(loaded);
    }
}

void OBDHandler::saveRememberedDevices() {
    std::vector<OBDRememberedDevice> snapshot;
    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return;
        }
        snapshot = rememberedDevices;
    }

    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (const auto& d : snapshot) {
        JsonObject o = arr.add<JsonObject>();
        o["address"] = d.address;
        o["name"] = d.name;
        o["pin"] = d.pin;
        o["autoConnect"] = d.autoConnect;
        o["lastSeenMs"] = d.lastSeenMs;
    }

    String blob;
    serializeJson(doc, blob);

    Preferences p;
    if (p.begin("obd_store", false)) {
        p.putString("devices", blob);
        p.end();
    }
}

void OBDHandler::upsertRemembered(const String& address,
                                  const String& name,
                                  const String& pin,
                                  bool autoConnect,
                                  uint32_t lastSeenMs) {
    if (isNullAddressString(address)) {
        return;
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return;
    }

    for (auto& d : rememberedDevices) {
        if (d.address.equalsIgnoreCase(address)) {
            if (name.length()) d.name = name;
            d.pin = pin;
            d.autoConnect = autoConnect;
            d.lastSeenMs = lastSeenMs;
            return;
        }
    }

    OBDRememberedDevice d;
    d.address = address;
    d.name = name.length() ? name : address;
    d.pin = pin;
    d.autoConnect = autoConnect;
    d.lastSeenMs = lastSeenMs;

    rememberedDevices.insert(rememberedDevices.begin(), d);
    if (rememberedDevices.size() > MAX_REMEMBERED_DEVICES) {
        rememberedDevices.resize(MAX_REMEMBERED_DEVICES);
    }
}

bool OBDHandler::findAutoConnectTarget(OBDRememberedDevice& out) const {
    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return false;
    }

    for (const auto& d : rememberedDevices) {
        if (d.autoConnect && d.address.length() > 0 && !isNullAddressString(d.address)) {
            out = d;
            return true;
        }
    }
    return false;
}

void OBDHandler::notificationCallback(NimBLERemoteCharacteristic* pChar,
                                      uint8_t* pData,
                                      size_t length,
                                      bool isNotify) {
    (void)pChar;
    (void)isNotify;

    if (!s_obdInstance || !pData || length == 0) {
        return;
    }

    // Write raw bytes into the lock-free stream buffer so the OBD task can
    // drain them without any mutex contention.  Zero timeout keeps the BLE
    // host task non-blocking.
    if (s_obdInstance->notifyStream) {
        size_t sent = xStreamBufferSend(s_obdInstance->notifyStream,
                                        pData, length, 0);
        if (sent < length) {
            s_obdInstance->notifyDropCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void OBDHandler::taskEntry(void* param) {
    OBDHandler* self = static_cast<OBDHandler*>(param);
    if (!self) {
        vTaskDelete(nullptr);
        return;
    }

    while (!self->taskShouldExit) {
        self->runStateMachine();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    self->obdTaskHandle = nullptr;
    self->taskRunning = false;
    vTaskDelete(nullptr);
}

void OBDHandler::startTask() {
    if (obdTaskHandle) {
        taskRunning = true;
        return;
    }

    taskShouldExit = false;
    const BaseType_t res = xTaskCreatePinnedToCore(taskEntry,
                                                    "obdTask",
                                                    6144,
                                                    this,
                                                    1,
                                                    &obdTaskHandle,
                                                    tskNO_AFFINITY);
    taskRunning = (res == pdPASS);
}

void OBDHandler::stopTask() {
    if (!obdTaskHandle) {
        return;
    }

    taskShouldExit = true;
    const uint32_t startMs = millis();

    while (obdTaskHandle && (millis() - startMs) < 500) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (obdTaskHandle) {
        TaskHandle_t handle = obdTaskHandle;
        obdTaskHandle = nullptr;
        taskRunning = false;
        vTaskDelete(handle);
    }
}

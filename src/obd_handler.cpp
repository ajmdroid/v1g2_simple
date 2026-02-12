// OBD-II Handler implementation

#include "obd_handler.h"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#include <algorithm>
#include <cstring>

OBDHandler obdHandler;

static OBDHandler* s_obdInstance = nullptr;
static volatile uint32_t s_activePinCode = 1234;

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
    if (!isLinkReady()) {
        return;
    }

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
            const bool sameTarget = currentAddr.equalsIgnoreCase(address);
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
        if (remember) {
            upsertRemembered(address, effectiveName, pin, autoConnect, millis());
            saveRememberedDevices();
        }
        Serial.printf("[OBD] Connect ignored: already active for %s (%s)\n",
                      effectiveName.c_str(),
                      address.c_str());
        return true;
    }

    disconnect();

    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return false;
        }

        targetAddress = NimBLEAddress(std::string(address.c_str()), BLE_ADDR_PUBLIC);
        targetDeviceName = name.length() ? name : address;
        targetPin = pin;
        targetIsObdLink = isObdLinkName(std::string(targetDeviceName.c_str()));
        hasTargetDevice = true;
        rememberTargetOnConnect = remember;
        targetAutoConnect = autoConnect;
        scanActive = false;
        connectionFailures = 0;
        state = OBDState::CONNECTING;
    }

    if (remember) {
        upsertRemembered(address, targetDeviceName, pin, autoConnect, millis());
        saveRememberedDevices();
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
            if (!hasTargetDevice || !isLinkReady()) {
                return false;
            }
            if (connectionFailures >= MAX_CONNECTION_FAILURES) {
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
    if (!isLinkReady()) {
        state = OBDState::DISCONNECTED;
        lastPollMs = millis();
        return;
    }

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

    if ((pollCounter % OIL_TEMP_POLL_DIV) == 0) {
        requestOilTemp();
    }

    if ((pollCounter % 30) == 0) {
        requestVoltage();
    }
}

bool OBDHandler::connectToDevice() {
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

    s_activePinCode = normalizePin(targetPin, targetIsObdLink);

    const std::string targetAddrStr = targetAddress.toString();
    const uint8_t primaryAddrType = targetAddress.getType();

    struct SecurityProfile {
        bool mitm;
        bool sc;
        uint8_t ioCap;
        const char* label;
    };

    SecurityProfile profiles[2];
    size_t profileCount = 0;

    if (targetIsObdLink) {
        // OBDLink CX typically uses Secure Connections, but keep a legacy fallback.
        profiles[profileCount++] = SecurityProfile{true, true, BLE_HS_IO_DISPLAY_YESNO, "sc"};
        profiles[profileCount++] = SecurityProfile{true, false, BLE_HS_IO_KEYBOARD_ONLY, "legacy"};
    } else {
        profiles[profileCount++] = SecurityProfile{false, false, BLE_HS_IO_KEYBOARD_ONLY, "legacy"};
    }

    for (size_t i = 0; i < profileCount; i++) {
        NimBLEDevice::setSecurityAuth(true, profiles[i].mitm, profiles[i].sc);
        NimBLEDevice::setSecurityIOCap(profiles[i].ioCap);

        // Some adapters (or bonded identities) may resolve with ID address types.
        // Try a small set of valid NimBLE peer types in deterministic order.
        uint8_t addrTypes[4] = {0};
        size_t addrTypeCount = 0;
        auto pushAddrType = [&](uint8_t t) {
            for (size_t k = 0; k < addrTypeCount; k++) {
                if (addrTypes[k] == t) return;
            }
            addrTypes[addrTypeCount++] = t;
        };

        pushAddrType(primaryAddrType);
        pushAddrType(BLE_ADDR_PUBLIC);
        pushAddrType(BLE_ADDR_RANDOM);
        pushAddrType(BLE_ADDR_PUBLIC_ID);
        pushAddrType(BLE_ADDR_RANDOM_ID);

        for (size_t j = 0; j < addrTypeCount; j++) {
            const uint8_t addrType = addrTypes[j];
            NimBLEAddress candidate(targetAddrStr, addrType);

            Serial.printf("[OBD] Connect try (%s, addrType=%u)\n",
                          profiles[i].label,
                          (unsigned)addrType);

            // NimBLE signature: connect(address, deleteAttributes, asyncConnect, exchangeMTU)
            // Use blocking connect with configured timeout.
            if (!pOBDClient->connect(candidate, false, false, true)) {
                Serial.printf("[OBD] Connect attempt failed (%s, addrType=%u, err=%d)\n",
                              profiles[i].label,
                              (unsigned)addrType,
                              pOBDClient->getLastError());
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(250));
            if (pOBDClient->isConnected()) {
                // Preserve the working address type for subsequent reconnects.
                targetAddress = candidate;
                return true;
            }

            pOBDClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    return false;
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

    if (!sendATCommand("ATZ", response, 3000)) return false;
    if (!sendATCommand("ATE0", response)) return false;
    if (!sendATCommand("ATL0", response)) return false;
    if (!sendATCommand("ATS0", response)) return false;
    if (!sendATCommand("ATH0", response)) return false;
    if (!sendATCommand("ATSP0", response, 5000)) return false;

    // Best-effort probe (vehicle may be asleep)
    (void)sendATCommand("0100", response, 1000);
    return true;
}

bool OBDHandler::sendATCommand(const char* cmd, String& response, uint32_t timeoutMs) {
    if (!pRXChar || !pOBDClient || !pOBDClient->isConnected()) {
        return false;
    }

    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return false;
        }
        responseLength = 0;
        responseBuffer[0] = '\0';
        responseComplete = false;
    }

    String cmdLine = String(cmd) + "\r";
    if (!pRXChar->writeValue((uint8_t*)cmdLine.c_str(), cmdLine.length(), false)) {
        return false;
    }

    const uint32_t startMs = millis();
    while (true) {
        {
            ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
            if (lock.ok() && responseComplete) {
                break;
            }
        }

        if ((millis() - startMs) >= timeoutMs) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return false;
        }
        response = responseBuffer;
        if (!responseComplete) {
            return false;
        }
    }

    if (response.indexOf("ERROR") >= 0 ||
        response.indexOf("UNABLE") >= 0 ||
        response.indexOf("NO DATA") >= 0 ||
        response.indexOf("?") >= 0) {
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
    return upper.indexOf("OBDLINK") >= 0 ||
           upper.indexOf("OBD LINK") >= 0 ||
           upper.indexOf("OBD-LINK") >= 0;
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

void OBDHandler::loadRememberedDevices() {
    std::vector<OBDRememberedDevice> loaded;
    loaded.reserve(MAX_REMEMBERED_DEVICES);

    Preferences p;
    if (!p.begin("obd_store", true)) {
        return;
    }

    String blob = p.getString("devices", "");
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
        if (d.autoConnect && d.address.length() > 0) {
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

    ObdLock lock(s_obdInstance->obdMutex, 0);
    if (!lock.ok()) {
        s_obdInstance->notifyDropCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    for (size_t i = 0; i < length; i++) {
        const char c = static_cast<char>(pData[i]);

        if (c == '>') {
            s_obdInstance->responseComplete = true;
            return;
        }

        if (c != '\r' && c != '\n' && s_obdInstance->responseLength < RESPONSE_BUFFER_SIZE) {
            s_obdInstance->responseBuffer[s_obdInstance->responseLength++] = c;
            s_obdInstance->responseBuffer[s_obdInstance->responseLength] = '\0';
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
                                                    4096,
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

// OBD-II Handler Implementation
// Full ELM327 BLE adapter support using NimBLE
//
// ARCHITECTURE:
// - Creates a separate NimBLE client instance for ELM327
// - V1 BLE scan detects both V1 and ELM327 devices
// - When ELM327 found, onELM327Found() is called to queue connection
// - State machine manages connection, initialization, and polling

#include "obd_handler.h"
#include "settings.h"
#include "ble_client.h"
#include <NimBLEDevice.h>

// External references
extern V1BLEClient bleClient;

// Static constexpr definitions
constexpr const char* OBDHandler::ELM327_NAME_PATTERNS[];
constexpr const char* OBDHandler::NUS_SERVICE_UUID;
constexpr const char* OBDHandler::NUS_RX_CHAR_UUID;
constexpr const char* OBDHandler::NUS_TX_CHAR_UUID;

// Global instance
OBDHandler obdHandler;

// Static instance pointer for callbacks
static OBDHandler* s_obdInstance = nullptr;

// Logging macros
#define OBD_LOGF(...) do { if (DEBUG_OBD) Serial.printf(__VA_ARGS__); } while(0)
#define OBD_LOGLN(msg) do { if (DEBUG_OBD) Serial.println(msg); } while(0)

// Simple RAII lock for the OBD mutex
class ObdLock {
public:
    explicit ObdLock(SemaphoreHandle_t m) : mutex(m), locked(false) {
        if (mutex) locked = xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
    }
    ~ObdLock() {
        if (locked) xSemaphoreGive(mutex);
    }
    bool ok() const { return locked; }
private:
    SemaphoreHandle_t mutex;
    bool locked;
};

// Security callbacks for ELM327 pairing
class OBDSecurityCallbacks : public NimBLEClientCallbacks {
public:
    void onConnect(NimBLEClient* pClient) override {
        Serial.println("[OBD] Security: Connected");
    }
    
    void onDisconnect(NimBLEClient* pClient, int reason) override {
        Serial.printf("[OBD] Security: Disconnected, reason=%d\n", reason);
    }
    
    void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
        // Device is asking us to enter a PIN
        // Try common ELM327 PINs: 1234, 0000, 6789
        Serial.println("[OBD] Security: PassKey entry requested - trying 1234");
        NimBLEDevice::injectPassKey(connInfo, 1234);
    }
    
    void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pass_key) override {
        // Device is asking us to confirm a displayed PIN
        Serial.printf("[OBD] Security: Confirm passkey %06d - accepting\n", pass_key);
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }
    
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (connInfo.isEncrypted()) {
            Serial.println("[OBD] Security: Authentication complete (encrypted)");
        } else {
            Serial.println("[OBD] Security: Authentication complete (NOT encrypted)");
        }
    }
};

static OBDSecurityCallbacks obdSecurityCallbacks;

OBDHandler::OBDHandler() 
    : state(OBDState::OBD_DISABLED)
    , moduleDetected(false)
    , detectionComplete(false)
    , detectionStartMs(0)
    , pOBDClient(nullptr)
    , pNUSService(nullptr)
    , pRXChar(nullptr)
    , pTXChar(nullptr)
    , hasTargetDevice(false)
    , targetDeviceName("")
    , scanActive(false)
    , scanStartMs(0)
    , responseComplete(false)
    , lastPollMs(0)
    , obdMutex(nullptr)
    , obdTaskHandle(nullptr)
    , taskRunning(false) {
    
    // Initialize lastData with zero values
    lastData.speed_kph = 0;
    lastData.speed_mph = 0;
    lastData.rpm = 0;
    lastData.voltage = 0;
    lastData.valid = false;
    lastData.timestamp_ms = 0;
    
    s_obdInstance = this;
}

OBDHandler::~OBDHandler() {
    disconnect();
    if (obdMutex) {
        vSemaphoreDelete(obdMutex);
        obdMutex = nullptr;
    }
}

void OBDHandler::begin() {
    // Create mutex if not already created
    if (!obdMutex) {
        obdMutex = xSemaphoreCreateMutex();
    }
    
    startTask();
    
    // Check if we have a saved device to reconnect to
    String savedAddr = settingsManager.getObdDeviceAddress();
    String savedName = settingsManager.getObdDeviceName();
    
    if (savedAddr.length() > 0) {
        // We have a saved device - scan for 120s to try reconnecting
        Serial.printf("[OBD] Saved device: %s (%s) - scanning to reconnect...\n", 
                      savedName.c_str(), savedAddr.c_str());
        
        targetAddress = NimBLEAddress(std::string(savedAddr.c_str()), BLE_ADDR_PUBLIC);
        targetDeviceName = savedName;
        hasTargetDevice = true;
        
        detectionStartMs = millis();
        state = OBDState::SCANNING;
        scanActive = true;
        
        // Trigger actual BLE scan
        bleClient.startOBDScan();
    } else {
        // No saved device - wait for manual scan from UI
        Serial.println("[OBD] No saved device - waiting for manual scan");
        state = OBDState::IDLE;
        scanActive = false;
        detectionComplete = true;  // Not "failed", just idle
    }
}

bool OBDHandler::update() {
    // If background task is running, just report freshness
    if (taskRunning) {
        return hasValidData();
    }
    // Fallback to synchronous processing (e.g., if task failed to start)
    return runStateMachine();
}

const char* OBDHandler::getStateString() const {
    switch (state) {
        case OBDState::OBD_DISABLED: return "DISABLED";
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

// Thread-safe data accessors
OBDData OBDHandler::getData() const {
    ObdLock lock(obdMutex);
    return lastData;
}

bool OBDHandler::hasValidData() const {
    ObdLock lock(obdMutex);
    return lastData.valid && (millis() - lastData.timestamp_ms) <= 2000;
}

bool OBDHandler::isDataStale(uint32_t maxAge_ms) const {
    ObdLock lock(obdMutex);
    return (millis() - lastData.timestamp_ms) > maxAge_ms;
}

float OBDHandler::getSpeedKph() const {
    ObdLock lock(obdMutex);
    return lastData.speed_kph;
}

float OBDHandler::getSpeedMph() const {
    ObdLock lock(obdMutex);
    return lastData.speed_mph;
}

std::vector<OBDDeviceInfo> OBDHandler::getFoundDevices() const {
    ObdLock lock(obdMutex);
    return foundDevices;  // Return copy
}

void OBDHandler::clearFoundDevices() {
    ObdLock lock(obdMutex);
    foundDevices.clear();
}

bool OBDHandler::isELM327Device(const std::string& name) {
    if (name.empty()) return false;
    
    // Convert name to uppercase for case-insensitive matching
    String upperName = String(name.c_str());
    upperName.toUpperCase();
    
    for (int i = 0; i < ELM327_NAME_PATTERN_COUNT; i++) {
        String pattern = String(ELM327_NAME_PATTERNS[i]);
        pattern.toUpperCase();
        if (upperName.indexOf(pattern) >= 0) {
            return true;
        }
    }
    return false;
}

void OBDHandler::onELM327Found(const NimBLEAdvertisedDevice* device) {
    const std::string& name = device->getName();
    String addrStr = String(device->getAddress().toString().c_str());
    
    // Always add to found devices list (for UI display)
    // Check if already in list
    bool alreadyFound = false;
    {
        ObdLock lock(obdMutex);
        for (const auto& d : foundDevices) {
            if (d.address == addrStr) {
                alreadyFound = true;
                break;
            }
        }
        if (!alreadyFound) {
            OBDDeviceInfo info;
            info.address = addrStr;
            info.name = String(name.c_str());
            info.rssi = device->getRSSI();
            foundDevices.push_back(info);
            Serial.printf("[OBD] Found ELM327 device: '%s' [%s] RSSI:%d\n", 
                          name.c_str(), addrStr.c_str(), info.rssi);
        }
    }
    
    // If we're in SCANNING state and auto-connecting, connect to first device found
    if (state != OBDState::SCANNING) {
        return;  // Already found or not looking
    }
    
    // Save target device info
    targetAddress = device->getAddress();
    targetDeviceName = name.c_str();
    hasTargetDevice = true;
    
    // Mark as detected
    moduleDetected = true;
    detectionComplete = true;
    
    // Transition to connecting state
    state = OBDState::CONNECTING;
}

void OBDHandler::onDeviceFound(const NimBLEAdvertisedDevice* device) {
    // Called for ANY named BLE device during active scan
    // Just adds to list for user selection - no auto-connect
    const std::string& name = device->getName();
    String addrStr = String(device->getAddress().toString().c_str());
    
    // Skip devices with no name or very short names
    if (name.empty() || name.length() < 2) {
        return;
    }
    
    ObdLock lock(obdMutex);
    
    // Check if already in list
    for (const auto& d : foundDevices) {
        if (d.address == addrStr) {
            return;  // Already found
        }
    }
    
    // Add to found devices list
    OBDDeviceInfo info;
    info.address = addrStr;
    info.name = String(name.c_str());
    info.rssi = device->getRSSI();
    foundDevices.push_back(info);
    Serial.printf("[OBD] Found BLE device #%d: '%s' [%s] RSSI:%d\n", 
                  (int)foundDevices.size(), name.c_str(), addrStr.c_str(), info.rssi);
}

void OBDHandler::handleConnecting() {
    if (!hasTargetDevice) {
        state = OBDState::FAILED;
        return;
    }
    
    Serial.printf("[OBD] Connecting to %s...\n", targetDeviceName.c_str());
    
    if (connectToDevice()) {
        Serial.println("[OBD] Connected! Discovering services...");
        if (discoverServices()) {
            Serial.println("[OBD] Services discovered, initializing ELM327...");
            state = OBDState::INITIALIZING;
        } else {
            Serial.println("[OBD] Service discovery failed");
            disconnect();
            state = OBDState::DISCONNECTED;
            lastPollMs = millis();  // For reconnect delay
        }
    } else {
        Serial.println("[OBD] Connection failed");
        state = OBDState::DISCONNECTED;
        lastPollMs = millis();  // For reconnect delay
    }
}

void OBDHandler::handleInitializing() {
    if (initializeELM327()) {
        Serial.println("[OBD] ELM327 initialized successfully");
        state = OBDState::READY;
    } else {
        Serial.println("[OBD] ELM327 initialization failed");
        disconnect();
        state = OBDState::DISCONNECTED;
        lastPollMs = millis();
    }
}

void OBDHandler::handlePolling() {
    // Check if still connected
    if (pOBDClient && !pOBDClient->isConnected()) {
        Serial.println("[OBD] Connection lost");
        lastData.valid = false;
        state = OBDState::DISCONNECTED;
        lastPollMs = millis();
        return;
    }
    
    // Poll at regular intervals
    if (millis() - lastPollMs < POLL_INTERVAL_MS) {
        return;
    }
    lastPollMs = millis();
    
    // Request speed
    requestSpeed();
}

bool OBDHandler::runStateMachine() {
    // Handle scan timeout
    if (state == OBDState::SCANNING && scanActive) {
        if (millis() - detectionStartMs > DETECTION_TIMEOUT_MS) {
            // Scan timed out - go to IDLE, not FAILED
            state = OBDState::IDLE;
            scanActive = false;
            detectionComplete = true;
            Serial.println("[OBD] Scan timeout (120s) - returning to idle. Use UI to scan again.");
        }
    }
    
    switch (state) {
        case OBDState::OBD_DISABLED:
        case OBDState::FAILED:
            return false;
        case OBDState::IDLE:
        case OBDState::SCANNING:
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
            if (millis() - lastPollMs > 5000) {
                if (hasTargetDevice) {
                    state = OBDState::CONNECTING;
                }
            }
            return false;
    }
    return false;
}

void OBDHandler::taskEntry(void* param) {
    OBDHandler* self = static_cast<OBDHandler*>(param);
    if (!self) vTaskDelete(nullptr);
    for (;;) {
        self->runStateMachine();
        vTaskDelay(pdMS_TO_TICKS(10));  // keep loop responsive
    }
}

void OBDHandler::startTask() {
    if (obdTaskHandle) {
        taskRunning = true;
        return;
    }
    BaseType_t res = xTaskCreatePinnedToCore(taskEntry, "obdTask", 4096, this, 1, &obdTaskHandle, tskNO_AFFINITY);
    taskRunning = (res == pdPASS);
}

bool OBDHandler::connectToDevice() {
    Serial.printf("[OBD] connectToDevice() called, target: %s\n", targetAddress.toString().c_str());
    
    // Create client if needed
    if (!pOBDClient) {
        Serial.println("[OBD] Creating new BLE client with security...");
        pOBDClient = NimBLEDevice::createClient();
        if (!pOBDClient) {
            Serial.println("[OBD] Failed to create BLE client");
            return false;
        }
        
        // Set security callbacks for pairing/bonding
        pOBDClient->setClientCallbacks(&obdSecurityCallbacks);
        
        // Configure client for OBD (relaxed timing)
        pOBDClient->setConnectionParams(12, 12, 0, 500);  // min, max, latency, timeout
    }
    
    // Configure security for commodity ELM327 clones: bond only, no MITM/SC, no keypad needed
    // Keep this relaxed to avoid impacting the V1 link (NimBLE security is global)
    NimBLEDevice::setSecurityAuth(true, false, false);      // bonding only
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT); // Just Works / PIN-less
    
    Serial.printf("[OBD] Attempting BLE connect (10s timeout, security enabled)...\n");
    
    // Connect with 10 second timeout
    // NimBLE handles pairing automatically if the device requests it
    if (!pOBDClient->connect(targetAddress, false, 10)) {
        Serial.printf("[OBD] Failed to connect to %s\n", targetAddress.toString().c_str());
        return false;
    }
    
    Serial.println("[OBD] BLE connected successfully!");
    return true;
}

bool OBDHandler::discoverServices() {
    if (!pOBDClient) {
        Serial.println("[OBD] discoverServices: No client!");
        return false;
    }
    if (!pOBDClient->isConnected()) {
        Serial.println("[OBD] discoverServices: Not connected (disconnected after connect?)");
        return false;
    }
    
    Serial.println("[OBD] Starting service discovery...");
    
    // Force refresh of services
    if (!pOBDClient->discoverAttributes()) {
        Serial.println("[OBD] Failed to discover attributes");
        // Continue anyway, some devices work without explicit discovery
    }
    
    // List ALL services first for debugging
    Serial.println("[OBD] Available services:");
    const std::vector<NimBLERemoteService*>& services = pOBDClient->getServices(true);
    for (NimBLERemoteService* svc : services) {
        if (svc) {
            Serial.printf("  - Service: %s\n", svc->getUUID().toString().c_str());
            // Also list characteristics
            const std::vector<NimBLERemoteCharacteristic*>& chars = svc->getCharacteristics(true);
            for (NimBLERemoteCharacteristic* chr : chars) {
                if (chr) {
                    uint8_t props = chr->getRemoteService() ? 0 : 0;  // placeholder
                    Serial.printf("      Char: %s\n", chr->getUUID().toString().c_str());
                }
            }
        }
    }
    if (services.empty()) {
        Serial.println("[OBD] No services found!");
    }
    
    // Look for Nordic UART Service
    pNUSService = pOBDClient->getService(NUS_SERVICE_UUID);
    if (!pNUSService) {
        Serial.println("[OBD] Nordic UART Service not found, trying FFF0...");
        
        // Some adapters use different service UUIDs - try FFF0 (common alternative)
        pNUSService = pOBDClient->getService("FFF0");
        if (!pNUSService) {
            // Try 0xFFE0 (another common ELM327 service)
            Serial.println("[OBD] FFF0 not found, trying FFE0...");
            pNUSService = pOBDClient->getService("FFE0");
        }
        
        if (!pNUSService) {
            Serial.println("[OBD] No known ELM327 service found!");
            return false;
        }
        
        Serial.printf("[OBD] Found alternate service: %s\n", pNUSService->getUUID().toString().c_str());
        
        // Try common characteristic UUIDs for alternate services
        // FFF0 uses FFF1/FFF2, FFE0 uses FFE1
        pTXChar = pNUSService->getCharacteristic("FFF1");
        if (!pTXChar) pTXChar = pNUSService->getCharacteristic("FFE1");
        
        pRXChar = pNUSService->getCharacteristic("FFF2");
        if (!pRXChar) pRXChar = pNUSService->getCharacteristic("FFE1");  // Some use same char for both
    } else {
        Serial.println("[OBD] Found Nordic UART Service");
        // Standard NUS characteristics
        pTXChar = pNUSService->getCharacteristic(NUS_TX_CHAR_UUID);
        pRXChar = pNUSService->getCharacteristic(NUS_RX_CHAR_UUID);
    }
    
    if (!pTXChar) {
        Serial.println("[OBD] TX characteristic not found");
        return false;
    }
    
    if (!pRXChar) {
        Serial.println("[OBD] RX characteristic not found");
        return false;
    }
    
    // Subscribe to notifications
    if (pTXChar->canNotify()) {
        if (!pTXChar->subscribe(true, notificationCallback)) {
            Serial.println("[OBD] Failed to subscribe to notifications");
            return false;
        }
        Serial.println("[OBD] Subscribed to ELM327 notifications");
    } else {
        Serial.println("[OBD] TX characteristic doesn't support notifications");
        return false;
    }
    
    return true;
}

void OBDHandler::notificationCallback(NimBLERemoteCharacteristic* pChar, 
                                       uint8_t* pData, size_t length, bool isNotify) {
    if (!s_obdInstance || length == 0) return;

    ObdLock lock(s_obdInstance->obdMutex);
    if (!lock.ok()) return;
    
    // Append data to response buffer
    for (size_t i = 0; i < length; i++) {
        char c = (char)pData[i];
        
        // ELM327 uses '>' as command prompt (end of response)
        if (c == '>') {
            s_obdInstance->responseComplete = true;
            return;
        }
        
        // Filter out \r and \n, append others
        if (c != '\r' && c != '\n' && s_obdInstance->responseBuffer.length() < RESPONSE_BUFFER_SIZE) {
            s_obdInstance->responseBuffer += c;
        }
    }
}

bool OBDHandler::initializeELM327() {
    String response;
    
    // Reset ELM327
    Serial.println("[OBD] Sending ATZ (reset)...");
    if (!sendATCommand("ATZ", response, 3000)) {
        Serial.println("[OBD] ATZ failed");
        return false;
    }
    Serial.printf("[OBD] ATZ response: %s\n", response.c_str());
    
    // Check for ELM327 in response
    if (response.indexOf("ELM") < 0 && response.indexOf("elm") < 0) {
        Serial.println("[OBD] Warning: ELM327 not confirmed in reset response");
    }
    
    // Echo off
    if (!sendATCommand("ATE0", response)) {
        Serial.println("[OBD] ATE0 failed");
        return false;
    }
    
    // Linefeeds off
    if (!sendATCommand("ATL0", response)) {
        Serial.println("[OBD] ATL0 failed");
        return false;
    }
    
    // Spaces off (compact responses)
    if (!sendATCommand("ATS0", response)) {
        Serial.println("[OBD] ATS0 failed");
        return false;
    }
    
    // Headers off
    if (!sendATCommand("ATH0", response)) {
        Serial.println("[OBD] ATH0 failed");
        return false;
    }
    
    // Auto-detect protocol
    Serial.println("[OBD] Sending ATSP0 (auto protocol)...");
    if (!sendATCommand("ATSP0", response, 5000)) {
        Serial.println("[OBD] ATSP0 failed");
        return false;
    }
    
    // Try a test query to verify OBD connection to vehicle
    Serial.println("[OBD] Testing vehicle connection with 0100...");
    if (!sendATCommand("0100", response, 5000)) {
        Serial.println("[OBD] 0100 (PIDs supported) failed - vehicle may not be running");
        // Don't fail here - vehicle might just be off
    } else {
        Serial.printf("[OBD] Vehicle response: %s\n", response.c_str());
    }
    
    return true;
}

bool OBDHandler::sendATCommand(const char* cmd, String& response, uint32_t timeout_ms) {
    if (!pRXChar || !pOBDClient || !pOBDClient->isConnected()) {
        return false;
    }
    
    ObdLock lock(obdMutex);
    if (!lock.ok()) return false;
    
    // Clear response buffer
    responseBuffer = "";
    responseComplete = false;
    
    // Send command with carriage return
    String cmdStr = String(cmd) + "\r";
    
    OBD_LOGF("[OBD] TX: %s\n", cmd);
    
    if (!pRXChar->writeValue((uint8_t*)cmdStr.c_str(), cmdStr.length(), false)) {
        Serial.printf("[OBD] Failed to write command: %s\n", cmd);
        return false;
    }
    
    // Wait for response (with '>' prompt)
    uint32_t startMs = millis();
    while (!responseComplete && (millis() - startMs) < timeout_ms) {
        vTaskDelay(1);  // Yield to keep main loop responsive
    }
    
    if (!responseComplete) {
        Serial.printf("[OBD] Command timeout: %s\n", cmd);
        response = responseBuffer;  // Return partial response
        return false;
    }
    
    response = responseBuffer;
    OBD_LOGF("[OBD] RX: %s\n", response.c_str());
    
    // Check for error responses
    if (response.indexOf("ERROR") >= 0 || 
        response.indexOf("UNABLE") >= 0 ||
        response.indexOf("NO DATA") >= 0 ||
        response.indexOf("?") >= 0) {
        return false;
    }
    
    return true;
}

void OBDHandler::sendCommand(const char* cmd) {
    if (!pRXChar) return;
    
    String cmdStr = String(cmd) + "\r";
    pRXChar->writeValue((uint8_t*)cmdStr.c_str(), cmdStr.length(), false);
}

bool OBDHandler::requestSpeed() {
    String response;
    
    // Send PID 0x0D (Vehicle Speed)
    if (!sendATCommand("010D", response, 250)) {  // keep timeout short to avoid loop stalls
        lastData.valid = false;
        return false;
    }
    
    uint8_t speedKph;
    if (parseSpeedResponse(response, speedKph)) {
        lastData.speed_kph = speedKph;
        lastData.speed_mph = speedKph * 0.621371f;
        lastData.timestamp_ms = millis();
        lastData.valid = true;
        
        OBD_LOGF("[OBD] Speed: %d km/h (%.1f mph)\n", speedKph, lastData.speed_mph);
        return true;
    }
    
    return false;
}

bool OBDHandler::requestRPM() {
    String response;
    
    // Send PID 0x0C (Engine RPM)
    if (!sendATCommand("010C", response, 250)) {
        return false;
    }
    
    uint16_t rpm;
    if (parseRPMResponse(response, rpm)) {
        lastData.rpm = rpm;
        lastData.timestamp_ms = millis();
        return true;
    }
    
    return false;
}

bool OBDHandler::requestVoltage() {
    String response;
    
    // AT RV returns battery voltage
    if (!sendATCommand("ATRV", response, 250)) {
        return false;
    }
    
    float voltage;
    if (parseVoltageResponse(response, voltage)) {
        lastData.voltage = voltage;
        lastData.timestamp_ms = millis();
        return true;
    }
    
    return false;
}

bool OBDHandler::parseSpeedResponse(const String& response, uint8_t& speedKph) {
    // Response format: "410DXX" where XX is speed in km/h (hex)
    // With spaces off, look for "410D" followed by 2 hex digits
    
    int idx = response.indexOf("410D");
    if (idx < 0) {
        // Try lowercase
        idx = response.indexOf("410d");
    }
    if (idx < 0) {
        return false;
    }
    
    // Get the hex value after "410D"
    String hexVal = response.substring(idx + 4, idx + 6);
    hexVal.trim();
    
    if (hexVal.length() < 2) {
        return false;
    }
    
    speedKph = (uint8_t)strtoul(hexVal.c_str(), nullptr, 16);
    return true;
}

bool OBDHandler::parseRPMResponse(const String& response, uint16_t& rpm) {
    // Response format: "410CXXYY" where RPM = ((XX * 256) + YY) / 4
    
    int idx = response.indexOf("410C");
    if (idx < 0) {
        idx = response.indexOf("410c");
    }
    if (idx < 0) {
        return false;
    }
    
    String hexA = response.substring(idx + 4, idx + 6);
    String hexB = response.substring(idx + 6, idx + 8);
    
    if (hexA.length() < 2 || hexB.length() < 2) {
        return false;
    }
    
    uint8_t a = (uint8_t)strtoul(hexA.c_str(), nullptr, 16);
    uint8_t b = (uint8_t)strtoul(hexB.c_str(), nullptr, 16);
    
    rpm = ((uint16_t)a * 256 + b) / 4;
    return true;
}

bool OBDHandler::parseVoltageResponse(const String& response, float& voltage) {
    // Response format: "12.5V" or similar
    
    voltage = response.toFloat();
    return voltage > 0 && voltage < 20;  // Sanity check
}

void OBDHandler::disconnect() {
    if (pOBDClient && pOBDClient->isConnected()) {
        Serial.println("[OBD] Disconnecting...");
        pOBDClient->disconnect();
    }
    
    pNUSService = nullptr;
    pRXChar = nullptr;
    pTXChar = nullptr;
    lastData.valid = false;
    
    if (state != OBDState::OBD_DISABLED && state != OBDState::FAILED) {
        state = OBDState::DISCONNECTED;
    }
}

void OBDHandler::startScan() {
    // Don't clear foundDevices - user may have multiple devices and want to accumulate results
    // They can clear manually via clearFoundDevices() if needed
    scanActive = true;
    scanStartMs = millis();
    
    // Reset state to scanning (but don't disconnect if already connected)
    if (state != OBDState::POLLING && state != OBDState::READY) {
        hasTargetDevice = false;
        moduleDetected = false;
        detectionComplete = false;
        state = OBDState::SCANNING;
    }
    detectionStartMs = millis();
    
    Serial.println("[OBD] Manual scan started - looking for ELM327 devices");
    
    // Trigger actual BLE scan via V1 BLE client
    bleClient.startOBDScan();
}

void OBDHandler::stopScan() {
    if (!scanActive) {
        Serial.println("[OBD] stopScan() - scan not active");
        return;
    }
    
    Serial.println("[OBD] Stopping scan manually");
    
    // Stop the BLE scan
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
    }
    
    // Mark scan as complete
    onScanComplete();
}

void OBDHandler::onScanComplete() {
    if (!scanActive) return;
    
    scanActive = false;
    Serial.printf("[OBD] Scan complete - found %d devices\n", foundDevices.size());
    
    // If we're in SCANNING state, go back to IDLE
    if (state == OBDState::SCANNING) {
        state = OBDState::IDLE;
        detectionComplete = true;
    }
}

bool OBDHandler::connectToAddress(const String& address, const String& name) {
    Serial.printf("[OBD] Connecting to specific device: %s (%s)\n", 
                  address.c_str(), name.length() > 0 ? name.c_str() : "unknown");
    
    // Ensure mutex is created (in case begin() wasn't called)
    if (!obdMutex) {
        obdMutex = xSemaphoreCreateMutex();
    }
    
    // Disconnect from current device if any
    disconnect();
    
    // Lock while modifying state variables
    ObdLock lock(obdMutex);
    
    // Set target device - NimBLEAddress needs std::string and address type
    targetAddress = NimBLEAddress(std::string(address.c_str()), BLE_ADDR_PUBLIC);
    targetDeviceName = name.length() > 0 ? name : address;
    hasTargetDevice = true;
    
    // Mark as detected and start connecting
    moduleDetected = true;
    detectionComplete = true;
    scanActive = false;
    state = OBDState::CONNECTING;
    
    Serial.printf("[OBD] State set to CONNECTING, hasTarget=%d\n", hasTargetDevice);
    
    return true;
}

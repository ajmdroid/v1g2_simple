// OBD-II Handler Implementation
// Full ELM327 BLE adapter support using NimBLE
//
// ARCHITECTURE:
// - Creates a separate NimBLE client instance for ELM327
// - V1 BLE scan detects both V1 and ELM327 devices
// - When ELM327 found, onELM327Found() is called to queue connection
// - State machine manages connection, initialization, and polling

#include "obd_handler.h"
#include <NimBLEDevice.h>

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
    , responseComplete(false)
    , lastPollMs(0)
    , obdMutex(nullptr) {
    
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
    
    detectionStartMs = millis();
    state = OBDState::SCANNING;
    
    Serial.println("[OBD] OBD Handler initialized - scanning for ELM327 devices");
    Serial.println("[OBD] Looking for: OBDII, OBD2, ELM327, Vgate, iCar, KONNWEI, Viecar, Veepeak");
    
    // Note: Actual scanning is done by the V1 BLE scan in ble_client.cpp
    // When an ELM327 device is found, onELM327Found() will be called
}

bool OBDHandler::update() {
    // Handle detection timeout
    if (state == OBDState::SCANNING) {
        if (millis() - detectionStartMs > DETECTION_TIMEOUT_MS) {
            state = OBDState::FAILED;
            detectionComplete = true;
            moduleDetected = false;
            Serial.println("[OBD] Module NOT detected (timeout after 60s) - OBD disabled");
            return false;
        }
    }
    
    // State machine
    switch (state) {
        case OBDState::OBD_DISABLED:
        case OBDState::FAILED:
            return false;
            
        case OBDState::IDLE:
        case OBDState::SCANNING:
            // Waiting for onELM327Found() callback from V1 scan
            return false;
            
        case OBDState::CONNECTING:
            handleConnecting();
            return false;
            
        case OBDState::INITIALIZING:
            handleInitializing();
            return false;
            
        case OBDState::READY:
            // Start polling
            state = OBDState::POLLING;
            lastPollMs = millis();
            return false;
            
        case OBDState::POLLING:
            handlePolling();
            return lastData.valid;
            
        case OBDState::DISCONNECTED:
            // Attempt reconnect after delay
            if (millis() - lastPollMs > 5000) {
                if (hasTargetDevice) {
                    state = OBDState::CONNECTING;
                }
            }
            return false;
    }
    
    return false;
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

bool OBDHandler::isDataStale(uint32_t maxAge_ms) const {
    return (millis() - lastData.timestamp_ms) > maxAge_ms;
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
    if (state != OBDState::SCANNING) {
        return;  // Already found or not looking
    }
    
    const std::string& name = device->getName();
    Serial.printf("[OBD] Found ELM327 device: '%s' [%s]\n", 
                  name.c_str(), device->getAddress().toString().c_str());
    
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

bool OBDHandler::connectToDevice() {
    // Create client if needed
    if (!pOBDClient) {
        pOBDClient = NimBLEDevice::createClient();
        if (!pOBDClient) {
            Serial.println("[OBD] Failed to create BLE client");
            return false;
        }
        
        // Configure client for OBD (relaxed timing)
        pOBDClient->setConnectionParams(12, 12, 0, 500);  // min, max, latency, timeout
    }
    
    // Connect with 10 second timeout
    if (!pOBDClient->connect(targetAddress, false, 10)) {
        Serial.printf("[OBD] Failed to connect to %s\n", targetAddress.toString().c_str());
        return false;
    }
    
    return true;
}

bool OBDHandler::discoverServices() {
    if (!pOBDClient || !pOBDClient->isConnected()) {
        return false;
    }
    
    // Look for Nordic UART Service
    pNUSService = pOBDClient->getService(NUS_SERVICE_UUID);
    if (!pNUSService) {
        Serial.println("[OBD] Nordic UART Service not found");
        
        // Some adapters use different service UUIDs - try FFF0 (common alternative)
        pNUSService = pOBDClient->getService("FFF0");
        if (!pNUSService) {
            // List available services for debugging
            Serial.println("[OBD] Available services:");
            const std::vector<NimBLERemoteService*>& services = pOBDClient->getServices(true);
            for (NimBLERemoteService* svc : services) {
                if (svc) {
                    Serial.printf("  - %s\n", svc->getUUID().toString().c_str());
                }
            }
            return false;
        }
        Serial.println("[OBD] Found FFF0 service (alternate ELM327)");
        
        // FFF0 service uses FFF1 (notify) and FFF2 (write)
        pTXChar = pNUSService->getCharacteristic("FFF1");
        pRXChar = pNUSService->getCharacteristic("FFF2");
    } else {
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
        delay(10);
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
    if (!sendATCommand("010D", response, 1000)) {
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
    if (!sendATCommand("010C", response, 1000)) {
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
    if (!sendATCommand("ATRV", response, 1000)) {
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


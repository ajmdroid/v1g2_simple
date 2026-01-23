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
#include "debug_logger.h"
#include <NimBLEDevice.h>

// External references
extern V1BLEClient bleClient;
extern DebugLogger debugLogger;

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

// Debug logger macro for settings-enabled OBD logging
#define OBD_DEBUG_LOGF(...) do { debugLogger.logf(DebugLogCategory::Obd, __VA_ARGS__); } while(0)

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

// Flag to track when authentication completes
static volatile bool s_authComplete = false;
static volatile bool s_authSuccess = false;

// Security callbacks for ELM327 pairing
class OBDSecurityCallbacks : public NimBLEClientCallbacks {
public:
    void onConnect(NimBLEClient* pClient) override {
        Serial.println("[OBD] Security: Connected");
        s_authComplete = false;  // Reset for new connection
        s_authSuccess = false;
    }
    
    void onDisconnect(NimBLEClient* pClient, int reason) override {
        Serial.printf("[OBD] Security: Disconnected, reason=%d\n", reason);
        // Mark auth as complete (failed) so we don't hang waiting
        s_authComplete = true;
        s_authSuccess = false;
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
        s_authComplete = true;
        s_authSuccess = connInfo.isEncrypted();
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
    , connectionFailures(0)
    , lastKnownRssi(-127)
    , pendingClientDelete(false)
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
    // Stop the task first to prevent races
    stopTask();
    
    // Clean up client
    if (pOBDClient) {
        if (pOBDClient->isConnected()) {
            pOBDClient->disconnect();
        }
        // In destructor, we can try to delete the client since we're shutting down
        // and no more BLE activity should be happening
        vTaskDelay(pdMS_TO_TICKS(500));  // Give NimBLE time to clean up
        NimBLEDevice::deleteClient(pOBDClient);
        pOBDClient = nullptr;
    }
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
    
    // Check if we have a saved device
    String savedAddr = settingsManager.getObdDeviceAddress();
    String savedName = settingsManager.getObdDeviceName();
    
    if (savedAddr.length() > 0) {
        // We have a saved device - store info but DON'T scan yet
        // Wait for V1 to connect and settle before attempting OBD connection
        Serial.printf("[OBD] Saved device: %s (%s) - waiting for V1 to connect first\n", 
                      savedName.c_str(), savedAddr.c_str());
        OBD_DEBUG_LOGF("[OBD] Saved device: %s - waiting for V1", savedName.c_str());
        
        targetAddress = NimBLEAddress(std::string(savedAddr.c_str()), BLE_ADDR_PUBLIC);
        targetDeviceName = savedName;
        hasTargetDevice = true;
        
        // Stay idle - tryAutoConnect() will be called after V1 settles
        state = OBDState::IDLE;
        scanActive = false;
        detectionComplete = false;
    } else {
        // No saved device - wait for manual scan from UI
        Serial.println("[OBD] No saved device - waiting for manual scan");
        OBD_DEBUG_LOGF("[OBD] No saved device configured");
        state = OBDState::IDLE;
        scanActive = false;
        detectionComplete = true;  // Not "failed", just idle
    }
}

void OBDHandler::tryAutoConnect() {
    // Called after V1 connection has settled
    // Only connect if we have a saved device and not already connecting/connected
    
    if (!hasTargetDevice) {
        Serial.println("[OBD] tryAutoConnect: No saved device - skipping");
        return;
    }
    
    if (state == OBDState::CONNECTING || state == OBDState::INITIALIZING ||
        state == OBDState::READY || state == OBDState::POLLING) {
        Serial.println("[OBD] tryAutoConnect: Already connecting or connected - skipping");
        return;
    }
    
    Serial.printf("[OBD] tryAutoConnect: Connecting to saved device %s (%s)\n",
                  targetDeviceName.c_str(), targetAddress.toString().c_str());
    OBD_DEBUG_LOGF("[OBD] Auto-connecting to %s", targetDeviceName.c_str());
    
    // Reset connection failure counter for fresh auto-connect attempt
    connectionFailures = 0;
    
    // Go directly to connecting state - no scan needed
    state = OBDState::CONNECTING;
    detectionStartMs = millis();
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
    // Allow up to 3x poll interval for data freshness (BLE can have delays)
    // Poll interval is 1s, so 3s allows for occasional slow responses
    uint32_t age = millis() - lastData.timestamp_ms;
    bool fresh = lastData.valid && age <= 3000;
    return fresh;
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
    
    {
        ObdLock lock(obdMutex);
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
        OBD_DEBUG_LOGF("[OBD] Connected to %s", targetDeviceName.c_str());
        if (discoverServices()) {
            Serial.println("[OBD] Services discovered, initializing ELM327...");
            // Reset failure counter on successful connection
            connectionFailures = 0;
            state = OBDState::INITIALIZING;
        } else {
            Serial.println("[OBD] Service discovery failed");
            OBD_DEBUG_LOGF("[OBD] Service discovery failed for %s", targetDeviceName.c_str());
            disconnect();
            connectionFailures++;
            Serial.printf("[OBD] Connection failures: %d/%d\n", connectionFailures, MAX_CONNECTION_FAILURES);
            if (connectionFailures >= MAX_CONNECTION_FAILURES) {
                Serial.println("[OBD] Max failures reached - OBD adapter may be off or out of range");
            }
            state = OBDState::DISCONNECTED;
            lastPollMs = millis();  // For reconnect delay
        }
    } else {
        Serial.println("[OBD] Connection failed");
        OBD_DEBUG_LOGF("[OBD] Connection failed to %s", targetDeviceName.c_str());
        connectionFailures++;
        Serial.printf("[OBD] Connection failures: %d/%d\n", connectionFailures, MAX_CONNECTION_FAILURES);
        if (connectionFailures >= MAX_CONNECTION_FAILURES) {
            Serial.println("[OBD] Max failures reached - OBD adapter may be off or out of range");
        }
        state = OBDState::DISCONNECTED;
        lastPollMs = millis();  // For reconnect delay
    }
}

void OBDHandler::handleInitializing() {
    if (initializeELM327()) {
        Serial.println("[OBD] ELM327 initialized successfully");
        OBD_DEBUG_LOGF("[OBD] ELM327 initialized - %s ready", targetDeviceName.c_str());
        // Fully connected and ready - ensure failure counter is reset
        connectionFailures = 0;
        state = OBDState::READY;
    } else {
        Serial.println("[OBD] ELM327 initialization failed");
        OBD_DEBUG_LOGF("[OBD] ELM327 init failed for %s", targetDeviceName.c_str());
        disconnect();
        connectionFailures++;
        Serial.printf("[OBD] Connection failures: %d/%d\n", connectionFailures, MAX_CONNECTION_FAILURES);
        if (connectionFailures >= MAX_CONNECTION_FAILURES) {
            Serial.println("[OBD] Max failures reached - OBD adapter may be off or out of range");
        }
        state = OBDState::DISCONNECTED;
        lastPollMs = millis();
    }
}

void OBDHandler::handlePolling() {
    // Check if still connected
    if (pOBDClient && !pOBDClient->isConnected()) {
        Serial.println("[OBD] Connection lost");
        OBD_DEBUG_LOGF("[OBD] Connection lost to %s", targetDeviceName.c_str());
        {
            ObdLock lock(obdMutex);
            if (lock.ok()) {
                lastData.valid = false;
            }
        }
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
    // Note: We NO LONGER delete the OBD client - we reuse it!
    // Deleting NimBLE clients causes heap corruption in many scenarios.
    // Instead, we keep the client alive and just disconnect/reconnect as needed.
    // The client will be cleaned up properly when the OBDHandler is destroyed.
    if (pendingClientDelete) {
        Serial.println("[OBD] Clearing pending client delete flag (client reused, not deleted)");
        pendingClientDelete = false;
        // Don't set pOBDClient to nullptr - keep it for reuse
    }
    
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
            if (hasTargetDevice) {
                // Check if we've exceeded max connection failures
                if (connectionFailures >= MAX_CONNECTION_FAILURES) {
                    // Give up after too many failures - device is likely not available
                    // (No more log spam - we've already logged each failure)
                    return false;
                }
                
                // Calculate exponential backoff delay: 5s, 10s, 20s, 40s, 60s (capped)
                uint32_t retryDelay = BASE_RETRY_DELAY_MS * (1 << connectionFailures);
                if (retryDelay > MAX_RETRY_DELAY_MS) {
                    retryDelay = MAX_RETRY_DELAY_MS;
                }
                
                if (millis() - lastPollMs > retryDelay) {
                    // Before attempting reconnection, check if device is visible with decent signal
                    if (!checkDevicePresence()) {
                        // Device not found or weak signal - count as failure and extend backoff
                        connectionFailures++;
                        Serial.printf("[OBD] Device not visible or weak signal - failures: %d/%d\n", 
                                      connectionFailures, MAX_CONNECTION_FAILURES);
                        if (connectionFailures >= MAX_CONNECTION_FAILURES) {
                            Serial.println("[OBD] Max failures reached - OBD adapter may be off or out of range");
                        }
                        lastPollMs = millis();
                        return false;
                    }
                    
                    Serial.printf("[OBD] Retry attempt %d/%d (RSSI: %d)\n", 
                                  connectionFailures + 1, MAX_CONNECTION_FAILURES, lastKnownRssi);
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

void OBDHandler::stopTask() {
    if (obdTaskHandle) {
        TaskHandle_t handle = obdTaskHandle;
        obdTaskHandle = nullptr;
        taskRunning = false;
        vTaskDelete(handle);
    }
}

bool OBDHandler::checkDevicePresence() {
    // Quick BLE scan to check if target device is visible with acceptable signal
    // Returns true if device found with RSSI >= MIN_RSSI_FOR_CONNECT
    
    if (!hasTargetDevice) {
        return false;
    }
    
    Serial.printf("[OBD] Checking presence of %s...\n", targetDeviceName.c_str());
    
    // Use NimBLE's scan to look for our target device
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan) {
        Serial.println("[OBD] Failed to get scan object");
        return true;  // Fail open - let connection attempt proceed
    }
    
    // Configure for quick scan (2 seconds, active scan for name)
    pScan->setActiveScan(true);
    pScan->setInterval(100);  // 62.5ms
    pScan->setWindow(80);     // 50ms (80% duty)
    
    // Start blocking scan for 2 seconds (duration, not continuous)
    // NimBLE 2.x: start() returns bool, use getResults() to get scan results
    bool scanStarted = pScan->start(2000);  // 2000ms = 2 seconds
    if (!scanStarted) {
        Serial.println("[OBD] Failed to start presence scan");
        return true;  // Fail open - let connection attempt proceed
    }
    
    // Get results from completed scan
    NimBLEScanResults results = pScan->getResults();
    
    // Search results for our target device
    bool found = false;
    int rssi = -127;
    
    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice* device = results.getDevice(i);
        if (device && device->getAddress() == targetAddress) {
            found = true;
            rssi = device->getRSSI();
            lastKnownRssi = rssi;
            Serial.printf("[OBD] Found %s with RSSI: %d\n", targetDeviceName.c_str(), rssi);
            break;
        }
    }
    
    // Clear scan results to free memory
    pScan->clearResults();
    
    if (!found) {
        Serial.printf("[OBD] Device %s not found in scan\n", targetDeviceName.c_str());
        lastKnownRssi = -127;
        return false;
    }
    
    if (rssi < MIN_RSSI_FOR_CONNECT) {
        Serial.printf("[OBD] RSSI %d too weak (min: %d) - skipping connection\n", rssi, MIN_RSSI_FOR_CONNECT);
        return false;
    }
    
    return true;
}

bool OBDHandler::connectToDevice() {
    Serial.printf("[OBD] connectToDevice() called, target: %s\n", targetAddress.toString().c_str());
    
    // Note: We don't delete bonds here - let NimBLE manage bonding naturally
    // Deleting bonds on every connect can cause issues with some devices
    
    // Check if existing client is usable for reuse
    if (pOBDClient) {
        // If already connected, disconnect first
        if (pOBDClient->isConnected()) {
            Serial.println("[OBD] Disconnecting existing client before reconnect...");
            pOBDClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));  // Let disconnect complete
        }
        Serial.println("[OBD] Reusing existing BLE client");
    } else {
        // Create new client
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
    
    // Configure security for ELM327 adapters that require PIN pairing (e.g., Veepeak BLE+)
    // IMPORTANT: Use Legacy Pairing (sc=false), NOT Secure Connections!
    // Veepeak and most cheap ELM327 adapters only support legacy pairing with fixed PIN
    // The security callbacks handle PIN entry (onPassKeyEntry injects 1234)
    NimBLEDevice::setSecurityAuth(true, true, false);       // bonding, MITM, NO secure connections (legacy pairing)
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY); // We can "type" a PIN
    
    Serial.printf("[OBD] Attempting BLE connect (10s timeout, legacy pairing)...\n");
    
    // Reset auth flags before connecting
    s_authComplete = false;
    s_authSuccess = false;
    
    // Connect with 10 second timeout
    // NimBLE handles pairing automatically if the device requests it
    if (!pOBDClient->connect(targetAddress, false, 10)) {
        Serial.printf("[OBD] Failed to connect to %s\n", targetAddress.toString().c_str());
        // Keep client for reuse on next attempt - don't delete
        return false;
    }
    
    Serial.println("[OBD] BLE connected!");
    
    // Don't call secureConnection() - it causes heap corruption on some devices
    // Instead, let NimBLE handle pairing automatically when we try to access 
    // encrypted services. The security callbacks will inject the PIN when prompted.
    
    // Give the connection a moment to stabilize and for device to request pairing
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Check if still connected
    if (!pOBDClient->isConnected()) {
        Serial.println("[OBD] Lost connection after connect!");
        // Keep client for reuse on next attempt - don't delete
        return false;
    }
    
    Serial.println("[OBD] Connection stable, proceeding to service discovery...");
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
    
    // Small delay to let ELM327 settle before polling begins
    delay(200);
    
    return true;
}

bool OBDHandler::sendATCommand(const char* cmd, String& response, uint32_t timeout_ms) {
    if (!pRXChar || !pOBDClient || !pOBDClient->isConnected()) {
        return false;
    }
    
    {
        // Clear response buffer under lock
        ObdLock lock(obdMutex);
        if (!lock.ok()) return false;
        responseBuffer = "";
        responseComplete = false;
    }
    
    // Send command with carriage return
    String cmdStr = String(cmd) + "\r";
    
    OBD_LOGF("[OBD] TX: %s\n", cmd);
    
    if (!pRXChar->writeValue((uint8_t*)cmdStr.c_str(), cmdStr.length(), false)) {
        Serial.printf("[OBD] Failed to write command: %s\n", cmd);
        return false;
    }
    
    // Wait for response (with '>' prompt)
    uint32_t startMs = millis();
    while (true) {
        {
            ObdLock lock(obdMutex);
            if (lock.ok() && responseComplete) break;
        }
        if ((millis() - startMs) >= timeout_ms) break;
        vTaskDelay(1);  // Yield to keep main loop responsive
    }
    
    {
        ObdLock lock(obdMutex);
        response = responseBuffer;
        if (!responseComplete) {
            Serial.printf("[OBD] Command timeout: %s\n", cmd);
            return false;
        }
    }
    
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
    // Periodic speed logging (every 10 seconds when OBD logging enabled)
    static unsigned long lastSpeedLogMs = 0;
    static constexpr unsigned long SPEED_LOG_INTERVAL_MS = 10000;
    
    String response;
    
    // Send PID 0x0D (Vehicle Speed)
    // First query after init may take longer as ELM327 establishes vehicle communication
    if (!sendATCommand("010D", response, 1000)) {  // 1 second timeout for reliability
        ObdLock lock(obdMutex);
        if (lock.ok()) {
            lastData.valid = false;
        }
        OBD_DEBUG_LOGF("[OBD] Speed query failed");
        return false;
    }
    
    uint8_t speedKph;
    if (parseSpeedResponse(response, speedKph)) {
        ObdLock lock(obdMutex);
        if (lock.ok()) {
            lastData.speed_kph = speedKph;
            lastData.speed_mph = speedKph * 0.621371f;
            lastData.timestamp_ms = millis();
            lastData.valid = true;
        }
        
        OBD_LOGF("[OBD] Speed: %d km/h (%.1f mph)\n", speedKph, lastData.speed_mph);
        
        // Log speed periodically when OBD logging is enabled
        if (millis() - lastSpeedLogMs >= SPEED_LOG_INTERVAL_MS) {
            lastSpeedLogMs = millis();
            OBD_DEBUG_LOGF("[OBD] Speed: %d km/h (%.1f mph)", speedKph, lastData.speed_mph);
        }
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
        ObdLock lock(obdMutex);
        if (lock.ok()) {
            lastData.rpm = rpm;
            lastData.timestamp_ms = millis();
        }
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
        ObdLock lock(obdMutex);
        if (lock.ok()) {
            lastData.voltage = voltage;
            lastData.timestamp_ms = millis();
        }
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
    if (pOBDClient) {
        if (pOBDClient->isConnected()) {
            Serial.println("[OBD] Disconnecting...");
            pOBDClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));  // Let disconnect complete
        }
        // Keep client for reuse - don't delete it
        // Deleting NimBLE clients causes heap corruption
        Serial.println("[OBD] Client disconnected (kept for reuse)");
    }
    
    {
        ObdLock lock(obdMutex);
        pNUSService = nullptr;
        pRXChar = nullptr;
        pTXChar = nullptr;
    }
    {
        ObdLock lock(obdMutex);
        if (lock.ok()) lastData.valid = false;
    }
    
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
    
    // Only scan if V1 is connected - OBD uses second BLE client which needs V1 stable first
    if (!bleClient.isConnected()) {
        Serial.println("[OBD] ERROR: V1 not connected - connect V1 first before OBD scan");
        state = OBDState::IDLE;
        scanActive = false;
        return;
    }
    
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
    
    // Save device to settings for auto-reconnect on next boot
    settingsManager.setObdDevice(address, targetDeviceName);
    Serial.printf("[OBD] Saved device to settings: %s (%s)\n", address.c_str(), targetDeviceName.c_str());
    
    Serial.printf("[OBD] State set to CONNECTING, hasTarget=%d\n", hasTargetDevice);
    
    return true;
}

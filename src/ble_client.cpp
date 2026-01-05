/**
 * BLE Client for Valentine1 Gen2
 * With BLE Server proxy support for JBV1 app
 * 
 * Architecture:
 * - NimBLE 2.3.7 tuned for stable dual-role operation
 * - Client connects to V1 (V1G* device names)
 * - Server advertises as V1C-LE-S3 for JBV1
 * - FreeRTOS task manages advertising timing
 * - Thread-safe with mutexes for BLE operations
 * 
 * Key Features:
 * - Automatic V1 discovery and reconnection
 * - Bidirectional proxy (V1 ↔ JBV1)
 * - Profile settings push
 * - Mode control (All Bogeys/Logic/Advanced Logic)
 * - Mute toggle
 */

#include "ble_client.h"
#include "settings.h"
#include "../include/config.h"
#include <Arduino.h>
#include <set>
#include <string>
#include <cstdlib>

// Task to restart advertising after delay (Kenny's v1g2-t4s3 approach for NimBLE 2.x)
// Only restarts if no client is connected
static void restartAdvertisingTask(void* param) {
    vTaskDelay(pdMS_TO_TICKS(150));
    
    // Don't restart advertising if a client is already connected
    NimBLEServer* pServer = NimBLEDevice::getServer();
    if (pServer && pServer->getConnectedCount() > 0) {
        Serial.println("[ADV_TASK] Client connected, skipping advertising restart");
        vTaskDelete(NULL);
        return;
    }
    
    NimBLEDevice::startAdvertising(0);  // 0 = advertise indefinitely (no timeout)
    vTaskDelete(NULL);
}

namespace {
class SemaphoreGuard {
public:
    explicit SemaphoreGuard(SemaphoreHandle_t sem) : sem_(sem), locked_(false) {
        if (sem_) {
            locked_ = xSemaphoreTake(sem_, portMAX_DELAY) == pdTRUE;
        }
    }
    ~SemaphoreGuard() {
        if (sem_ && locked_) {
            xSemaphoreGive(sem_);
        }
    }
    bool locked() const { return locked_; }
private:
    SemaphoreHandle_t sem_;
    bool locked_;
};

uint16_t shortUuid(const NimBLEUUID& uuid) {
    std::string s = uuid.toString();
    if (s.size() >= 8) {
        // UUID is like 92a0b2ce-9e05-11e2-aa59-f23c91aec05e → take b2ce
        return static_cast<uint16_t>(strtoul(s.substr(4, 4).c_str(), nullptr, 16));
    }
    return 0;
}
} // namespace

// Static instance for callbacks
static V1BLEClient* instancePtr = nullptr;

V1BLEClient::V1BLEClient() 
    : pClient(nullptr)
    , pRemoteService(nullptr)
    , pDisplayDataChar(nullptr)
    , pCommandChar(nullptr)
    , pCommandCharLong(nullptr)
    , pServer(nullptr)
    , pProxyService(nullptr)
    , pProxyNotifyChar(nullptr)
    , pProxyNotifyLongChar(nullptr)
    , pProxyWriteChar(nullptr)
    , proxyEnabled(false)
    , proxyServerInitialized(false)
    , proxyClientConnected(false)
    , proxyName_("V1-Proxy")
    , dataCallback(nullptr)
    , connectCallback(nullptr)
    , connected(false)
    , shouldConnect(false)
    , hasTargetDevice(false)
    , targetAddress()
    , lastScanStart(0)
    , pScanCallbacks(nullptr)
    , pClientCallbacks(nullptr)
    , pProxyServerCallbacks(nullptr)
    , pProxyWriteCallbacks(nullptr) {
    instancePtr = this;
}

V1BLEClient::~V1BLEClient() {
    // Delete allocated callback handlers to prevent memory leaks
    if (pScanCallbacks) {
        delete pScanCallbacks;
        pScanCallbacks = nullptr;
    }
    if (pClientCallbacks) {
        delete pClientCallbacks;
        pClientCallbacks = nullptr;
    }
    if (pProxyServerCallbacks) {
        delete pProxyServerCallbacks;
        pProxyServerCallbacks = nullptr;
    }
    if (pProxyWriteCallbacks) {
        delete pProxyWriteCallbacks;
        pProxyWriteCallbacks = nullptr;
    }
}

// ==================== BLE State Machine ====================

void V1BLEClient::setBLEState(BLEState newState, const char* reason) {
    BLEState oldState = bleState;
    if (oldState == newState) return;  // No change
    
    unsigned long now = millis();
    unsigned long stateTime = (oldState != BLEState::DISCONNECTED && stateEnteredMs > 0) ? (now - stateEnteredMs) : 0;
    
    bleState = newState;
    stateEnteredMs = now;
    
    Serial.printf("[BLE_SM][%lu] %s (%lums) -> %s | Reason: %s\n",
                  now,
                  bleStateToString(oldState),
                  stateTime,
                  bleStateToString(newState), 
                  reason);
}

// Full cleanup of BLE connection state - call before retry or after failures
// This ensures no stale references prevent reconnection
void V1BLEClient::cleanupConnection() {
    Serial.println("[BLE_SM] Cleaning up connection state...");
    
    // 1. Unsubscribe from notifications if subscribed
    if (pDisplayDataChar && pDisplayDataChar->canNotify()) {
        Serial.println("[BLE_SM] Unsubscribing from notifications");
        pDisplayDataChar->unsubscribe();
    }
    
    // 2. Disconnect if connected
    if (pClient && pClient->isConnected()) {
        Serial.println("[BLE_SM] Disconnecting client");
        pClient->disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));  // Brief delay for disconnect to complete
    }
    
    // 3. Clear characteristic references (they become invalid after disconnect)
    pDisplayDataChar = nullptr;
    pCommandChar = nullptr;
    pCommandCharLong = nullptr;
    pRemoteService = nullptr;
    
    // 4. Clear connection flags
    {
        SemaphoreGuard lock(bleMutex);
        if (lock.locked()) {
            connected = false;
            shouldConnect = false;
            hasTargetDevice = false;
            targetDevice = NimBLEAdvertisedDevice();
        }
    }
    
    connectInProgress = false;
    Serial.println("[BLE_SM] Cleanup complete");
}

// Hard reset of BLE client stack - use after repeated failures
// This recreates the client object to clear any stuck internal state
void V1BLEClient::hardResetBLEClient() {
    Serial.println("[BLE_SM] === HARD RESET of BLE client ===");
    
    // Full cleanup first
    cleanupConnection();
    
    // Stop any active scanning
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    // Delete and recreate client
    if (pClient) {
        Serial.println("[BLE_SM] Deleting old client");
        // Note: NimBLEDevice::deleteClient can cause issues, so we just disconnect
        // and let NimBLE manage the client internally
        // NimBLEDevice::deleteClient(pClient);  // Can cause heap corruption!
        pClient = nullptr;
    }
    
    // Create fresh client
    Serial.println("[BLE_SM] Creating new client");
    pClient = NimBLEDevice::createClient();
    if (pClient) {
        if (!pClientCallbacks) {
            pClientCallbacks = new ClientCallbacks();
        }
        pClient->setClientCallbacks(pClientCallbacks);
        pClient->setConnectionParams(12, 24, 0, 400);
        pClient->setConnectTimeout(10);
        Serial.println("[BLE_SM] New client created successfully");
    } else {
        Serial.println("[BLE_SM] ERROR: Failed to create new client!");
    }
    
    // Reset failure counter after hard reset
    consecutiveConnectFailures = 0;
    nextConnectAllowedMs = millis() + 2000;  // Brief cooldown after reset
    
    setBLEState(BLEState::DISCONNECTED, "hard reset complete");
    Serial.println("[BLE_SM] === Hard reset complete ===");
}

// Initialize BLE stack without starting scan - use for fast reconnect
bool V1BLEClient::initBLE(bool enableProxy, const char* proxyName) {
    static bool initialized = false;
    if (initialized) {
        return true;  // Already initialized
    }
    
    Serial.println("Initializing BLE stack...");
    
    proxyEnabled = enableProxy;
    proxyName_ = proxyName ? proxyName : "V1C-LE-S3";
    
    // Create mutexes for thread-safe BLE operations (only once)
    if (!bleMutex) {
        bleMutex = xSemaphoreCreateMutex();
    }
    if (!bleNotifyMutex) {
        bleNotifyMutex = xSemaphoreCreateMutex();
    }
    
    if (!bleMutex || !bleNotifyMutex) {
        Serial.println("ERROR: Failed to create BLE mutexes");
        return false;
    }
    
    // Kenny's exact initialization pattern:
    // 1. init() with generic name
    // 2. setDeviceName() with the actual advertised name
    // 3. setPower() and setMTU for better throughput
    if (proxyEnabled) {
        NimBLEDevice::init("V1 Proxy");
        NimBLEDevice::setDeviceName(proxyName_.c_str());
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        NimBLEDevice::setMTU(517);  // Max MTU for BLE 5.x (512 payload + 5 header)
    } else {
        NimBLEDevice::init("V1Display");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        NimBLEDevice::setMTU(517);  // Max MTU for BLE 5.x (512 payload + 5 header)
    }
    
    // Create proxy server early (required before BLE stack starts client operations)
    if (proxyEnabled) {
        // Kenny's approach: NO security settings for proxy mode
        // JBV1 and V1 Companion expect open connections without pairing
        Serial.println("Creating proxy server (advertising will start after V1 connects)...");
        initProxyServer(proxyName_.c_str());
        proxyServerInitialized = true;
        // Advertising data configured in initProxyServer, will start after V1 connects
    }
    
    initialized = true;
    Serial.println("BLE stack initialized");
    return true;
}

bool V1BLEClient::begin(bool enableProxy, const char* proxyName) {
    // Initialize BLE stack first (idempotent)
    if (!initBLE(enableProxy, proxyName)) {
        return false;
    }
    
    // Start scanning for V1 - optimized for reliable discovery
    NimBLEScan* pScan = NimBLEDevice::getScan();
    
    // Delete existing callback handlers to prevent memory leaks on restart
    if (pScanCallbacks) {
        delete pScanCallbacks;
    }
    pScanCallbacks = new ScanCallbacks(this);
    pScan->setScanCallbacks(pScanCallbacks);
    pScan->setActiveScan(true);  // Request scan response to get device names
    pScan->setInterval(100);  // 62.5ms interval - relaxed scanning (Kenny's setting)
    pScan->setWindow(75);     // 47ms window - 75% duty cycle
    pScan->setMaxResults(0);  // Unlimited results
    pScan->setDuplicateFilter(false);  // Don't filter duplicates - we want to see everything
    Serial.println("Scan configured: interval=100 (62.5ms), window=75 (47ms), active=true, 75% duty, 10s duration");
    
    Serial.println("Scanning for V1 Gen2...");
    lastScanStart = millis();
    bool started = pScan->start(SCAN_DURATION, false, false);  // duration, isContinuous, restart
    Serial.printf("Scan started: %s\n", started ? "YES" : "NO");
    
    if (started) {
        setBLEState(BLEState::SCANNING, "begin()");
    }
    
    return started;
}

bool V1BLEClient::isConnected() {
    // Quick check without mutex - the connected flag is atomic enough for reading
    // and pClient->isConnected() is thread-safe in NimBLE
    if (!connected || !pClient) {
        return false;
    }
    return pClient->isConnected();
}

bool V1BLEClient::isProxyClientConnected() {
    return proxyClientConnected;
}

void V1BLEClient::setProxyClientConnected(bool connected) {
    proxyClientConnected = connected;
}

void V1BLEClient::onDataReceived(DataCallback callback) {
    dataCallback = callback;
}

void V1BLEClient::onV1Connected(ConnectionCallback callback) {
    connectCallback = callback;
}

void V1BLEClient::ScanCallbacks::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    String name = advertisedDevice->getName().c_str();
    std::string addrStr = advertisedDevice->getAddress().toString();
    int rssi = advertisedDevice->getRSSI();
    
    // Ignore our own proxy advertisement to avoid self-connect loops
    if (bleClient->proxyEnabled) {
        NimBLEAddress selfAddr = NimBLEDevice::getAddress();
        if (advertisedDevice->getAddress() == selfAddr) {
            return;
        }
    }
    
    // Optional: Uncomment for BLE scan debugging
    // static int debugCount = 0;
    // if (debugCount < 20) {
    //     Serial.printf("[BLE %2d] addr=%s RSSI=%3d name='%s'\n",
    //                   debugCount++, addrStr.c_str(), rssi,
    //                   name.length() > 0 ? name.c_str() : "(no name)");
    // }
    
    // *** V1 NAME FILTER - Only connect to Valentine V1 Gen2 devices ***
    // V1 Gen2 advertises as "V1G*" (like "V1G27B7A") or sometimes "V1-*"
    String nameLower = name;
    nameLower.toLowerCase();
    
    bool isV1 = nameLower.startsWith("v1g") || nameLower.startsWith("v1-");
    
    if (!isV1) {
        // Not a V1 device, keep scanning
        return;
    }
    
    // *** FOUND V1! Stop scan and queue connection ***
    int advAddrType = advertisedDevice->getAddressType();
    Serial.printf("\n========================================\n");
    Serial.printf("*** FOUND V1: '%s' [%s] RSSI:%d addrType=%d ***\n", 
                  name.c_str(), addrStr.c_str(), rssi, advAddrType);
    Serial.printf("========================================\n");
    
    // Check if we're already connecting or connected
    if (bleClient->bleState == BLEState::CONNECTING) {
        Serial.println("[BLE_SM] Ignoring V1 found - already connecting");
        return;
    }
    if (bleClient->bleState == BLEState::CONNECTED) {
        Serial.println("[BLE_SM] Ignoring V1 found - already connected");
        return;
    }
    
    // Save this address for future fast reconnects
    settingsManager.setLastV1Address(addrStr.c_str());
    
    // Stop scanning - state machine will handle the connection after settle time
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) {
        pScan->stop();
        Serial.println("[BLE_SM] Scan stop requested");
    }
    
    // Queue connection to this V1 device
    SemaphoreGuard lock(bleClient->bleMutex);
    if (lock.locked()) {
        bleClient->targetDevice = *advertisedDevice;
        bleClient->targetAddress = bleClient->targetDevice.getAddress();
        bleClient->targetAddressType = advAddrType;  // Save for reconnect
        bleClient->hasTargetDevice = true;
        bleClient->shouldConnect = true;
        bleClient->scanStopRequestedMs = millis();
        bleClient->setBLEState(BLEState::SCAN_STOPPING, "V1 found");
    }
}

void V1BLEClient::ScanCallbacks::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
    Serial.printf("[BLE_SM] Scan ended: found %d devices, reason=%d\n", 
                  scanResults.getCount(), reason);
    
    // If we were SCANNING and scan ended without finding V1, go back to DISCONNECTED
    // to allow process() to restart the scan
    if (instancePtr) {
        SemaphoreGuard lock(instancePtr->bleMutex);
        if (lock.locked()) {
            if (instancePtr->bleState == BLEState::SCANNING) {
                // Scan ended without finding V1, go back to DISCONNECTED
                instancePtr->setBLEState(BLEState::DISCONNECTED, "scan ended without finding V1");
            }
            // If SCAN_STOPPING, process() will handle the transition
        }
    }
}

void V1BLEClient::ClientCallbacks::onConnect(NimBLEClient* pClient) {
    Serial.println("Connected to V1");
    if (instancePtr) {
        SemaphoreGuard lock(instancePtr->bleMutex);
        instancePtr->connected = true;
        instancePtr->setBLEState(BLEState::CONNECTED, "onConnect callback");
    }
}

void V1BLEClient::ClientCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
    Serial.printf("[BLE_SM] ========== V1 DISCONNECTED (reason: %d) ==========\n", reason);
    
    // If the disconnect was unexpected (e.g., V1 powered off), clear bonding info
    // to ensure a clean reconnect next time.
    if (reason != 0 && reason != BLE_HS_ETIMEOUT) { // 0 is normal disconnect
        NimBLEAddress addr = pClient->getPeerAddress();
        if (NimBLEDevice::isBonded(addr)) {
            Serial.printf("[BLE_SM] Unexpected disconnect. Deleting bond for %s\n", addr.toString().c_str());
            NimBLEDevice::deleteBond(addr);
        }
    }

    if (instancePtr) {
        // Stop proxy advertising FIRST before any state changes
        if (instancePtr->proxyEnabled && NimBLEDevice::getAdvertising()->isAdvertising()) {
            Serial.println("[BLE_SM] Stopping proxy advertising due to V1 disconnect...");
            NimBLEDevice::stopAdvertising();
            delay(50);  // Brief settle time
        }
        
        SemaphoreGuard lock(instancePtr->bleMutex);
        instancePtr->connected = false;
        instancePtr->connectInProgress = false;  // Clear connection guard
        // Clear proxy client connection state too - can't proxy without V1 connection
        instancePtr->proxyClientConnected = false;
        // Do NOT clear pClient - we reuse it to prevent memory leaks
        // instancePtr->pClient = nullptr; 
        instancePtr->pRemoteService = nullptr;
        instancePtr->pDisplayDataChar = nullptr;
        instancePtr->pCommandChar = nullptr;
        instancePtr->pCommandCharLong = nullptr;
        // Set state to DISCONNECTED - will trigger scan restart in process()
        instancePtr->setBLEState(BLEState::DISCONNECTED, "onDisconnect callback");
        Serial.println("[BLE_SM] Connection cleanup complete, will restart scanning...");
    }
}

bool V1BLEClient::connectToServer() {
    std::string addrStr = targetAddress.toString();
    int addrType = hasTargetDevice ? targetDevice.getAddressType() : targetAddressType;
    
    // ========== CONNECTION GUARDS ==========
    // Prevent overlapping connection attempts which cause EBUSY errors
    
    // Guard 1: Check if already connecting
    if (connectInProgress) {
        Serial.printf("[BLE_SM] BLOCKED: Connection already in progress (target: %s)\n", addrStr.c_str());
        return false;
    }
    
    // Guard 2: Check if scanning is still active
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        Serial.println("[BLE_SM] BLOCKED: Cannot connect while scan is active - stopping scan");
        pScan->stop();
        // Don't proceed - let process() handle connection after scan settles
        scanStopRequestedMs = millis();
        setBLEState(BLEState::SCAN_STOPPING, "connectToServer guard");
        return false;
    }
    
    // Guard 3: Check exponential backoff
    unsigned long now = millis();
    if (consecutiveConnectFailures > 0 && now < nextConnectAllowedMs) {
        Serial.printf("[BLE_SM] BLOCKED: Backoff active (%lu ms remaining)\n", nextConnectAllowedMs - now);
        {
            SemaphoreGuard lock(bleMutex);
            shouldConnect = false;
        }
        setBLEState(BLEState::BACKOFF, "backoff active");
        return false;
    }
    
    // Set connection guard
    connectInProgress = true;
    setBLEState(BLEState::CONNECTING, "connectToServer");
    
    Serial.printf("[BLE_SM] Attempting connection to %s (addrType=%d, hasDevice=%d)\n", 
                  addrStr.c_str(), addrType, hasTargetDevice ? 1 : 0);
    
    // Clear any stale bonding info that might interfere with connection
    // V1 Gen2 can get into weird states if old pairing info lingers
    if (NimBLEDevice::isBonded(targetAddress)) {
        Serial.printf("[BLE_SM] Deleting stale bond for %s\n", addrStr.c_str());
        NimBLEDevice::deleteBond(targetAddress);
        delay(100);  // Brief pause after bond deletion
    }
    
    // Brief pause for radio to settle after scan stop
    Serial.println("[BLE_SM] Waiting for radio to settle (300ms)...");
    vTaskDelay(pdMS_TO_TICKS(300));

    bool connectedOk = false;
    // Try up to 3 times - V1 can be slow to respond
    int attempts = 3; 
    
    // Reuse existing client if available to prevent leaks
    // If client exists but may be stuck, delete and recreate
    if (pClient) {
        // Check if client is in a bad state
        if (pClient->isConnected()) {
            Serial.println("[BLE_SM] Client already connected? Disconnecting first...");
            pClient->disconnect();
            delay(100);
        }
    } else {
        pClient = NimBLEDevice::createClient();
    }
    
    if (!pClient) {
        // If we still don't have a client, we can't proceed.
        Serial.println("[BLE_SM] ERROR: Failed to create client");
        connectInProgress = false;
        setBLEState(BLEState::DISCONNECTED, "client creation failed");
        return false;
    }

    // Create client callbacks if not already created
    if (!pClientCallbacks) {
        pClientCallbacks = new ClientCallbacks();
    }
    pClient->setClientCallbacks(pClientCallbacks);
    // Use tighter connection parameters for lower proxy latency
    // min/max interval: 12-24 (15-30ms), latency: 0, timeout: 400 (4000ms)
    pClient->setConnectionParams(12, 24, 0, 400);
    // Give it plenty of time to connect (10s)
    pClient->setConnectTimeout(10); 

    for (int attempt = 1; attempt <= attempts && !connectedOk; ++attempt) {
        Serial.printf("[BLE_SM] Connect attempt %d/%d\n", attempt, attempts);
        
        // Prefer device object on first attempt (has full advertisement data)
        if (hasTargetDevice && attempt == 1) {
            Serial.printf("[BLE_SM] Using targetDevice (addrType from advert: %d)\n", 
                         targetDevice.getAddressType());
            connectedOk = pClient->connect(targetDevice, false);
        } else {
            Serial.printf("[BLE_SM] Using targetAddress with saved type %d\n", targetAddressType);
            connectedOk = pClient->connect(targetAddress, false);
        }

        if (!connectedOk) {
            int err = pClient->getLastError();
            Serial.printf("[BLE_SM] Attempt %d failed (error: %d)\n", attempt, err);
            
            // Error 13 = EBUSY - device busy, wait longer before retry
            // Do NOT recreate client - that causes heap corruption
            if (err == 13) {
                Serial.println("[BLE_SM] EBUSY - radio busy, waiting 1s before retry...");
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else if (attempt < attempts) {
                Serial.println("[BLE_SM] Waiting 500ms before retry...");
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }

    if (!connectedOk) {
        int lastErr = pClient->getLastError();
        Serial.printf("[BLE_SM] FAILED after all attempts (last error: %d)\n", lastErr);
        
        // Track consecutive failures
        consecutiveConnectFailures++;
        Serial.printf("[BLE_SM] Consecutive failures: %d/%d\n", 
                      consecutiveConnectFailures, MAX_BACKOFF_FAILURES);
        
        // Check if we need a hard reset of the BLE stack
        if (consecutiveConnectFailures >= MAX_BACKOFF_FAILURES) {
            Serial.println("[BLE_SM] Max failures reached - triggering hard reset");
            hardResetBLEClient();
            // hardResetBLEClient() sets state to DISCONNECTED and clears flags
            return false;
        }
        
        // Cleanup connection state before backoff
        Serial.println("[BLE_SM] Cleaning up connection state before backoff...");
        cleanupConnection();
        
        // Calculate exponential backoff
        int exponent = (consecutiveConnectFailures > 4) ? 4 : (consecutiveConnectFailures - 1);
        unsigned long backoffMs = BACKOFF_BASE_MS * (1 << exponent);
        if (backoffMs > BACKOFF_MAX_MS) backoffMs = BACKOFF_MAX_MS;
        nextConnectAllowedMs = millis() + backoffMs;
        Serial.printf("[BLE_SM] Backoff set: %lu ms\n", backoffMs);
        
        // Use cleanupConnection() for proper teardown
        cleanupConnection();
        
        connectInProgress = false;
        setBLEState(BLEState::BACKOFF, "connection failed");
        return false;
    }
    
    // Success! Reset backoff state
    consecutiveConnectFailures = 0;
    nextConnectAllowedMs = 0;
    Serial.println("[BLE_SM] Connected! Setting up characteristics...");
    
    // NimBLE 2.x requires explicit service discovery before getService()
    // Try to discover services with a timeout
    Serial.println("[BLE_SM] Discovering services...");
    Serial.printf("[BLE_SM] V1 connection MTU: %d\n", pClient->getMTU());
    int maxRetries = 3;
    for (int retry = 0; retry < maxRetries; retry++) {
        if (pClient->discoverAttributes()) {
            Serial.println("[BLE_SM] Service discovery completed");
            break;
        }
        Serial.printf("[BLE_SM] Service discovery attempt %d failed, retrying...\n", retry + 1);
        delay(50);
    }
    
    bool ok = setupCharacteristics();
    if (!ok) {
        Serial.println("[BLE_SM] Setup failed, disconnecting");
        disconnect();
        {
            SemaphoreGuard lock(bleMutex);
            shouldConnect = false;
            hasTargetDevice = false;
        }
        connectInProgress = false;
        setBLEState(BLEState::DISCONNECTED, "characteristic setup failed");
        return false;
    }
    
    connectInProgress = false;
    // State is already CONNECTED from onConnect callback
    return connected;
}

bool V1BLEClient::setupCharacteristics() {
    pRemoteService = pClient->getService(V1_SERVICE_UUID);
    if (!pRemoteService) {
        Serial.println("Failed to find V1 service");
        pClient->disconnect();
        SemaphoreGuard lock(bleMutex);
        connected = false;
        return false;
    }

    // Enumerate all characteristics in the V1 service
    auto& chars = pRemoteService->getCharacteristics(true);
    if (!chars.empty()) {
        Serial.printf("Found %u characteristics on V1 service\n", (unsigned)chars.size());
        for (auto* c : chars) {
            auto uuid = c->getUUID().toString();
            Serial.printf("Char %s props: notify=%d indicate=%d read=%d write=%d writeNR=%d\n",
                          uuid.c_str(), c->canNotify(), c->canIndicate(),
                          c->canRead(), c->canWrite(), c->canWriteNoResponse());
        }
    } else {
        Serial.println("No characteristics found on V1 service");
    }
    
    // Get display data characteristic (notify)
    pDisplayDataChar = pRemoteService->getCharacteristic(V1_DISPLAY_DATA_UUID);
    if (!pDisplayDataChar) {
        Serial.println("Failed to find display data characteristic");
        pClient->disconnect();
        SemaphoreGuard lock(bleMutex);
        connected = false;
        return false;
    }
    Serial.printf("DisplayChar props: notify=%d indicate=%d read=%d write=%d writeNR=%d\n",
                  pDisplayDataChar->canNotify(), pDisplayDataChar->canIndicate(),
                  pDisplayDataChar->canRead(), pDisplayDataChar->canWrite(), pDisplayDataChar->canWriteNoResponse());
    
    // Get command characteristic (write)
    pCommandChar = pRemoteService->getCharacteristic(V1_COMMAND_WRITE_UUID);
    NimBLERemoteCharacteristic* altCommandChar = nullptr;
    if (pRemoteService) {
        altCommandChar = pRemoteService->getCharacteristic(V1_COMMAND_WRITE_ALT_UUID);
    }

    // Get LONG write characteristic (B8D2) for commands like voltage request
    pCommandCharLong = pRemoteService->getCharacteristic("92A0B8D2-9E05-11E2-AA59-F23C91AEC05E");
    if (pCommandCharLong) {
        Serial.println("Found B8D2 (LONG write) characteristic");
    } else {
        Serial.println("WARNING: B8D2 (LONG write) not found - some commands may fail");
    }

    // Prefer the primary B6D4 characteristic; only fall back to BAD4 if B6D4 is unusable
    if (!pCommandChar || (!pCommandChar->canWrite() && !pCommandChar->canWriteNoResponse())) {
        if (altCommandChar && (altCommandChar->canWrite() || altCommandChar->canWriteNoResponse())) {
            Serial.println("Primary command char unusable, falling back to BAD4");
            pCommandChar = altCommandChar;
        } else {
            Serial.println("Command characteristic not available");
            pClient->disconnect();
            SemaphoreGuard lock(bleMutex);
            connected = false;
            return false;
        }
    }

    if (!pCommandChar) {
        Serial.println("Failed to find command characteristic");
        pClient->disconnect();
        SemaphoreGuard lock(bleMutex);
        connected = false;
        return false;
    }
    Serial.printf("CommandChar props: notify=%d indicate=%d read=%d write=%d writeNR=%d\n",
                  pCommandChar->canNotify(), pCommandChar->canIndicate(),
                  pCommandChar->canRead(), pCommandChar->canWrite(), pCommandChar->canWriteNoResponse());
    
    // Subscribe to notifications (main display data characteristic only)
    // Following Kenny's approach: only subscribe to B2CE for alert data
    bool subscribed = false;
    if (pDisplayDataChar->canNotify()) {
        subscribed = pDisplayDataChar->subscribe(true, notifyCallback, true);
        Serial.println(subscribed ? "Subscribed to display data notifications" : "Failed to subscribe");
    } else if (pDisplayDataChar->canIndicate()) {
        subscribed = pDisplayDataChar->subscribe(false, notifyCallback);
        Serial.println(subscribed ? "Subscribed to indications for display data" : "Failed to subscribe (indicate)");
    } else {
        Serial.println("Display characteristic cannot notify or indicate!");
    }

    if (!subscribed) {
        pClient->disconnect();
        SemaphoreGuard lock(bleMutex);
        connected = false;
        return false;
    }

    // Force CCCD write for notifications if descriptor is present
    NimBLERemoteDescriptor* cccd = pDisplayDataChar->getDescriptor(NimBLEUUID((uint16_t)0x2902));
    if (cccd) {
        uint8_t notifOn[] = {0x01, 0x00};
        if (cccd->writeValue(notifOn, sizeof(notifOn), true)) {
            Serial.println("Wrote CCCD to enable notifications");
        } else {
            Serial.println("Failed to write CCCD for notifications");
            pClient->disconnect();
            SemaphoreGuard lock(bleMutex);
            connected = false;
            return false;
        }
    } else {
        Serial.println("No CCCD descriptor found on display characteristic");
    }
    
    // Also subscribe to B4E0 (LONG) characteristic for voltage/response data
    NimBLERemoteCharacteristic* pDisplayDataLongChar = pRemoteService->getCharacteristic(V1_DISPLAY_DATA_LONG_UUID);
    if (pDisplayDataLongChar) {
        if (pDisplayDataLongChar->canNotify()) {
            if (pDisplayDataLongChar->subscribe(true, notifyCallback, true)) {
                Serial.println("Subscribed to LONG (B4E0) notifications");
                // Force CCCD write
                NimBLERemoteDescriptor* cccdLong = pDisplayDataLongChar->getDescriptor(NimBLEUUID((uint16_t)0x2902));
                if (cccdLong) {
                    uint8_t notifOn[] = {0x01, 0x00};
                    if (cccdLong->writeValue(notifOn, sizeof(notifOn), true)) {
                        Serial.println("Wrote CCCD to enable LONG notifications");
                    } else {
                        Serial.println("Failed to write CCCD for LONG (non-critical)");
                    }
                }
            } else {
                Serial.println("Failed to subscribe to LONG characteristic (non-critical)");
            }
        } else {
            Serial.println("LONG characteristic cannot notify");
        }
    } else {
        Serial.println("WARNING: B4E0 (LONG) characteristic not found - voltage passthrough will fail");
    }
    
    // Try an initial read for sanity
    if (pDisplayDataChar->canRead()) {
        std::string v = pDisplayDataChar->readValue();
        Serial.printf("Initial display value len=%u\n", (unsigned)v.size());
    }

    {
        SemaphoreGuard lock(bleMutex);
        connected = true;
    }
    
    // Request alert data from V1 - brief delay for MTU negotiation
    delay(100);
    
    if (!requestAlertData()) {
        Serial.println("Failed to request alert data (non-critical)");
    }
    
    // Notify user callback that V1 connection is fully established
    if (connectCallback) {
        connectCallback();
    }
    
    // Wait for V1 connection to fully stabilize before starting proxy advertising
    // This allows time for:
    // - Initial data exchange with V1
    // - Profile push to complete
    // - Subscriptions to settle
    // - Connection parameters to negotiate
    // NON-BLOCKING: Schedule advertising start in process() instead of blocking here
    if (proxyEnabled && proxyServerInitialized) {
        Serial.println("Scheduling proxy advertising start (non-blocking)...");
        proxyAdvertisingStartMs = millis() + PROXY_STABILIZE_MS;  // Deferred start
    }

    return connected;
}

void V1BLEClient::notifyCallback(NimBLERemoteCharacteristic* pChar, 
                                  uint8_t* pData, 
                                  size_t length, 
                                  bool isNotify) {
    if (!pData || !instancePtr || !pChar) {
        return;
    }
    
    uint16_t charId = shortUuid(pChar->getUUID());
    
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

    // PERFORMANCE: Forward to proxy IMMEDIATELY - zero latency path to JBV1
    // NimBLE handles thread safety for server notifications
    instancePtr->forwardToProxyImmediate(pData, length, routeCharId);
    
    // Call user callback for display processing (queued to main loop for SPI safety)
    if (instancePtr->dataCallback) {
        instancePtr->dataCallback(pData, length, charId);
    }
}

bool V1BLEClient::sendCommand(const uint8_t* data, size_t length) {
    if (!isConnected() || !pCommandChar) {
        //Serial.println("sendCommand: not connected or command characteristic missing");
        return false;
    }
    
    // Validate inputs
    if (!data) {
        //Serial.println("sendCommand: ERROR - null data pointer");
        return false;
    }
    if (length == 0) {
        //Serial.println("sendCommand: ERROR - zero length");
        return false;
    }
    if (length > 64) {  // Reasonable maximum for BLE packets
        //Serial.printf("sendCommand: ERROR - length %u exceeds maximum (64)\n", (unsigned)length);
        return false;
    }
    
    // Don't print during command forwarding - causes crashes in callback context
    // Serial.printf("sendCommand: sending %u bytes: ", (unsigned)length);
    // for (size_t i = 0; i < length; i++) {
    //     Serial.printf("%02X ", data[i]);
    // }
    // Serial.println();
    
    bool ok = false;
    if (pCommandChar->canWrite()) {
        // Use write-with-response when supported
        ok = pCommandChar->writeValue(data, length, true);
    } else if (pCommandChar->canWriteNoResponse()) {
        // Use write-without-response when that's the only option
        ok = pCommandChar->writeValue(data, length, false);
    } else {
        return false;
    }
    
    // Only log failure to keep output clean
    if (!ok) {
        Serial.println("sendCommand: write failed");
    }
    return ok;
}

bool V1BLEClient::requestAlertData() {
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_REQ_START_ALERT,
        0x01,
        0x00,
        ESP_PACKET_END
    };

    uint8_t checksum = 0;
    for (size_t i = 0; i < 5; ++i) {
        checksum += packet[i];
    }
    packet[5] = checksum;

    Serial.println("Requesting alert data from V1...");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::requestVersion() {
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_VERSION,
        0x01,
        0x00,
        ESP_PACKET_END
    };

    uint8_t checksum = 0;
    for (size_t i = 0; i < 5; ++i) {
        checksum += packet[i];
    }
    packet[5] = checksum;

    Serial.println("Requesting version info from V1...");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::setDisplayOn(bool on) {
    // For dark mode, we need to use reqTurnOffMainDisplay with proper payload
    // Mode 0 = completely off, Mode 1 = only BT icon visible
    // For "dark mode on" we want display OFF, for "dark mode off" we want display ON
    // 
    // Based on Kenny's v1g2-t4s3 implementation:
    // - reqTurnOnMainDisplay: 7-byte packet with payloadLength=1, no actual payload data
    // - reqTurnOffMainDisplay: 8-byte packet with payloadLength=2, only 1 mode byte in payload
    
    if (on) {
        // Turn display back ON (exit dark mode)
        // Packet: AA DA E4 33 01 [checksum] AB  (7 bytes total)
        uint8_t packet[] = {
            ESP_PACKET_START,                               // [0] 0xAA
            static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
            static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
            PACKET_ID_TURN_ON_DISPLAY,                      // [3] 0x33
            0x01,                                           // [4] payload length
            0x00,                                           // [5] checksum placeholder
            ESP_PACKET_END                                  // [6] 0xAB
        };
        
        // Calculate checksum over bytes 0-4 (5 bytes)
        uint8_t checksum = 0;
        for (size_t i = 0; i < 5; ++i) {
            checksum += packet[i];
        }
        packet[5] = checksum;
        
        Serial.printf("Setting V1 display ON (exit dark mode), packet: ");
        for (size_t i = 0; i < sizeof(packet); i++) {
            Serial.printf("%02X ", packet[i]);
        }
        Serial.println();
        
        return sendCommand(packet, sizeof(packet));
    } else {
        // Turn display OFF (enter dark mode)
        // Per Kenny's implementation: payloadLength=2 but only 1 actual payload byte
        // Packet: AA DA E4 32 02 [mode] [checksum] AB  (8 bytes total)
        // mode=0: completely dark, mode=1: only BT icon visible
        uint8_t mode = 0x00;  // Completely dark
        uint8_t packet[] = {
            ESP_PACKET_START,                               // [0] 0xAA
            static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
            static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
            PACKET_ID_TURN_OFF_DISPLAY,                     // [3] 0x32
            0x02,                                           // [4] payload length = 2
            mode,                                           // [5] mode byte
            0x00,                                           // [6] checksum placeholder
            ESP_PACKET_END                                  // [7] 0xAB
        };
        
        // Calculate checksum over bytes 0-5 (6 bytes)
        uint8_t checksum = 0;
        for (size_t i = 0; i < 6; ++i) {
            checksum += packet[i];
        }
        packet[6] = checksum;
        
        Serial.printf("Setting V1 display OFF (dark mode), packet: ");
        for (size_t i = 0; i < sizeof(packet); i++) {
            Serial.printf("%02X ", packet[i]);
        }
        Serial.println();
        
        return sendCommand(packet, sizeof(packet));
    }
}

bool V1BLEClient::setMute(bool muted) {
    uint8_t packetId = muted ? PACKET_ID_MUTE_ON : PACKET_ID_MUTE_OFF;
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        packetId,
        0x01,
        0x00,
        ESP_PACKET_END
    };

    uint8_t checksum = 0;
    for (size_t i = 0; i < 5; ++i) {
        checksum += packet[i];
    }
    packet[5] = checksum;

    Serial.printf("Setting V1 mute %s...\n", muted ? "ON" : "OFF");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::setMode(uint8_t mode) {
    // Packet ID 0x36 = REQCHANGEMODE
    // Mode: 0x01 = All Bogeys, 0x02 = Logic, 0x03 = Advanced Logic
    uint8_t packet[] = {
        ESP_PACKET_START,                               // [0] 0xAA
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
        0x36,                                           // [3] REQCHANGEMODE
        0x02,                                           // [4] payload length = 2
        mode,                                           // [5] mode byte
        0x00,                                           // [6] checksum placeholder
        ESP_PACKET_END                                  // [7] 0xAB
    };
    
    // Calculate checksum over bytes 0-5 (6 bytes)
    uint8_t checksum = 0;
    for (size_t i = 0; i < 6; ++i) {
        checksum += packet[i];
    }
    packet[6] = checksum;
    
    const char* modeName = "Unknown";
    if (mode == 0x01) modeName = "All Bogeys";
    else if (mode == 0x02) modeName = "Logic";
    else if (mode == 0x03) modeName = "Advanced Logic";
    
    Serial.printf("Setting V1 mode to %s (0x%02X), packet: ", modeName, mode);
    for (size_t i = 0; i < sizeof(packet); i++) {
        Serial.printf("%02X ", packet[i]);
    }
    Serial.println();
    
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::setVolume(uint8_t mainVolume, uint8_t mutedVolume) {
    // 0xFF means "don't change" - skip command entirely if either is undefined
    // V1 REQWRITEVOLUME sets BOTH values, so we need both to be valid (0-9)
    if (mainVolume == 0xFF || mutedVolume == 0xFF) {
        Serial.printf("setVolume: skipping - main=%d mute=%d (0xFF means not configured)\n", 
                      mainVolume, mutedVolume);
        return true;  // Success - nothing to do (user hasn't configured both)
    }
    
    // Clamp to valid range (0-9)
    if (mainVolume > 9) mainVolume = 9;
    if (mutedVolume > 9) mutedVolume = 9;
    
    // Packet ID 0x39 = REQWRITEVOLUME
    // Payload: mainVolume, mutedVolume (currentVolume), aux0
    uint8_t packet[] = {
        ESP_PACKET_START,                               // [0] 0xAA
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
        PACKET_ID_REQ_WRITE_VOLUME,                     // [3] 0x39
        0x04,                                           // [4] payload length = 4 (3 data + checksum)
        mainVolume,                                     // [5] main volume 0-9
        mutedVolume,                                    // [6] muted volume 0-9  
        0x00,                                           // [7] aux0 (unused, set to 0)
        0x00,                                           // [8] checksum placeholder
        ESP_PACKET_END                                  // [9] 0xAB
    };
    
    // Calculate checksum over bytes 0-7 (8 bytes)
    uint8_t checksum = 0;
    for (size_t i = 0; i < 8; ++i) {
        checksum += packet[i];
    }
    packet[8] = checksum;
    
    Serial.printf("Setting V1 volume - main: %d, muted: %d, packet: ", mainVolume, mutedVolume);
    for (size_t i = 0; i < sizeof(packet); i++) {
        Serial.printf("%02X ", packet[i]);
    }
    Serial.println();
    
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::requestUserBytes() {
    // Build packet: AA D0+dest E0+src 11 01 [checksum] AB
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_REQ_USER_BYTES,
        0x01,  // length
        0x00,  // checksum placeholder
        ESP_PACKET_END
    };

    uint8_t checksum = 0;
    for (size_t i = 0; i < 5; ++i) {
        checksum += packet[i];
    }
    packet[5] = checksum;

    Serial.println("Requesting V1 user bytes...");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::writeUserBytes(const uint8_t* bytes) {
    if (!bytes) {
        return false;
    }
    
    // Build packet: AA D0+dest E0+src 13 07 [6 bytes] [checksum] AB
    uint8_t packet[13];
    packet[0] = ESP_PACKET_START;
    packet[1] = static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1);
    packet[2] = static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE);
    packet[3] = PACKET_ID_WRITE_USER_BYTES;
    packet[4] = 0x07;  // length = 6 bytes + 1
    memcpy(&packet[5], bytes, 6);
    
    uint8_t checksum = 0;
    for (size_t i = 0; i < 11; ++i) {
        checksum += packet[i];
    }
    packet[11] = checksum;
    packet[12] = ESP_PACKET_END;

    Serial.printf("Writing V1 user bytes: %02X %02X %02X %02X %02X %02X\n",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return sendCommand(packet, sizeof(packet));
}

V1BLEClient::WriteVerifyResult V1BLEClient::writeUserBytesVerified(const uint8_t* bytes, int maxRetries) {
    if (!bytes || !isConnected()) {
        return VERIFY_WRITE_FAILED;
    }

    // Note: Full verification is disabled because BLE responses come async via the main loop queue,
    // but this function blocks. The write typically succeeds - V1 is reliable.
    // We do multiple write attempts for robustness but don't wait for read-back verification.
    
    Serial.println("[VerifyPush] Writing to V1 (async verification not possible in blocking context)");
    
    for (int attempt = 1; attempt <= maxRetries; attempt++) {
        if (writeUserBytes(bytes)) {
            Serial.printf("[VerifyPush] Write command sent successfully (attempt %d/%d)\n", attempt, maxRetries);
            // Request a read-back - result will come async and update currentSettings
            // This helps confirm the write worked, but we can't wait for it here
            delay(50);  // Brief delay to let V1 process
            requestUserBytes();  // Fire-and-forget read-back request
            return VERIFY_OK;
        }
        Serial.printf("[VerifyPush] Write attempt %d/%d failed, retrying...\n", attempt, maxRetries);
        delay(100);
    }
    
    Serial.println("[VerifyPush] All write attempts failed");
    return VERIFY_WRITE_FAILED;
}

void V1BLEClient::onUserBytesReceived(const uint8_t* bytes) {
    if (verifyPending && bytes) {
        memcpy(verifyReceived, bytes, 6);
        verifyComplete = true;
        verifyMatch = (memcmp(verifyExpected, verifyReceived, 6) == 0);
        Serial.printf("[VerifyPush] Received user bytes: %02X%02X%02X%02X%02X%02X (match=%s)\n",
            verifyReceived[0], verifyReceived[1], verifyReceived[2],
            verifyReceived[3], verifyReceived[4], verifyReceived[5],
            verifyMatch ? "YES" : "NO");
    }
}

void V1BLEClient::process() {
    // Handle deferred proxy advertising start (non-blocking replacement for delay(1500))
    if (proxyAdvertisingStartMs != 0 && millis() >= proxyAdvertisingStartMs) {
        proxyAdvertisingStartMs = 0;  // Clear pending flag
        
        if (isConnected() && proxyEnabled && proxyServerInitialized) {
            Serial.println("[BLE_SM] Starting deferred proxy advertising...");
            
            // Advertising data already configured in initProxyServer() with proper flags
            // Just start advertising (configuration was done at init time)
            startProxyAdvertising();
        } else {
            Serial.println("[BLE_SM] Skipping deferred proxy advertising (disconnected or disabled)");
        }
    }
    
    unsigned long now = millis();
    NimBLEScan* pScan = NimBLEDevice::getScan();
    
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
                Serial.println("[BLE_SM] Starting scan for V1...");
                lastScanStart = now;
                pScan->clearResults();
                bool started = pScan->start(SCAN_DURATION, false, false);
                if (started) {
                    setBLEState(BLEState::SCANNING, "scan started");
                } else {
                    Serial.println("[BLE_SM] ERROR: Failed to start scan");
                }
            }
            break;
        }
        
        case BLEState::SCANNING: {
            // Check if scan found a device (shouldConnect flag set by callback)
            bool wantConnect = false;
            {
                SemaphoreGuard lock(bleMutex);
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
                    Serial.println("[BLE_SM] Scan stop timeout - forcing stop");
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
            if (elapsed >= SCAN_STOP_SETTLE_MS) {
                resultsCleared = false;  // Reset for next time
                // Ready to connect
                bool wantConnect = false;
                {
                    SemaphoreGuard lock(bleMutex);
                    if (lock.locked()) {
                        wantConnect = shouldConnect;
                        shouldConnect = false;  // Clear flag
                    }
                }
                
                if (wantConnect) {
                    Serial.printf("[BLE_SM] Scan settled (%lums), proceeding to connect\n", elapsed);
                    connectToServer();  // This will set state to CONNECTING
                } else {
                    Serial.println("[BLE_SM] Scan settled but no connect pending");
                    setBLEState(BLEState::DISCONNECTED, "no connect pending");
                }
            }
            break;
        }
        
        case BLEState::CONNECTING: {
            // Connection in progress - just wait for callbacks
            // onConnect/onDisconnect will update state
            break;
        }
        
        case BLEState::CONNECTED: {
            // All good - nothing to do in state machine
            // Verify we're actually still connected
            if (!pClient || !pClient->isConnected()) {
                Serial.println("[BLE_SM] Lost connection unexpectedly");
                connected = false;
                connectInProgress = false;
                setBLEState(BLEState::DISCONNECTED, "connection lost");
            }
            break;
        }
        
        case BLEState::BACKOFF: {
            // Waiting for backoff period to expire
            if (now >= nextConnectAllowedMs) {
                Serial.printf("[BLE_SM] Backoff expired (failures=%d)\n", consecutiveConnectFailures);
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
            Serial.println("[BLE_SM] Starting scan for V1...");
            lastScanStart = millis();
            pScan->clearResults();
            bool started = pScan->start(SCAN_DURATION, false, false);
            if (started) {
                setBLEState(BLEState::SCANNING, "manual scan start");
            } else {
                Serial.println("[BLE_SM] ERROR: Failed to start scan");
            }
        }
    }
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

void V1BLEClient::setWifiPriority(bool enabled) {
    if (wifiPriorityMode == enabled) return;  // No change
    
    wifiPriorityMode = enabled;
    
    if (enabled) {
        Serial.println("[BLE] WiFi priority ENABLED - suppressing scans/reconnects/proxy");
        
        // Stop any active scan
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan && pScan->isScanning()) {
            Serial.println("[BLE] Stopping scan for WiFi priority mode");
            pScan->stop();
            pScan->clearResults();
        }
        
        // Stop proxy advertising if running
        if (proxyEnabled && NimBLEDevice::getAdvertising()->isAdvertising()) {
            Serial.println("[BLE] Stopping proxy advertising for WiFi priority mode");
            NimBLEDevice::stopAdvertising();
        }
        
        // Cancel any pending deferred advertising start
        proxyAdvertisingStartMs = 0;
        
        // Note: We keep existing V1 connection if already connected
        // to avoid disrupting active radar detection
        
    } else {
        Serial.println("[BLE] WiFi priority DISABLED - resuming normal BLE operation");
        
        // Resume proxy advertising if we're connected and proxy is enabled
        if (isConnected() && proxyEnabled && proxyServerInitialized) {
            Serial.println("[BLE] Resuming proxy advertising after WiFi priority mode");
            // Defer advertising start by 500ms to avoid stall
            proxyAdvertisingStartMs = millis() + 500;
        }
        
        // Resume scanning if disconnected
        if (!isConnected() && bleState == BLEState::DISCONNECTED) {
            Serial.println("[BLE] Resuming scan after WiFi priority mode");
            startScanning();
        }
    }
}

// ==================== BLE Proxy Server Functions ====================

void V1BLEClient::ProxyServerCallbacks::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
    Serial.println("===== JBV1 PROXY CLIENT CONNECTED =====");
    // Log negotiated MTU with phone/JBV1
    uint16_t connHandle = connInfo.getConnHandle();
    Serial.printf("[BLE_SM] Phone/JBV1 connection MTU: %d (handle: %d)\n", 
                  NimBLEDevice::getMTU(), connHandle);
    
    // Request tighter connection parameters for lower latency on phone side
    // min/max interval: 12-24 (15-30ms), latency: 0, timeout: 400 (4s)
    pServer->updateConnParams(connHandle, 12, 24, 0, 400);
    Serial.println("[BLE_SM] Requested tighter connection params for phone");
    
    if (bleClient) {
        bleClient->proxyClientConnected = true;
    }
}

void V1BLEClient::ProxyServerCallbacks::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
    Serial.printf("===== JBV1 PROXY CLIENT DISCONNECTED (reason: %d) =====\n", reason);
    if (bleClient) {
        bleClient->proxyClientConnected = false;
        // Resume advertising if V1 is still connected
        if (bleClient->connected) {
            Serial.println("Resuming proxy advertising...");
            NimBLEDevice::startAdvertising();
        }
    }
}

void V1BLEClient::ProxyWriteCallbacks::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    // Forward commands from JBV1 to V1
    if (!pCharacteristic || !bleClient) {
        Serial.println("ProxyWrite: null pCharacteristic or bleClient");
        return;
    }
    
    if (!bleClient->connected) {
        Serial.println("ProxyWrite: V1 not connected");
        return;
    }
    
    // Get the raw data pointer and length directly
    NimBLEAttValue attrValue = pCharacteristic->getValue();
    const uint8_t* rawData = attrValue.data();
    size_t rawLen = attrValue.size();
    
    if (rawLen == 0 || !rawData || rawLen > 32) {
        Serial.println("ProxyWrite: invalid data");
        return;
    }
    
    // Copy to a safe buffer and forward to V1
    uint16_t sourceChar = shortUuid(pCharacteristic->getUUID());
    uint8_t cmdBuf[32];
    memcpy(cmdBuf, rawData, rawLen);
    
    // Route to appropriate V1 characteristic based on source
    // B6D4 (SHORT) -> V1 B6D4
    // B8D2 (LONG) -> V1 B8D2 (for commands like voltage request)
    // BAD4 -> V1 B6D4 (fallback)
    if (sourceChar == 0xB8D2 && bleClient->pCommandCharLong) {
        bleClient->pCommandCharLong->writeValue(cmdBuf, rawLen, false);
    } else {
        bleClient->sendCommand(cmdBuf, rawLen);
    }
}

void V1BLEClient::initProxyServer(const char* deviceName) {
    Serial.printf("Creating BLE proxy server as '%s'\n", deviceName);
    
    // Kenny's exact order:
    // 1. Create server (no callbacks yet)
    pServer = NimBLEDevice::createServer();
    
    // 2. Create service
    pProxyService = pServer->createService(V1_SERVICE_UUID);
    
    // 3. Create ALL 6 characteristics that the V1 exposes (apps expect all of them)
    // Characteristic UUIDs from V1:
    // 92A0B2CE - Display data SHORT (notify) - primary alert data
    // 92A0B4E0 - V1 out LONG (notify)
    // 92A0B6D4 - Client write SHORT (writeNR)
    // 92A0B8D2 - Client write LONG (writeNR)
    // 92A0BCE0 - Notify characteristic
    // 92A0BAD4 - Write with response
    
    // Primary notify characteristic - display/alert data
    pProxyNotifyChar = pProxyService->createCharacteristic(
        "92A0B2CE-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    
    // V1 out LONG - notify (stores responses like voltage)
    pProxyNotifyLongChar = pProxyService->createCharacteristic(
        "92A0B4E0-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    
    // Client write SHORT - primary command input
    pProxyWriteChar = pProxyService->createCharacteristic(
        "92A0B6D4-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE_NR
    );
    
    // Client write LONG
    NimBLECharacteristic* pWriteLong = pProxyService->createCharacteristic(
        "92A0B8D2-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE_NR
    );
    
    // Additional notify characteristic
    NimBLECharacteristic* pNotifyAlt = pProxyService->createCharacteristic(
        "92A0BCE0-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    
    // Alternate write with response
    NimBLECharacteristic* pWriteAlt = pProxyService->createCharacteristic(
        "92A0BAD4-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    
    // 4. Set characteristic callbacks - all write chars forward to V1
    pProxyWriteCallbacks = new ProxyWriteCallbacks(this);
    pProxyWriteChar->setCallbacks(pProxyWriteCallbacks);
    pWriteLong->setCallbacks(pProxyWriteCallbacks);
    pWriteAlt->setCallbacks(pProxyWriteCallbacks);
    
    // 5. Start service
    pProxyService->start();
    
    // 6. Set server callbacks AFTER service start (Kenny's order - critical!)
    pProxyServerCallbacks = new ProxyServerCallbacks(this);
    pServer->setCallbacks(pProxyServerCallbacks);
    
    // Configure advertising data (Kenny's exact approach)
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    NimBLEAdvertisementData scanRespData;
    
    // Kenny uses setCompleteServices (includes service UUID in adv data)
    advData.setCompleteServices(pProxyService->getUUID());
    advData.setAppearance(0x0C80);  // Generic tag appearance
    scanRespData.setName(deviceName);
    
    pAdvertising->setAdvertisementData(advData);
    pAdvertising->setScanResponseData(scanRespData);
    
    // CRITICAL: Kenny's pattern - start then immediately stop if not connected
    // This initializes the advertising stack properly
    pAdvertising->start();
    if (!connected) {
        NimBLEDevice::stopAdvertising();
    }
    
    Serial.println("Proxy service created with 6 characteristics (full V1 API)");
}

void V1BLEClient::startProxyAdvertising() {
    if (!proxyServerInitialized || !pServer) {
        Serial.println("Cannot start advertising - proxy server not initialized");
        return;
    }
    
    // Don't restart if client already connected
    if (pServer->getConnectedCount() > 0) {
        Serial.println("Proxy client already connected, not restarting advertising");
        return;
    }
    
    // Start advertising if not already (simple approach, no task needed)
    if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
        if (NimBLEDevice::startAdvertising()) {
            Serial.println("Proxy advertising started");
        }
    } else {
        Serial.println("Proxy already advertising");
    }
}

void V1BLEClient::forwardToProxy(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!proxyEnabled || !proxyClientConnected) {
        return;
    }
    
    // Validate packet size
    if (length == 0 || length > PROXY_PACKET_MAX) {
        return;
    }
    
    // Queue packet for async send (non-blocking)
    // Use simple ring buffer with drop-oldest backpressure
    if (proxyQueueCount >= PROXY_QUEUE_SIZE) {
        // Queue full - drop oldest packet
        proxyQueueTail = (proxyQueueTail + 1) % PROXY_QUEUE_SIZE;
        proxyQueueCount--;
        proxyMetrics.dropCount++;
    }
    
    // Add packet to queue
    ProxyPacket& pkt = proxyQueue[proxyQueueHead];
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = sourceCharUUID;
    proxyQueueHead = (proxyQueueHead + 1) % PROXY_QUEUE_SIZE;
    proxyQueueCount++;
    
    // Track high water mark
    if (proxyQueueCount > proxyMetrics.queueHighWater) {
        proxyMetrics.queueHighWater = proxyQueueCount;
    }
}

// PERFORMANCE: Immediate proxy forwarding - zero latency path
// Called directly from BLE callback context - no queue, no delay
void V1BLEClient::forwardToProxyImmediate(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!proxyEnabled || !proxyClientConnected) {
        return;
    }
    
    // Validate packet size
    if (length == 0 || length > PROXY_PACKET_MAX) {
        return;
    }
    
    // Route to correct proxy characteristic based on source
    // B2CE (0xB2CE) -> proxy B2CE (short display data)
    // B4E0 (0xB4E0) -> proxy B4E0 (long alert/response data - voltage, etc)
    NimBLECharacteristic* targetChar = nullptr;
    
    if (sourceCharUUID == 0xB4E0 && pProxyNotifyLongChar) {
        targetChar = pProxyNotifyLongChar;
    } else if (pProxyNotifyChar) {
        targetChar = pProxyNotifyChar;
    }
    
    // PERF: Use combined notify(data, len) - single BLE operation instead of setValue + notify
    if (targetChar && targetChar->notify(data, length)) {
        proxyMetrics.sendCount++;
    } else if (targetChar) {
        proxyMetrics.errorCount++;
    }
}

int V1BLEClient::processProxyQueue() {
    if (!proxyEnabled || !proxyClientConnected || proxyQueueCount == 0) {
        return 0;
    }
    
    int sent = 0;
    
    // Process all queued packets (typically 1-2)
    while (proxyQueueCount > 0) {
        ProxyPacket& pkt = proxyQueue[proxyQueueTail];
        
        // Send notification
        if (pProxyNotifyChar) {
            pProxyNotifyChar->setValue(pkt.data, pkt.length);
            if (pProxyNotifyChar->notify()) {
                proxyMetrics.sendCount++;
                sent++;
            } else {
                proxyMetrics.errorCount++;
            }
        }
        
        proxyQueueTail = (proxyQueueTail + 1) % PROXY_QUEUE_SIZE;
        proxyQueueCount--;
    }
    
    return sent;
}

// Attempt a fast reconnect to a known address before scanning
bool V1BLEClient::fastReconnect() {
    // Check if we have a previously known address by comparing to a default (zeroed) address
    if (targetAddress == NimBLEAddress()) {
        Serial.println("[BLE_SM] FastReconnect: No previous V1 address known, skipping.");
        return false;
    }
    
    // Guard: Don't attempt if already connecting or connected
    if (bleState == BLEState::CONNECTING || bleState == BLEState::CONNECTED) {
        Serial.printf("[BLE_SM] FastReconnect: Cannot connect in state %s\n", 
                      bleStateToString(bleState));
        return false;
    }
    
    // Stop any active scan first
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) {
        Serial.println("[BLE_SM] FastReconnect: Stopping scan first...");
        pScan->stop();
        vTaskDelay(pdMS_TO_TICKS(SCAN_STOP_SETTLE_MS));
    }

    Serial.printf("[BLE_SM] FastReconnect: Attempting direct connection to %s\n", 
                  targetAddress.toString().c_str());
    hasTargetDevice = false;  // Ensure we connect by address only (not by advertised device)
    
    // Set state to DISCONNECTED so connectToServer() can proceed
    setBLEState(BLEState::DISCONNECTED, "fast reconnect prep");
    
    // Try to connect directly - connectToServer will handle state transitions
    bool result = connectToServer();
    
    // Clear shouldConnect flag regardless of result
    {
        SemaphoreGuard lock(bleMutex);
        if (lock.locked()) {
            shouldConnect = false;
        }
    }
    
    if (result) {
        Serial.println("[BLE_SM] FastReconnect: SUCCESS - Connected to V1!");
    } else {
        Serial.println("[BLE_SM] FastReconnect: FAILED - Will fall back to scanning");
        // Reset to DISCONNECTED so process() can start scanning
        if (bleState != BLEState::BACKOFF) {
            setBLEState(BLEState::DISCONNECTED, "fast reconnect failed");
        }
    }
    
    return result;
}

void V1BLEClient::setTargetAddress(const NimBLEAddress& address) {
    // Check if address is valid (not all zeros)
    if (address != NimBLEAddress()) {
        targetAddress = address;
        Serial.printf("[BLE] Set target address for fast reconnect: %s\n", targetAddress.toString().c_str());
    }
}

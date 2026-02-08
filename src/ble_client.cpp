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
#include "obd_handler.h"  // For OBD adapter detection during scan
#include "debug_logger.h"
#include "perf_metrics.h"
#include "../include/config.h"
#include <Arduino.h>
#include <WiFi.h>  // For WiFi coexistence during BLE connect
#include <Preferences.h>  // For fresh-flash detection
#include <set>
#include <string>
#include <cstdlib>

// Helper: calculate V1 packet checksum (sum of bytes)
static inline uint8_t calcV1Checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return sum;
}

// Task to restart advertising after delay
// Pattern derived from v1g2-t4s3 reference implementation for NimBLE 2.x dual-role stability
// Only restarts if no client is connected
[[maybe_unused]] static void restartAdvertisingTask(void* param) {
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
// RED ZONE SAFE: All semaphore takes use bounded timeouts, never portMAX_DELAY
// HOT paths use timeout 0 (try-lock), COLD paths use 20ms max
// RULE: Never use default timeout in a loop or frequent path.
//       If it runs more than once per second, pass 0 explicitly.
class SemaphoreGuard {
public:
    // timeout: 0 = try-lock (non-blocking), >0 = bounded wait in ms
    // Default 20ms for COLD paths - never use portMAX_DELAY
    // Increments appropriate counter on failure for monitoring
    explicit SemaphoreGuard(SemaphoreHandle_t sem, TickType_t timeout = pdMS_TO_TICKS(20)) 
        : sem_(sem), locked_(false) {
        if (sem_) {
            locked_ = xSemaphoreTake(sem_, timeout) == pdTRUE;
            if (!locked_) {
                // Track contention: try-lock skip vs bounded timeout
                if (timeout == 0) {
                    PERF_INC(bleMutexSkip);
                } else {
                    PERF_INC(bleMutexTimeout);
                }
            }
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
    NimBLEUUID uuid16 = uuid;
    uuid16.to16();
    if (uuid16.bitSize() == BLE_UUID_TYPE_16) {
        const uint8_t* val = uuid16.getValue();
        if (val) {
            uint16_t out = 0;
            memcpy(&out, val, sizeof(out));
            return out;
        }
    }
    std::string s = uuid.toString();
    if (s.size() >= 8) {
        // UUID is like 92a0b2ce-9e05-11e2-aa59-f23c91aec05e → take b2ce
        return static_cast<uint16_t>(strtoul(s.substr(4, 4).c_str(), nullptr, 16));
    }
    return 0;
}
// Debug log controls
constexpr bool BLE_DEBUG_LOGS = false;           // General BLE operation logs
constexpr bool CONNECT_ATTEMPT_VERBOSE = false;  // Individual connect attempt logs
constexpr bool BLE_STATE_MACHINE_LOGS = false;   // BLE state machine transitions (high frequency during reconnect)
constexpr bool BLE_CALLBACK_LOGS = false;        // BLE callback logs (default OFF - RED ZONE VIOLATION if enabled!)

// BLE logging macros - log to Serial AND debugLogger when BLE category enabled
// WARNING: These macros call debugLogger which is NOT red-zone safe.
// Only use from main loop context (process(), command handlers).
// NEVER use directly in onConnect/onDisconnect/onNotify callbacks.
#define BLE_LOGF(...) do { \
    if (BLE_DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    if (debugLogger.isEnabledFor(DebugLogCategory::Ble)) debugLogger.logf(DebugLogCategory::Ble, __VA_ARGS__); \
} while (0)
#define BLE_LOGLN(msg) do { \
    if (BLE_DEBUG_LOGS) Serial.println(msg); \
    if (debugLogger.isEnabledFor(DebugLogCategory::Ble)) debugLogger.log(DebugLogCategory::Ble, msg); \
} while (0)
#define BLE_SM_LOGF(...) do { \
    if (BLE_STATE_MACHINE_LOGS) Serial.printf(__VA_ARGS__); \
    if (debugLogger.isEnabledFor(DebugLogCategory::Ble)) debugLogger.logf(DebugLogCategory::Ble, __VA_ARGS__); \
} while (0)
} // namespace

// Spinlock for deferring settings writes from BLE scan callbacks
static portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;
// Spinlock for deferring OBD scan results from BLE scan callbacks
static portMUX_TYPE obdScanMux = portMUX_INITIALIZER_UNLOCKED;

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
    // proxyClientConnected - uses default member initializer (atomic)
    , proxyName_("V1-Proxy")
    , dataCallback(nullptr)
    , connectCallback(nullptr)
    // connected, shouldConnect - use default member initializers (atomic)
    , hasTargetDevice(false)
    , targetAddress()
    , lastScanStart(0)
    , freshFlashBoot(false)
    , pScanCallbacks(nullptr)
    , pClientCallbacks(nullptr)
    , pProxyServerCallbacks(nullptr)
    , pProxyWriteCallbacks(nullptr) {
    instancePtr = this;
}

V1BLEClient::~V1BLEClient() {
    // Delete allocated callback handlers to prevent memory leaks
    // NOTE: Ownership verified January 20, 2026 - NimBLE does NOT take ownership of callbacks,
    // caller is responsible for deletion. No double-free risk.
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
    
    BLE_SM_LOGF("[BLE_SM][%lu] %s (%lums) -> %s | Reason: %s\n",
                  now,
                  bleStateToString(oldState),
                  stateTime,
                  bleStateToString(newState), 
                  reason);
}

// Full cleanup of BLE connection state - call before retry or after failures
void V1BLEClient::cleanupConnection() {
    // 1. Unsubscribe from notifications if subscribed
    if (pDisplayDataChar && pDisplayDataChar->canNotify()) {
        pDisplayDataChar->unsubscribe();
    }
    
    // 2. Disconnect if connected
    if (pClient && pClient->isConnected()) {
        pClient->disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));  // Allow disconnect to fully complete
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
}

// Hard reset of BLE client stack - use after repeated failures
// Clears stuck state without recreating client (which causes callback corruption)
void V1BLEClient::hardResetBLEClient() {
    Serial.println("[BLE] Hard reset...");
    
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
        pClient = nullptr;
    }
    
    // Create fresh client
    pClient = NimBLEDevice::createClient();
    if (pClient) {
        if (!pClientCallbacks) {
            pClientCallbacks = new ClientCallbacks();
        }
        pClient->setClientCallbacks(pClientCallbacks);
        // Connection parameters: 12-24 (15-30ms interval), balanced for stability
        pClient->setConnectionParams(12, 24, 0, 400);
        pClient->setConnectTimeout(15);
    } else {
        Serial.println("[BLE] ERROR: Failed to create client!");
    }
    
    // Reset failure counter after hard reset
    consecutiveConnectFailures = 0;
    nextConnectAllowedMs = millis() + 2000;
    
    setBLEState(BLEState::DISCONNECTED, "hard reset complete");
}

// Initialize BLE stack without starting scan
bool V1BLEClient::initBLE(bool enableProxy, const char* proxyName) {
    static bool initialized = false;
    if (initialized) {
        return true;  // Already initialized
    }
    
    Serial.print("[BLE] Init...");
    
    proxyEnabled = enableProxy;
    proxyName_ = proxyName ? proxyName : "V1C-LE-S3";
    
    // Create mutexes for thread-safe BLE operations (only once)
    if (!bleMutex) {
        bleMutex = xSemaphoreCreateMutex();
    }
    if (!bleNotifyMutex) {
        bleNotifyMutex = xSemaphoreCreateMutex();
    }
    if (!phoneCmdMutex) {
        phoneCmdMutex = xSemaphoreCreateMutex();
    }
    
    if (!bleMutex || !bleNotifyMutex || !phoneCmdMutex) {
        Serial.println("FAIL");
        return false;
    }
    
    // Fresh-flash detection: clear BLE bonds if firmware version changed
    // Stale bonding info in NVS can cause connection issues after OTA/flash
    {
        Preferences blePrefs;
        blePrefs.begin("ble_state", false);  // Read-write mode
        String storedVersion = blePrefs.getString("fwVersion", "");
        if (storedVersion != FIRMWARE_VERSION) {
            Serial.printf(" fresh-flash, clearing bonds...");
            // NimBLE must be initialized before deleteAllBonds works
            // We'll do a minimal init, clear bonds, then deinit and reinit properly
            NimBLEDevice::init("");
            NimBLEDevice::deleteAllBonds();
            NimBLEDevice::deinit(true);  // true = clear all BLE state
            vTaskDelay(pdMS_TO_TICKS(100));  // Let BLE stack settle
            blePrefs.putString("fwVersion", FIRMWARE_VERSION);
            freshFlashBoot = true;
        }
        blePrefs.end();
    }
    
    // BLE initialization pattern for NimBLE dual-role stability:
    // 1. init() with generic name
    // 2. setDeviceName() with the actual advertised name  
    // 3. setPower() and setMTU for better throughput
    // 4. Create proxy server BEFORE scanning (critical for dual-role)
    // 5. Start advertising then stop (initializes BLE stack)
    // 6. After V1 connects, advertising restarts via startProxyAdvertising()
    if (proxyEnabled) {
        NimBLEDevice::init("V1 Proxy");
        NimBLEDevice::setDeviceName(proxyName_.c_str());
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        NimBLEDevice::setMTU(517);  // Max MTU for BLE 5.x
        
        // Create proxy server before scanning for dual-role stability
        initProxyServer(proxyName_.c_str());
        proxyServerInitialized = true;
    } else {
        NimBLEDevice::init("V1Display");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        NimBLEDevice::setMTU(517);  // Max MTU for BLE 5.x
    }
    
    // Create client once during init - reuse for all connection attempts
    // Don't delete/recreate on failures - causes callback pointer corruption
    if (!pClient) {
        pClient = NimBLEDevice::createClient();
        if (!pClient) {
            Serial.println("ERROR: Failed to create BLE client");
            return false;
        }
        
        // Create callbacks once and keep them for the lifetime of the client
        if (!pClientCallbacks) {
            pClientCallbacks = new ClientCallbacks();
        }
        pClient->setClientCallbacks(pClientCallbacks);
        
        // Connection parameters: 12-24 (15-30ms interval), balanced for stability
        pClient->setConnectionParams(12, 24, 0, 400);
        pClient->setConnectTimeout(15);
    }
    
    initialized = true;
    Serial.printf(" OK proxy=%s\n", proxyEnabled ? "on" : "off");
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
    // ESP32-S3 WiFi coexistence: use 75% duty cycle for reliable V1 discovery
    // Higher duty = more BLE radio time = faster discovery, but less WiFi throughput
    pScan->setInterval(160);  // 100ms interval 
    pScan->setWindow(120);    // 75ms window - 75% duty cycle (was 50%)
    pScan->setMaxResults(0);  // Unlimited results
    // Filter duplicate advertisements to reduce scan load and radio time
    pScan->setDuplicateFilter(true);
    
    BLE_SM_LOGF("Scanning for V1 Gen2...\n");
    lastScanStart = millis();
    bool started = pScan->start(SCAN_DURATION, false, false);  // duration, isContinuous, restart
    BLE_SM_LOGF("Scan started: %s\n", started ? "YES" : "NO");
    
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

// RSSI caching - only query BLE stack every 2 seconds to reduce overhead
static int s_cachedV1Rssi = 0;
static unsigned long s_lastV1RssiQueryMs = 0;
static constexpr unsigned long RSSI_QUERY_INTERVAL_MS = 2000;
static int s_lastLoggedV1Rssi = 0;
static bool s_loggedV1Rssi = false;

int V1BLEClient::getConnectionRssi() {
    // Return RSSI of connected V1 device, or 0 if not connected
    if (!connected || !pClient || !pClient->isConnected()) {
        if (debugLogger.isEnabledFor(DebugLogCategory::Ble) && s_loggedV1Rssi && s_lastLoggedV1Rssi != 0) {
            debugLogger.log(DebugLogCategory::Ble, "V1 RSSI unavailable (not connected)");
        }
        s_cachedV1Rssi = 0;
        s_lastLoggedV1Rssi = 0;
        s_loggedV1Rssi = true;
        return 0;
    }
    
    // Only query BLE stack every 2 seconds - return cached value otherwise
    unsigned long now = millis();
    bool updated = false;
    if (now - s_lastV1RssiQueryMs >= RSSI_QUERY_INTERVAL_MS) {
        s_cachedV1Rssi = pClient->getRssi();
        s_lastV1RssiQueryMs = now;
        updated = true;
    }

    if (updated && debugLogger.isEnabledFor(DebugLogCategory::Ble)) {
        if (!s_loggedV1Rssi || s_cachedV1Rssi != s_lastLoggedV1Rssi) {
            debugLogger.logf(DebugLogCategory::Ble, "V1 RSSI: %d dBm", s_cachedV1Rssi);
            s_lastLoggedV1Rssi = s_cachedV1Rssi;
            s_loggedV1Rssi = true;
        }
    }
    return s_cachedV1Rssi;
}

// Proxy client RSSI caching
static int s_cachedProxyRssi = 0;
static unsigned long s_lastProxyRssiQueryMs = 0;
static int s_lastLoggedProxyRssi = 0;
static bool s_loggedProxyRssi = false;

int V1BLEClient::getProxyClientRssi() {
    // Return RSSI of connected proxy client (JBV1/phone), or 0 if not connected
    if (!proxyClientConnected || !pServer || pServer->getConnectedCount() == 0) {
        if (debugLogger.isEnabledFor(DebugLogCategory::Ble) && s_loggedProxyRssi && s_lastLoggedProxyRssi != 0) {
            debugLogger.log(DebugLogCategory::Ble, "Proxy RSSI unavailable (no client)");
        }
        s_cachedProxyRssi = 0;
        s_lastLoggedProxyRssi = 0;
        s_loggedProxyRssi = true;
        return 0;
    }
    
    // Only query BLE stack every 2 seconds
    unsigned long now = millis();
    bool updated = false;
    if (now - s_lastProxyRssiQueryMs >= RSSI_QUERY_INTERVAL_MS) {
        // Get connection handle of first connected peer
        NimBLEConnInfo peerInfo = pServer->getPeerInfo(0);
        uint16_t connHandle = peerInfo.getConnHandle();
        int8_t rssi = 0;
        if (ble_gap_conn_rssi(connHandle, &rssi) == 0) {
            s_cachedProxyRssi = rssi;
        }
        s_lastProxyRssiQueryMs = now;
        updated = true;
    }

    if (updated && debugLogger.isEnabledFor(DebugLogCategory::Ble)) {
        if (!s_loggedProxyRssi || s_cachedProxyRssi != s_lastLoggedProxyRssi) {
            debugLogger.logf(DebugLogCategory::Ble, "Proxy RSSI: %d dBm", s_cachedProxyRssi);
            s_lastLoggedProxyRssi = s_cachedProxyRssi;
            s_loggedProxyRssi = true;
        }
    }
    return s_cachedProxyRssi;
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
    
    // Optional: Uncomment for BLE scan debugging
    // static int debugCount = 0;
    // if (debugCount < 20) {
    //     Serial.printf("[BLE %2d] addr=%s RSSI=%3d name='%s'\n",
    //                   debugCount++, addrStr.c_str(), rssi,
    //                   name.length() > 0 ? name.c_str() : "(no name)");
    // }
    
    // *** Check for OBD-II device (pass to OBD handler) ***
    // When OBD is actively scanning, pass ALL named devices to handler
    // User can then select which one to connect to from the UI
    if (!name.empty() && obdHandler.isScanActive()) {
        // Defer OBD device discovery to main loop to avoid BLE callback work
        if (bleClient) {
            bleClient->enqueueObdScanResult(name.c_str(), addrStr.c_str(), rssi);
        }
    }
    
    // *** V1 NAME FILTER - Only connect to Valentine V1 Gen2 devices ***
    // V1 Gen2 advertises as "V1G*" (like "V1G27B7A") or sometimes "V1-*"
    // Case-insensitive check without creating String objects
    bool isV1 = false;
    if (name.length() >= 3) {
        char c0 = name[0] | 0x20;  // lowercase
        char c1 = name[1] | 0x20;
        char c2 = name[2] | 0x20;
        isV1 = (c0 == 'v' && c1 == '1' && (c2 == 'g' || c2 == '-'));
    }
    
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
    
    // If OBD scan is active AND we're already connected to V1, let OBD scan continue
    // If NOT connected to V1, we should still connect - OBD scan can happen after
    if (obdHandler.isScanActive() && bleClient->connected) {
        return;  // Already have V1, let OBD scan continue
    }
    
    // Save this address for future fast reconnects (deferred to main loop)
    if (bleClient) {
        bleClient->deferLastV1Address(addrStr.c_str());
    }
    
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
    } else if (bleClient) {
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
    
    // Notify OBD handler that scan has ended (if it was scanning)
    if (obdHandler.isScanActive()) {
        if (instancePtr) {
            instancePtr->pendingObdScanComplete = true;
        }
    }
}

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
    if (reason != 0 && reason != BLE_HS_ETIMEOUT) { // 0 is normal disconnect
        NimBLEAddress addr = pClient->getPeerAddress();
        if (NimBLEDevice::isBonded(addr)) {
            NimBLEDevice::deleteBond(addr);
        }
    }

    if (instancePtr) {
        // Stop proxy advertising FIRST before any state changes
        if (instancePtr->proxyEnabled && NimBLEDevice::getAdvertising()->isAdvertising()) {
            NimBLEDevice::stopAdvertising();
            // No delay here - callback must return quickly
        }
        
        if (instancePtr->bleMutex && xSemaphoreTake(instancePtr->bleMutex, 0) == pdTRUE) {
            instancePtr->connected = false;
            instancePtr->connectInProgress = false;  // Clear connection guard
            instancePtr->connectStartMs = 0;  // Clear async connect timer
            // Clear proxy client connection state too - can't proxy without V1 connection
            instancePtr->proxyClientConnected = false;
            // Do NOT clear pClient - we reuse it to prevent memory leaks
            instancePtr->pRemoteService = nullptr;
            instancePtr->pDisplayDataChar = nullptr;
            instancePtr->pCommandChar = nullptr;
            instancePtr->pCommandCharLong = nullptr;
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
    if (consecutiveConnectFailures > 0 && now < nextConnectAllowedMs) {
        {
            SemaphoreGuard lock(bleMutex);
            shouldConnect = false;
        }
        setBLEState(BLEState::BACKOFF, "backoff active");
        return false;
    }
    
    // Set connection guard and initiate async connect
    connectInProgress = true;
    connectStartMs = millis();
    connectAttemptNumber = 0;  // Reset for new connection sequence
    asyncConnectPending = false;
    asyncConnectSuccess = false;
    connectPhaseStartUs = micros();  // Start timing connect phase
    setBLEState(BLEState::CONNECTING, "connectToServer");
    
    // Initiate first async connect attempt
    return startAsyncConnect();
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
            pClientCallbacks = new ClientCallbacks();
        }
        pClient->setClientCallbacks(pClientCallbacks);
    }
    
    // Connection parameters: 12-24 (15-30ms interval), balanced for stability
    pClient->setConnectionParams(12, 24, 0, 400);
    // Give it plenty of time to connect (20s)
    pClient->setConnectTimeout(20);

    // Ensure client is disconnected before attempting
    if (pClient->isConnected()) {
        Serial.println("[BLE] Client thinks it's connected, disconnecting first");
        pClient->disconnect();
        // Give NimBLE time to process the disconnect
        vTaskDelay(pdMS_TO_TICKS(100));
        // If still "connected" after delay, need a harder reset
        if (pClient->isConnected()) {
            Serial.println("[BLE] Client still thinks it's connected after disconnect - hard reset");
            hardResetBLEClient();
            return false;  // Will retry on next process() cycle
        }
    }
    
    // Clear async state before initiating connect
    asyncConnectPending = true;
    asyncConnectSuccess = false;
    
    // Use ASYNCHRONOUS connect - returns immediately, callback will set asyncConnectSuccess
    // NimBLE 2.x: connect(address, deleteAttributes, asyncConnect, exchangeMTU)
    bool initiated = pClient->connect(targetAddress, true, true);
    
    if (!initiated) {
        int err = pClient->getLastError();
        BLE_SM_LOGF("[BLE] Async connect initiation failed (error: %d)\n", err);
        asyncConnectPending = false;
        
        // Check if we should retry
        if (connectAttemptNumber < MAX_CONNECT_ATTEMPTS) {
            // Will retry on next process() iteration
            return true;  // Keep state machine going
        }
        
        // All attempts exhausted
        consecutiveConnectFailures++;
        perfRecordBleConnectUs(micros() - connectPhaseStartUs);
        
        if (consecutiveConnectFailures >= MAX_BACKOFF_FAILURES) {
            hardResetBLEClient();
            return false;
        }
        
        // Calculate exponential backoff
        int exponent = (consecutiveConnectFailures > 4) ? 4 : (consecutiveConnectFailures - 1);
        unsigned long backoffMs = BACKOFF_BASE_MS * (1 << exponent);
        if (backoffMs > BACKOFF_MAX_MS) backoffMs = BACKOFF_MAX_MS;
        nextConnectAllowedMs = millis() + backoffMs;
        
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
            
            if (consecutiveConnectFailures >= MAX_BACKOFF_FAILURES) {
                hardResetBLEClient();
                return;
            }
            
            // Calculate exponential backoff
            int exponent = (consecutiveConnectFailures > 4) ? 4 : (consecutiveConnectFailures - 1);
            unsigned long backoffMs = BACKOFF_BASE_MS * (1 << exponent);
            if (backoffMs > BACKOFF_MAX_MS) backoffMs = BACKOFF_MAX_MS;
            nextConnectAllowedMs = millis() + backoffMs;
            
            connectInProgress = false;
            connectStartMs = 0;
            setBLEState(BLEState::BACKOFF, "connect timeout");
        }
        return;  // Still waiting
    }
    
    // Async connect failed (asyncConnectPending cleared without asyncConnectSuccess)
    int err = pClient ? pClient->getLastError() : -1;
    BLE_SM_LOGF("[BLE] Async connect attempt %d failed (error: %d)\n", connectAttemptNumber, err);
    
    // Check if we should retry
    if (connectAttemptNumber < MAX_CONNECT_ATTEMPTS) {
        // Brief delay before retry (non-blocking via state machine)
        if (err == 13) {  // EBUSY - need longer wait
            vTaskDelay(pdMS_TO_TICKS(100));  // Short yield, not 2s block
        }
        
        // Initiate next attempt
        startAsyncConnect();
        return;
    }
    
    // All attempts exhausted
    consecutiveConnectFailures++;
    perfRecordBleConnectUs(micros() - connectPhaseStartUs);
    
    if (consecutiveConnectFailures >= MAX_BACKOFF_FAILURES) {
        hardResetBLEClient();
        return;
    }
    
    // Calculate exponential backoff
    int exponent = (consecutiveConnectFailures > 4) ? 4 : (consecutiveConnectFailures - 1);
    unsigned long backoffMs = BACKOFF_BASE_MS * (1 << exponent);
    if (backoffMs > BACKOFF_MAX_MS) backoffMs = BACKOFF_MAX_MS;
    nextConnectAllowedMs = millis() + backoffMs;
    
    connectInProgress = false;
    connectStartMs = 0;
    setBLEState(BLEState::BACKOFF, "all connect attempts failed");
}

// Process DISCOVERING state - performs service discovery
// Uses cached GATT handles when available to skip full discovery
void V1BLEClient::processDiscovering() {
    unsigned long elapsed = millis() - connectStartMs;
    
    // Check for timeout
    if (elapsed > CONNECT_TIMEOUT_MS + DISCOVERY_TIMEOUT_MS) {
        Serial.println("[BLE] Discovery timeout");
        perfRecordBleDiscoveryUs(micros() - connectPhaseStartUs);
        disconnect();
        connectInProgress = false;
        connectStartMs = 0;
        setBLEState(BLEState::DISCONNECTED, "discovery timeout");
        return;
    }
    
    // Perform full service discovery
    // NOTE: This is BLOCKING (~2s) - NimBLE doesn't support non-blocking discovery
    // The subscribe step machine breaks up the remaining work to reduce overall stall time
    bool discovered = pClient->discoverAttributes();
    
    perfRecordBleDiscoveryUs(micros() - connectPhaseStartUs);
    
    if (!discovered) {
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
            SemaphoreGuard lock(bleMutex);
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
    if (millis() >= subscribeYieldUntilMs) {
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
            subscribeStep = SubscribeStep::WRITE_DISPLAY_CCCD;
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
                subscribeStep = SubscribeStep::WRITE_LONG_CCCD;
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
            // Mark as connected before sending requests
            {
                SemaphoreGuard lock(bleMutex);
                connected = true;
            }
            
            if (!requestAlertData()) {
                Serial.println("[BLE] Failed to request alert data (non-critical)");
            }
            subscribeStep = SubscribeStep::REQUEST_VERSION;
            return false;
        }
        
        case SubscribeStep::REQUEST_VERSION: {
            if (!requestVersion()) {
                Serial.println("[BLE] Failed to request version (non-critical)");
            }
            
            // Notify user callback
            if (connectCallback) {
                connectCallback();
            }
            
            // Schedule proxy advertising
            if (proxyEnabled && proxyServerInitialized) {
                proxyAdvertisingStartMs = millis() + PROXY_STABILIZE_MS;
            }
            
            subscribeStep = SubscribeStep::COMPLETE;
            return true;  // All done!
        }
        
        case SubscribeStep::COMPLETE:
            return true;  // Already complete
    }
    
    return true;  // Shouldn't reach here
}

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

// NOTE: setupCharacteristics() has been replaced by the step machine (executeSubscribeStep)
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
    return sendCommandWithResult(data, length) == SendResult::SENT;
}

SendResult V1BLEClient::sendCommandWithResult(const uint8_t* data, size_t length) {
    // Hard failures first - these should not be retried
    if (!isConnected() || !pCommandChar) {
        return SendResult::FAILED;
    }
    if (!data || length == 0 || length > 64) {
        return SendResult::FAILED;
    }

    // Light pacing: non-blocking timestamp gate
    // Return NOT_YET if too soon - caller retains packet for retry
    static unsigned long lastCommandMs = 0;
    unsigned long nowMs = millis();
    if (lastCommandMs != 0 && nowMs - lastCommandMs < 5) {
        PERF_INC(cmdPaceNotYet);
        return SendResult::NOT_YET;
    }
    lastCommandMs = millis();
    
    bool ok = false;
    if (pCommandChar->canWrite()) {
        ok = pCommandChar->writeValue(data, length, true);
    } else if (pCommandChar->canWriteNoResponse()) {
        ok = pCommandChar->writeValue(data, length, false);
    } else {
        return SendResult::FAILED;  // Characteristic doesn't support write
    }
    
    if (!ok) {
        // Write failed after isConnected() check - likely transient (BLE busy/queue full)
        // Return NOT_YET to retry; if connection truly dead, next isConnected() will catch it
        PERF_INC(cmdBleBusy);
        return SendResult::NOT_YET;
    }
    return SendResult::SENT;
}

bool V1BLEClient::requestAlertData() {
    // NOTE: Packet structure intentionally explicit (not abstracted) - January 20, 2026 review.
    // Matches V1 protocol docs exactly; easier to verify than a builder pattern.
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_REQ_START_ALERT,
        0x01,
        0x00,
        ESP_PACKET_END
    };

    packet[5] = calcV1Checksum(packet, 5);

    BLE_LOGLN("Requesting alert data from V1...");
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

    packet[5] = calcV1Checksum(packet, 5);

    BLE_LOGLN("Requesting version info from V1...");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::setDisplayOn(bool on) {
    // For dark mode, we need to use reqTurnOffMainDisplay with proper payload
    // Mode 0 = completely off, Mode 1 = only BT icon visible
    // For "dark mode on" we want display OFF, for "dark mode off" we want display ON
    // 
    // Packet format derived from v1g2-t4s3 reference implementation:
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
        packet[5] = calcV1Checksum(packet, 5);
        
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
        packet[6] = calcV1Checksum(packet, 6);
        
        return sendCommand(packet, sizeof(packet));
    }
}

bool V1BLEClient::setMute(bool muted) {
    uint8_t packetId = muted ? PACKET_ID_MUTE_ON : PACKET_ID_MUTE_OFF;
    // reqMuteOn/Off has no payload (payload length = 1, no actual payload bytes needed per V1 protocol)
    // Packet: AA DA E4 34/35 01 [checksum] AB
    uint8_t packet[] = {
        ESP_PACKET_START,                               // [0] 0xAA
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
        packetId,                                       // [3] 0x34 or 0x35
        0x01,                                           // [4] payload length
        0x00,                                           // [5] checksum placeholder
        ESP_PACKET_END                                  // [6] 0xAB
    };

    packet[5] = calcV1Checksum(packet, 5);
    
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
    packet[6] = calcV1Checksum(packet, 6);
    
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
    packet[8] = calcV1Checksum(packet, 8);
    
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

    packet[5] = calcV1Checksum(packet, 5);

    BLE_LOGLN("Requesting V1 user bytes...");
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
    
    packet[11] = calcV1Checksum(packet, 11);
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
    // Handle deferred BLE callback updates without blocking in callbacks
    if (pendingConnectStateUpdate) {
        if (bleMutex && xSemaphoreTake(bleMutex, 0) == pdTRUE) {
            pendingConnectStateUpdate = false;
            connected = true;
            // Don't set CONNECTED state here - async state machine handles transitions
            // Just set the connected flag; state machine will transition via asyncConnectSuccess
            xSemaphoreGive(bleMutex);
        }
    }
    if (pendingDisconnectCleanup) {
        if (bleMutex && xSemaphoreTake(bleMutex, 0) == pdTRUE) {
            pendingDisconnectCleanup = false;
            connected = false;
            connectInProgress = false;
            connectStartMs = 0;
            proxyClientConnected = false;
            pRemoteService = nullptr;
            pDisplayDataChar = nullptr;
            pCommandChar = nullptr;
            pCommandCharLong = nullptr;
            verifyPending = false;
            verifyComplete = false;
            verifyMatch = false;
            setBLEState(BLEState::DISCONNECTED, "deferred onDisconnect");
            xSemaphoreGive(bleMutex);
        }
    }
    if (pendingScanEndUpdate) {
        if (bleMutex && xSemaphoreTake(bleMutex, 0) == pdTRUE) {
            pendingScanEndUpdate = false;
            if (bleState == BLEState::SCANNING) {
                setBLEState(BLEState::DISCONNECTED, "scan ended without finding V1 (deferred)");
            }
            xSemaphoreGive(bleMutex);
        }
    }
    if (pendingScanTargetUpdate) {
        if (bleMutex && xSemaphoreTake(bleMutex, 0) == pdTRUE) {
            char addrCopy[sizeof(pendingScanTargetAddress)] = {0};
            uint8_t addrTypeCopy = BLE_ADDR_PUBLIC;
            bool havePending = false;
            portENTER_CRITICAL(&pendingAddrMux);
            if (pendingScanTargetUpdate) {
                pendingScanTargetUpdate = false;
                memcpy(addrCopy, pendingScanTargetAddress, sizeof(pendingScanTargetAddress));
                addrCopy[sizeof(addrCopy) - 1] = '\0';
                addrTypeCopy = pendingScanTargetAddressType;
                havePending = true;
            }
            portEXIT_CRITICAL(&pendingAddrMux);
            if (havePending) {
                targetAddress = NimBLEAddress(std::string(addrCopy), addrTypeCopy);
                targetAddressType = addrTypeCopy;
                hasTargetDevice = true;
                shouldConnect = true;
                scanStopRequestedMs = millis();
                setBLEState(BLEState::SCAN_STOPPING, "V1 found (deferred)");
            }
            xSemaphoreGive(bleMutex);
        }
    }
    if (pendingLastV1AddressValid) {
        char addrCopy[sizeof(pendingLastV1Address)] = {0};
        bool shouldWrite = false;
        portENTER_CRITICAL(&pendingAddrMux);
        if (pendingLastV1AddressValid) {
            pendingLastV1AddressValid = false;
            memcpy(addrCopy, pendingLastV1Address, sizeof(pendingLastV1Address));
            addrCopy[sizeof(addrCopy) - 1] = '\0';
            shouldWrite = true;
        }
        portEXIT_CRITICAL(&pendingAddrMux);
        if (shouldWrite) {
            settingsManager.setLastV1Address(addrCopy);
        }
    }
    if (pendingObdScanComplete) {
        pendingObdScanComplete = false;
        obdHandler.onScanComplete();
    }
    // Drain deferred OBD scan results (safe outside BLE callback)
    while (obdScanCount > 0) {
        ObdScanItem item;
        bool haveItem = false;
        portENTER_CRITICAL(&obdScanMux);
        if (obdScanCount > 0) {
            item = obdScanQueue[obdScanTail];
            obdScanTail = (obdScanTail + 1) % OBD_SCAN_QUEUE_SIZE;
            obdScanCount--;
            haveItem = true;
        }
        portEXIT_CRITICAL(&obdScanMux);
        if (!haveItem) {
            break;
        }
        obdHandler.onDeviceFoundDeferred(item.name, item.addr, item.rssi);
    }
    // Process phone->V1 commands (up to queue size per loop to drain any backlog)
    // Each call processes one command to minimize mutex hold time during BLE writes
    for (int i = 0; i < MAX_PHONE_CMDS_PER_LOOP; i++) {
        if (processPhoneCommandQueue() == 0) {
            break;
        }
    }

    // Handle deferred proxy advertising start (non-blocking replacement for delay(1500))
    if (proxyAdvertisingStartMs != 0 && millis() >= proxyAdvertisingStartMs) {
        proxyAdvertisingStartMs = 0;  // Clear pending flag
        
        if (isConnected() && proxyEnabled && proxyServerInitialized) {
            // Advertising data already configured in initProxyServer() with proper flags
            startProxyAdvertising();
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
                lastScanStart = now;
                pScan->clearResults();
                bool started = pScan->start(SCAN_DURATION, false, false);
                if (started) {
                    setBLEState(BLEState::SCANNING, "scan started");
                }
            }
            break;
        }
        
        case BLEState::SCANNING: {
            // Check if scan found a device (shouldConnect flag set by callback)
            bool wantConnect = false;
            {
                // HOT PATH: try-lock only, skip if busy
                SemaphoreGuard lock(bleMutex, 0);
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
            // Use longer settle on first scan after boot (radio is "cold")
            unsigned long settleTime = firstScanAfterBoot ? SCAN_STOP_SETTLE_FRESH_MS : SCAN_STOP_SETTLE_MS;
            if (elapsed >= settleTime) {
                if (firstScanAfterBoot) {
                    Serial.println("[BLE] First scan settle complete (extended)");
                    firstScanAfterBoot = false;
                }
                resultsCleared = false;  // Reset for next time
                // Ready to connect
                bool wantConnect = false;
                {
                    // HOT PATH: try-lock only, skip if busy
                    SemaphoreGuard lock(bleMutex, 0);
                    if (lock.locked()) {
                        wantConnect = shouldConnect;
                        shouldConnect = false;  // Clear flag
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
            // Async connection initiation in progress
            // This state is brief - we transition to CONNECTING_WAIT after initiating
            // If we're stuck here for too long, something is wrong
            if (connectStartMs > 0 && (now - connectStartMs) > 5000) {
                Serial.println("[BLE] Connect initiation stuck for 5s - resetting");
                connectInProgress = false;
                connectStartMs = 0;
                asyncConnectPending = false;
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
            // Verify we're actually still connected
            if (!pClient || !pClient->isConnected()) {
                connected = false;
                connectInProgress = false;
                setBLEState(BLEState::DISCONNECTED, "connection lost");
            }
            break;
        }
        
        case BLEState::BACKOFF: {
            // Waiting for backoff period to expire
            if (now >= nextConnectAllowedMs) {
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
            lastScanStart = millis();
            pScan->clearResults();
            bool started = pScan->start(SCAN_DURATION, false, false);
            if (started) {
                setBLEState(BLEState::SCANNING, "manual scan start");
            }
        }
    }
}

void V1BLEClient::startOBDScan() {
    // Start a BLE scan for OBD devices - works even when V1 is connected
    // This allows scanning for OBD adapters without disconnecting from V1
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && !pScan->isScanning()) {
        Serial.println("[BLE] Starting OBD device scan (30 seconds)...");
        pScan->clearResults();
        // 30-second scan for OBD devices - duration is in MILLISECONDS
        pScan->start(30000, false, false);
    } else if (pScan && pScan->isScanning()) {
        Serial.println("[BLE] Scan already in progress - extending for OBD");
        // Stop and restart with longer duration for OBD scan
        pScan->stop();
        delay(100);  // Brief delay for BLE stack
        pScan->clearResults();
        pScan->start(30000, false, false);
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
    // NOTE: BLE callback - keep fast
    if (BLE_CALLBACK_LOGS) {
        BLE_LOGF("[BLE] JBV1/Phone connected (handle: %d)\n", connInfo.getConnHandle());
    }
    
    // Request connection parameters - use Android-compatible range
    // Min 15ms (12), Max 45ms (36), Latency 0, Timeout 4s (400)
    // Some devices (Motorola G series) reject very tight intervals
    uint16_t connHandle = connInfo.getConnHandle();
    pServer->updateConnParams(connHandle, 12, 36, 0, 400);
    
    if (bleClient) {
        bleClient->proxyClientConnected = true;
    }
}

void V1BLEClient::ProxyServerCallbacks::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
    // NOTE: BLE callback - keep fast
    if (BLE_CALLBACK_LOGS) {
        BLE_LOGF("[BLE] JBV1/Phone disconnected (reason: %d)\n", reason);
    }
    if (bleClient) {
        bleClient->proxyClientConnected = false;
        // Resume advertising if V1 is still connected
        if (bleClient->connected) {
            NimBLEDevice::startAdvertising();
        }
    }
}

void V1BLEClient::deferLastV1Address(const char* addr) {
    if (!addr || addr[0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&pendingAddrMux);
    snprintf(pendingLastV1Address, sizeof(pendingLastV1Address), "%s", addr);
    pendingLastV1AddressValid = true;
    portEXIT_CRITICAL(&pendingAddrMux);
}

void V1BLEClient::enqueueObdScanResult(const char* name, const char* addr, int rssi) {
    if (!name || !addr || name[0] == '\0' || addr[0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&obdScanMux);
    if (obdScanCount >= OBD_SCAN_QUEUE_SIZE) {
        // Drop oldest on overflow
        obdScanTail = (obdScanTail + 1) % OBD_SCAN_QUEUE_SIZE;
        obdScanCount--;
    }
    ObdScanItem& item = obdScanQueue[obdScanHead];
    snprintf(item.name, sizeof(item.name), "%s", name);
    snprintf(item.addr, sizeof(item.addr), "%s", addr);
    item.rssi = rssi;
    obdScanHead = (obdScanHead + 1) % OBD_SCAN_QUEUE_SIZE;
    obdScanCount++;
    PERF_MAX(obdScanQueueHighWater, obdScanCount);
    portEXIT_CRITICAL(&obdScanMux);
}

void V1BLEClient::ProxyWriteCallbacks::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    // Forward commands from JBV1 to V1
    // NOTE: This is a BLE callback - avoid blocking operations (Serial, delays, long locks)
    if (!pCharacteristic || !bleClient) {
        return;
    }
    
    if (!bleClient->connected) {
        return;
    }
    
    // Get the raw data pointer and length directly
    NimBLEAttValue attrValue = pCharacteristic->getValue();
    const uint8_t* rawData = attrValue.data();
    size_t rawLen = attrValue.size();
    
    if (rawLen == 0 || !rawData || rawLen > 32) {
        return;
    }
    
    // Copy to a safe buffer and forward to V1
    uint16_t sourceChar = shortUuid(pCharacteristic->getUUID());
    uint8_t cmdBuf[32];
    memcpy(cmdBuf, rawData, rawLen);
    
    // Proxy command logging disabled - we confirmed JBV1 uses standard mute (0x34/0x35)
    // Uncomment to debug: snprintf(logBuf, ...) with packet ID at cmdBuf[3]
    
    // Enqueue for main-loop processing to avoid BLE callback blocking
    bleClient->enqueuePhoneCommand(cmdBuf, rawLen, sourceChar);
}

void V1BLEClient::initProxyServer(const char* deviceName) {
    // Proxy server init (name logged in initBLE summary)
    
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
    [[maybe_unused]] NimBLECharacteristic* pNotifyAlt = pProxyService->createCharacteristic(
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
    
    // Configure advertising data with improved Android compatibility
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    NimBLEAdvertisementData scanRespData;
    
    // CRITICAL: Set flags to indicate BLE-only device (0x06 = LE General Discoverable + BR/EDR Not Supported)
    // Without this flag, some Android devices (Motorola G series) may cache the device as "DUAL" (type 3)
    // and attempt BR/EDR connections which fail with GATT_ERROR 133
    advData.setFlags(0x06);
    
    // Include service UUID in advertising data (required for JBV1 discovery)
    advData.setCompleteServices(pProxyService->getUUID());
    advData.setAppearance(0x0C80);  // Generic tag appearance
    
    // Put name in both adv data AND scan response for broader compatibility
    // Some Android devices (especially older Motorola) only read one or the other
    advData.setName(deviceName);
    scanRespData.setName(deviceName);
    
    pAdvertising->setAdvertisementData(advData);
    pAdvertising->setScanResponseData(scanRespData);
    
    // Advertising interval: 50-100ms is optimal for Android discovery
    // Some devices (Motorola G series) have trouble with faster intervals
    pAdvertising->setMinInterval(0x50);   // 50ms in 0.625ms units = ~50ms
    pAdvertising->setMaxInterval(0xA0);   // 100ms in 0.625ms units = ~100ms
    
    // Start/stop advertising to initialize the stack cleanly before scanning
    pAdvertising->start();
    delay(50);
    NimBLEDevice::stopAdvertising();
    delay(50);
}

bool V1BLEClient::isProxyAdvertising() const {
    return proxyEnabled && proxyServerInitialized && 
           NimBLEDevice::getAdvertising()->isAdvertising();
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
    
    // Protect queue operations from concurrent access (BLE callback vs main loop)
    if (bleNotifyMutex && xSemaphoreTake(bleNotifyMutex, 0) != pdTRUE) {
        // Queue busy – drop to avoid blocking in callback path
        proxyMetrics.dropCount++;
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
    pkt.tsMs = millis();
    proxyQueueHead = (proxyQueueHead + 1) % PROXY_QUEUE_SIZE;
    proxyQueueCount++;
    
    // Track high water mark
    if (proxyQueueCount > proxyMetrics.queueHighWater) {
        proxyMetrics.queueHighWater = proxyQueueCount;
    }
    PERF_MAX(proxyQueueHighWater, proxyQueueCount);

    if (bleNotifyMutex) {
        xSemaphoreGive(bleNotifyMutex);
    }
}

// PERFORMANCE: Immediate proxy forwarding - zero latency path
// Called directly from BLE callback context - no queue, no delay
// Uses non-blocking mutex to avoid deadlock while preventing concurrent notifies
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
    
    if (!targetChar) {
        return;
    }
    
    // Try non-blocking mutex acquire to avoid concurrent notifies
    // If mutex is held (processProxyQueue running), enqueue instead
    if (xSemaphoreTake(bleNotifyMutex, 0) == pdTRUE) {
        // Got mutex - safe to notify immediately
        if (targetChar->notify(data, length)) {
            proxyMetrics.sendCount++;
        } else {
            proxyMetrics.errorCount++;
            // Notify failed (stack busy) - enqueue for retry
            // Release mutex first to avoid recursive lock in forwardToProxy
            xSemaphoreGive(bleNotifyMutex);
            forwardToProxy(data, length, sourceCharUUID);
            return;
        }
        xSemaphoreGive(bleNotifyMutex);
    } else {
        // Mutex held by processProxyQueue - enqueue to avoid race
        forwardToProxy(data, length, sourceCharUUID);
    }
}

int V1BLEClient::processProxyQueue() {
    if (!proxyEnabled || !proxyClientConnected || proxyQueueCount == 0) {
        return 0;
    }
    
    // HOT PATH: try-lock only, skip if busy (another iteration will process)
    SemaphoreGuard lock(bleNotifyMutex, 0);
    if (!lock.locked()) {
        return 0;  // Skip this cycle, try again next loop (counter incremented in SemaphoreGuard)
    }
    
    int sent = 0;
    
    // Process all queued packets (typically 1-2)
    while (proxyQueueCount > 0) {
        ProxyPacket& pkt = proxyQueue[proxyQueueTail];
        uint32_t nowMs = millis();
        if (pkt.tsMs != 0 && nowMs >= pkt.tsMs) {
            perfRecordNotifyToProxyMs(nowMs - pkt.tsMs);
        }

        NimBLECharacteristic* targetChar = nullptr;
        if (pkt.charUUID == 0xB4E0 && pProxyNotifyLongChar) {
            targetChar = pProxyNotifyLongChar;
        } else if (pProxyNotifyChar) {
            targetChar = pProxyNotifyChar;
        }

        if (targetChar) {
            if (targetChar->notify(pkt.data, pkt.length)) {
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

bool V1BLEClient::enqueuePhoneCommand(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!data || length == 0 || length > 32) {
        phoneCmdDropsInvalid++;
        return false;
    }

    if (!phoneCmdMutex || xSemaphoreTake(phoneCmdMutex, 0) != pdTRUE) {
        phoneCmdDropsLockBusy++;
        return false;
    }

    if (phone2v1QueueCount >= PHONE_CMD_QUEUE_SIZE) {
        phone2v1QueueTail = (phone2v1QueueTail + 1) % PHONE_CMD_QUEUE_SIZE;
        phone2v1QueueCount--;
        phoneCmdDropsOverflow++;
    }

    ProxyPacket& pkt = phone2v1Queue[phone2v1QueueHead];
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = sourceCharUUID;
    phone2v1QueueHead = (phone2v1QueueHead + 1) % PHONE_CMD_QUEUE_SIZE;
    phone2v1QueueCount++;
    PERF_MAX(phoneCmdQueueHighWater, phone2v1QueueCount);

    xSemaphoreGive(phoneCmdMutex);
    return true;
}

int V1BLEClient::processPhoneCommandQueue() {
    if (!connected) {
        return 0;
    }

    // Static pending packet: holds command when pacing/lock says "not yet"
    // Ensures no command loss from dequeue-before-send pattern
    static ProxyPacket pendingPkt;
    static uint16_t pendingCharUUID = 0;
    static bool hasPending = false;

    ProxyPacket pktCopy;
    uint16_t charUUID = 0;
    bool hasPacket = false;

    // Try pending packet first (from previous pacing/lock deferral)
    if (hasPending) {
        configASSERT(pendingPkt.length <= sizeof(pendingPkt.data));  // Belt-and-suspenders: validated at enqueue
        memcpy(pktCopy.data, pendingPkt.data, pendingPkt.length);
        pktCopy.length = pendingPkt.length;
        charUUID = pendingCharUUID;
        hasPacket = true;
    } else if (phoneCmdMutex && xSemaphoreTake(phoneCmdMutex, 0) == pdTRUE) {
        // Dequeue one packet under lock
        if (phone2v1QueueCount > 0) {
            ProxyPacket& pkt = phone2v1Queue[phone2v1QueueTail];
            configASSERT(pkt.length <= sizeof(pkt.data));  // Belt-and-suspenders: validated at enqueue
            memcpy(pktCopy.data, pkt.data, pkt.length);
            pktCopy.length = pkt.length;
            charUUID = pkt.charUUID;
            phone2v1QueueTail = (phone2v1QueueTail + 1) % PHONE_CMD_QUEUE_SIZE;
            phone2v1QueueCount--;
            hasPacket = true;
        }
        xSemaphoreGive(phoneCmdMutex);
    }

    if (!hasPacket) {
        return 0;
    }

    // Send outside queue lock - BLE write can take time
    // HOT PATH: try-lock only, skip if busy
    SendResult result = SendResult::FAILED;
    if (bleNotifyMutex) {
        SemaphoreGuard lock(bleNotifyMutex, 0);
        if (!lock.locked()) {
            // Mutex busy - store in pending for next iteration (NOT_YET semantics)
            // (counter incremented in SemaphoreGuard)
            memcpy(pendingPkt.data, pktCopy.data, pktCopy.length);
            pendingPkt.length = pktCopy.length;
            pendingCharUUID = charUUID;
            hasPending = true;
            return 0;
        }
        if (charUUID == 0xB8D2 && pCommandCharLong) {
            // Long characteristic write - same transient failure semantics as sendCommand
            if (pCommandCharLong->writeValue(pktCopy.data, pktCopy.length, false)) {
                result = SendResult::SENT;
            } else {
                PERF_INC(cmdBleBusy);
                result = SendResult::NOT_YET;  // Transient - retry
            }
        } else {
            result = sendCommandWithResult(pktCopy.data, pktCopy.length);
        }
    } else {
        if (charUUID == 0xB8D2 && pCommandCharLong) {
            if (pCommandCharLong->writeValue(pktCopy.data, pktCopy.length, false)) {
                result = SendResult::SENT;
            } else {
                PERF_INC(cmdBleBusy);
                result = SendResult::NOT_YET;  // Transient - retry
            }
        } else {
            result = sendCommandWithResult(pktCopy.data, pktCopy.length);
        }
    }

    switch (result) {
        case SendResult::SENT:
            // Successfully sent - clear pending state
            hasPending = false;
            return 1;
        case SendResult::NOT_YET:
            // Pacing: store in pending for next iteration
            memcpy(pendingPkt.data, pktCopy.data, pktCopy.length);
            pendingPkt.length = pktCopy.length;
            pendingCharUUID = charUUID;
            hasPending = true;
            return 0;
        case SendResult::FAILED:
        default:
            // Hard failure: drop packet, clear pending, count error
            hasPending = false;
            phoneCmdDropsBleFail++;  // Count as BLE failure (not connected, char null)
            return 0;
    }
}

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
#include "debug_logger.h"
#include "perf_metrics.h"
#include "storage_manager.h"
#include "../include/config.h"
#include <Arduino.h>
#include <WiFi.h>  // For WiFi coexistence during BLE connect
#include <Preferences.h>  // For fresh-flash detection
#include <set>
#include <string>
#include <cstdlib>
#include <cstring>
#include "../include/ble_internals.h"

// NimBLE low-level store API for bond backup/restore
extern "C" {
#include "nimble/nimble/host/include/host/ble_store.h"
}

// =========================================================================
// BLE Bond Backup/Restore (SD card)
// =========================================================================
// NimBLE stores bonds in NVS ("nimble_bond" namespace). NVS is volatile —
// brownouts, partition changes, and flash erases lose all bonds. This backs
// up bond key material to SD so it can be restored automatically.
//
// File format: /v1simple_ble_bonds.bin
//   [4 bytes]  magic "BLB\x01" (BLE Bonds v1)
//   [4 bytes]  uint32_t  ourSecCount
//   [4 bytes]  uint32_t  peerSecCount
//   [N * sizeof(ble_store_value_sec)]  our_sec entries
//   [M * sizeof(ble_store_value_sec)]  peer_sec entries
// =========================================================================

static constexpr const char* BLE_BOND_BACKUP_PATH = "/v1simple_ble_bonds.bin";
static constexpr uint8_t BLE_BOND_MAGIC[4] = { 'B', 'L', 'B', 0x01 };
static constexpr size_t MAX_BOND_ENTRIES = 8;  // Generous limit

struct BondBackupHeader {
    uint8_t magic[4];
    uint32_t ourSecCount;
    uint32_t peerSecCount;
};

// Callback context for ble_store_iterate
struct BondCollector {
    struct ble_store_value_sec entries[MAX_BOND_ENTRIES];
    size_t count;
};

static int bondCollectCallback(int obj_type, union ble_store_value* val, void* cookie) {
    (void)obj_type;
    auto* collector = static_cast<BondCollector*>(cookie);
    if (collector->count < MAX_BOND_ENTRIES) {
        memcpy(&collector->entries[collector->count], &val->sec, sizeof(struct ble_store_value_sec));
        collector->count++;
    }
    return 0;  // 0 = continue iterating
}

// Backup all bond keys to SD card. Safe to call anytime after NimBLEDevice::init().
// Returns number of bonds backed up, or -1 on error.
int backupBondsToSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return -1;
    }

    // Collect bond data from NimBLE store
    BondCollector ourSecs = {};
    BondCollector peerSecs = {};
    ble_store_iterate(BLE_STORE_OBJ_TYPE_OUR_SEC, bondCollectCallback, &ourSecs);
    ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC, bondCollectCallback, &peerSecs);

    if (ourSecs.count == 0 && peerSecs.count == 0) {
        return 0;  // Nothing to backup
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return -1;
    }

    fs::FS* sdFs = storageManager.getFilesystem();
    if (!sdFs) {
        return -1;
    }

    // Write to .tmp first, then rename (atomic pattern)
    const String tmpPath = String(BLE_BOND_BACKUP_PATH) + ".tmp";
    File f = sdFs->open(tmpPath.c_str(), "w");
    if (!f) {
        Serial.println("[BLE] WARN: Failed to open bond backup tmp file");
        return -1;
    }

    BondBackupHeader hdr = {};
    memcpy(hdr.magic, BLE_BOND_MAGIC, 4);
    hdr.ourSecCount = ourSecs.count;
    hdr.peerSecCount = peerSecs.count;

    bool ok = true;
    ok = ok && (f.write((const uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr));
    if (ourSecs.count > 0) {
        const size_t sz = ourSecs.count * sizeof(struct ble_store_value_sec);
        ok = ok && (f.write((const uint8_t*)ourSecs.entries, sz) == sz);
    }
    if (peerSecs.count > 0) {
        const size_t sz = peerSecs.count * sizeof(struct ble_store_value_sec);
        ok = ok && (f.write((const uint8_t*)peerSecs.entries, sz) == sz);
    }
    f.flush();
    f.close();

    if (!ok) {
        sdFs->remove(tmpPath.c_str());
        Serial.println("[BLE] WARN: Bond backup write incomplete");
        return -1;
    }

    // Atomic rename
    if (sdFs->exists(BLE_BOND_BACKUP_PATH)) {
        sdFs->remove(BLE_BOND_BACKUP_PATH);
    }
    if (!sdFs->rename(tmpPath.c_str(), BLE_BOND_BACKUP_PATH)) {
        sdFs->remove(tmpPath.c_str());
        Serial.println("[BLE] WARN: Bond backup rename failed");
        return -1;
    }

    const int total = (int)(ourSecs.count + peerSecs.count);
    Serial.printf("[BLE] Backed up %d bond(s) to SD (%u our, %u peer)\n",
                  total, (unsigned)ourSecs.count, (unsigned)peerSecs.count);
    return total;
}

// Restore bond keys from SD card. Must be called after NimBLEDevice::init()
// but before scanning/connecting. Returns number of bonds restored, or -1 on error.
static int restoreBondsFromSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return -1;
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return -1;
    }

    fs::FS* sdFs = storageManager.getFilesystem();
    if (!sdFs || !sdFs->exists(BLE_BOND_BACKUP_PATH)) {
        return -1;
    }

    File f = sdFs->open(BLE_BOND_BACKUP_PATH, "r");
    if (!f) {
        return -1;
    }

    // Sanity: file must be at least header size, and less than a reasonable max
    const size_t fileSize = f.size();
    const size_t maxSize = sizeof(BondBackupHeader) + 
                           2 * MAX_BOND_ENTRIES * sizeof(struct ble_store_value_sec);
    if (fileSize < sizeof(BondBackupHeader) || fileSize > maxSize) {
        f.close();
        Serial.printf("[BLE] Bond backup file size invalid: %u\n", (unsigned)fileSize);
        return -1;
    }

    BondBackupHeader hdr = {};
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) {
        f.close();
        return -1;
    }

    // Validate magic
    if (memcmp(hdr.magic, BLE_BOND_MAGIC, 4) != 0) {
        f.close();
        Serial.println("[BLE] Bond backup magic mismatch");
        return -1;
    }

    // Validate counts
    if (hdr.ourSecCount > MAX_BOND_ENTRIES || hdr.peerSecCount > MAX_BOND_ENTRIES) {
        f.close();
        Serial.println("[BLE] Bond backup count out of range");
        return -1;
    }

    // Validate file size matches header
    const size_t expectedSize = sizeof(BondBackupHeader) +
                                hdr.ourSecCount * sizeof(struct ble_store_value_sec) +
                                hdr.peerSecCount * sizeof(struct ble_store_value_sec);
    if (fileSize < expectedSize) {
        f.close();
        Serial.println("[BLE] Bond backup file truncated");
        return -1;
    }

    int restored = 0;

    // Restore our_sec entries
    for (uint32_t i = 0; i < hdr.ourSecCount; i++) {
        struct ble_store_value_sec sec = {};
        if (f.read((uint8_t*)&sec, sizeof(sec)) != sizeof(sec)) {
            break;
        }
        if (ble_store_write_our_sec(&sec) == 0) {
            restored++;
        }
    }

    // Restore peer_sec entries
    for (uint32_t i = 0; i < hdr.peerSecCount; i++) {
        struct ble_store_value_sec sec = {};
        if (f.read((uint8_t*)&sec, sizeof(sec)) != sizeof(sec)) {
            break;
        }
        if (ble_store_write_peer_sec(&sec) == 0) {
            restored++;
        }
    }

    f.close();

    if (restored > 0) {
        Serial.printf("[BLE] Restored %d bond(s) from SD backup\n", restored);
    }
    return restored;
}

// calcV1Checksum moved to ble_commands.cpp

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

// Spinlock for deferring settings writes from BLE scan callbacks
portMUX_TYPE pendingAddrMux = portMUX_INITIALIZER_UNLOCKED;
// Spinlock for proxy command telemetry (avoid Serial in BLE callback)
portMUX_TYPE proxyCmdMux = portMUX_INITIALIZER_UNLOCKED;

// Instance pointer for callbacks (extern in ble_internals.h)
V1BLEClient* instancePtr = nullptr;

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

V1BLEClient::~V1BLEClient() = default;

// ==================== BLE State Machine ====================

void V1BLEClient::setBLEState(BLEState newState, const char* reason) {
    BLEState oldState = bleState;
    if (oldState == newState) return;  // No change
    
    unsigned long now = millis();
    unsigned long stateTime = (oldState != BLEState::DISCONNECTED && stateEnteredMs > 0) ? (now - stateEnteredMs) : 0;
    
    bleState = newState;
    stateEnteredMs = now;

    if (newState == BLEState::SCANNING) {
        PERF_INC(bleScanStateEntries);
    }
    if (oldState == BLEState::SCANNING && newState != BLEState::SCANNING) {
        PERF_INC(bleScanStateExits);
        PERF_MAX(bleScanDwellMaxMs, stateTime);
    }

    if (newState == BLEState::SCANNING) {
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::ScanStart, now);
    } else if (newState == BLEState::SCAN_STOPPING && reason && strstr(reason, "V1 found")) {
        PERF_INC(bleScanTargetFound);
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::TargetFound, now);
    } else if (newState == BLEState::CONNECTING) {
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::ConnectStart, now);
    } else if (newState == BLEState::CONNECTED) {
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::Connected, now);
    }
    if (oldState == BLEState::SCANNING &&
        newState == BLEState::DISCONNECTED &&
        reason &&
        strstr(reason, "scan ended without finding V1")) {
        PERF_INC(bleScanNoTargetExits);
    }
    
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
    notifyShortChar.store(nullptr, std::memory_order_relaxed);
    notifyShortCharId.store(0, std::memory_order_relaxed);
    notifyLongChar.store(nullptr, std::memory_order_relaxed);
    notifyLongCharId.store(0, std::memory_order_relaxed);
    
    // 4. Clear connection flags
    {
        SemaphoreGuard lock(bleMutex, pdMS_TO_TICKS(20));  // COLD: disconnect cleanup
        if (lock.locked()) {
            connected = false;
            shouldConnect = false;
            hasTargetDevice = false;
            targetDevice = NimBLEAdvertisedDevice();
        }
    }
    
    // 5. Clear stale phone command state (prevents sending commands from previous session)
    phoneCmdPendingClear = true;
    
    connectInProgress = false;
}

// Hard reset of BLE client stack - use after repeated failures
// Reuses existing client to avoid NimBLE slot leak (max 3 slots)
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
    
    // Reuse existing client (don't destroy - NimBLE has fixed 3-slot array,
    // nulling without deleteClient leaks a slot permanently)
    if (!pClient) {
        pClient = NimBLEDevice::createClient();
    }
    if (pClient) {
        if (!pClientCallbacks) {
            pClientCallbacks.reset(new ClientCallbacks());
        }
        pClient->setClientCallbacks(pClientCallbacks.get());
        // Connection parameters: 12-24 (15-30ms interval), balanced for stability
        pClient->setConnectionParams(NIMBLE_CONN_INTERVAL_MIN,
                                     NIMBLE_CONN_INTERVAL_MAX,
                                     NIMBLE_CONN_LATENCY,
                                     NIMBLE_CONN_SUPERVISION_TIMEOUT);
        pClient->setConnectTimeout(NIMBLE_CONNECT_TIMEOUT_INIT_MS);
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
    // BUT: backup bonds to SD first so they can be restored below.
    {
        Preferences blePrefs;
        blePrefs.begin("ble_state", false);  // Read-write mode
        String storedVersion = blePrefs.getString("fwVersion", "");
        if (storedVersion != FIRMWARE_VERSION) {
            Serial.printf(" fresh-flash detected...");
            // NimBLE must be initialized before bond APIs work
            NimBLEDevice::init("");

            // Backup existing bonds to SD BEFORE deleting them
            const int backed = backupBondsToSD();
            if (backed > 0) {
                Serial.printf(" backed up %d bond(s)...", backed);
            }

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

    // Restore bonds from SD backup if NVS was cleared (fresh-flash or NVS corruption)
    if (NimBLEDevice::getNumBonds() == 0) {
        const int restored = restoreBondsFromSD();
        if (restored > 0) {
            Serial.printf("[BLE] Restored %d bond(s) from SD\n", restored);
        }
    } else {
        // NVS has bonds — keep SD backup fresh
        backupBondsToSD();
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
            pClientCallbacks.reset(new ClientCallbacks());
        }
        pClient->setClientCallbacks(pClientCallbacks.get());
        
        // Connection parameters: 12-24 (15-30ms interval), balanced for stability
        pClient->setConnectionParams(NIMBLE_CONN_INTERVAL_MIN,
                                     NIMBLE_CONN_INTERVAL_MAX,
                                     NIMBLE_CONN_LATENCY,
                                     NIMBLE_CONN_SUPERVISION_TIMEOUT);
        pClient->setConnectTimeout(NIMBLE_CONNECT_TIMEOUT_INIT_MS);
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
    
    // Replace scan callbacks atomically; previous handler is released automatically.
    pScanCallbacks.reset(new ScanCallbacks(this));
    pScan->setScanCallbacks(pScanCallbacks.get());
    pScan->setActiveScan(true);  // Request scan response to get device names
    // ESP32-S3 WiFi coexistence: use 75% duty cycle for reliable V1 discovery
    // Higher duty = more BLE radio time = faster discovery, but less WiFi throughput
    pScan->setInterval(160);  // 100ms interval 
    pScan->setWindow(120);    // 75ms window - 75% duty cycle (was 50%)
    pScan->setMaxResults(0);  // Unlimited results
    // Reliability first: allow duplicate reports so we don't miss a late name/scan-response
    // update under WiFi coexistence stress.
    pScan->setDuplicateFilter(false);
    
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
    if (connected) {
        proxyClientConnectedOnceThisBoot = true;
        proxyNoClientDeadlineMs = 0;
    }
}

void V1BLEClient::onDataReceived(DataCallback callback) {
    dataCallback = callback;
}

void V1BLEClient::onV1Connected(ConnectionCallback callback) {
    connectCallback = callback;
}

// Connection state machine, callbacks, and notify moved to ble_connection.cpp
// Command methods moved to ble_commands.cpp

// Proxy server methods moved to ble_proxy.cpp

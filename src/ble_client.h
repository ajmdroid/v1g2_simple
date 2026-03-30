/**
 * BLE Client for Valentine1 Gen2
 * Handles connection and data reception from V1 over BLE
 * Also supports BLE Server mode for proxying to companion apps
 */

#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <array>
#include <vector>
#include <atomic>
#include <memory>

#include "include/obd_ble_arbitration.h"

// Forward declarations
class V1BLEClient;

// Callback for receiving V1 display data packets
// data: pointer to packet data
// length: number of bytes
// charUUID: last 16-bit of source characteristic UUID (0xB2CE, 0xB4E0, etc)
typedef void (*DataCallback)(const uint8_t* data, size_t length, uint16_t charUUID);

// Callback for V1 connection events
typedef void (*ConnectionCallback)();

// BLE Connection State Machine
// Centralized state to prevent overlapping operations and race conditions
// Connection phases are broken into discrete states for non-blocking operation
enum class BLEState {
    DISCONNECTED,      // Not connected, not doing anything
    SCANNING,          // Actively scanning for V1
    SCAN_STOPPING,     // Scan stop requested, waiting for settle
    CONNECTING,        // Connection attempt initiated (async)
    CONNECTING_WAIT,   // Waiting for async connect callback
    DISCOVERING,       // Service discovery in progress (uses cached handles if available)
    SUBSCRIBING,       // Subscribing to characteristics (multi-step, non-blocking)
    SUBSCRIBE_YIELD,   // Yielding between subscribe steps to allow loop() to run
    CONNECTED,         // Successfully connected to V1
    BACKOFF            // Failed connection, waiting before retry
};

// Convert BLEState to string for logging
inline const char* bleStateToString(BLEState state) {
    switch (state) {
        case BLEState::DISCONNECTED: return "DISCONNECTED";
        case BLEState::SCANNING: return "SCANNING";
        case BLEState::SCAN_STOPPING: return "SCAN_STOPPING";
        case BLEState::CONNECTING: return "CONNECTING";
        case BLEState::CONNECTING_WAIT: return "CONNECTING_WAIT";
        case BLEState::DISCOVERING: return "DISCOVERING";
        case BLEState::SUBSCRIBING: return "SUBSCRIBING";
        case BLEState::SUBSCRIBE_YIELD: return "SUBSCRIBE_YIELD";
        case BLEState::CONNECTED: return "CONNECTED";
        case BLEState::BACKOFF: return "BACKOFF";
        default: return "UNKNOWN";
    }
}

inline uint8_t bleStateToCode(BLEState state) {
    return static_cast<uint8_t>(state);
}

// sendCommand result codes for proper retry semantics
enum class SendResult {
    SENT,       // Command successfully sent
    NOT_YET,    // Pacing: too soon, caller should retry
    FAILED      // Hard failure: drop and count
};

// Proxy metrics for monitoring proxy health
struct ProxyMetrics {
    uint32_t sendCount = 0;          // Successful notify sends
    uint32_t dropCount = 0;          // Dropped due to queue full
    uint32_t errorCount = 0;         // Notify failures
    uint32_t queueHighWater = 0;     // Max queue depth seen
    uint32_t lastResetMs = 0;        // When metrics were last reset
    
    void reset() {
        sendCount = 0;
        dropCount = 0;
        errorCount = 0;
        queueHighWater = 0;
        lastResetMs = millis();
    }
};

class V1BLEClient {
public:
    enum class SubscribeStepCode : uint8_t {
        GET_SERVICE = 0,
        GET_DISPLAY_CHAR = 1,
        GET_COMMAND_CHAR = 2,
        GET_COMMAND_LONG = 3,
        SUBSCRIBE_DISPLAY = 4,
        WRITE_DISPLAY_CCCD = 5,
        GET_DISPLAY_LONG = 6,
        SUBSCRIBE_LONG = 7,
        WRITE_LONG_CCCD = 8,
        REQUEST_ALERT_DATA = 9,
        REQUEST_VERSION = 10,
        COMPLETE = 11,
    };

    V1BLEClient();
    ~V1BLEClient();
    
    // Initialize BLE stack only (no scanning)
    bool initBLE(bool enableProxy = false, const char* proxyName = "V1C-LE-S3");
    
    // Initialize BLE and start scanning
    // If enableProxy is true, also starts BLE server for app connections
    bool begin(bool enableProxy = false, const char* proxyName = "V1C-LE-S3");
    
    // Check connection status
    bool isConnected();
    
    // Get RSSI of connected V1 device (returns 0 if not connected)
    int getConnectionRssi();
    
    // Get RSSI of connected proxy client (app) (returns 0 if not connected)
    int getProxyClientRssi();
    
    // Check if proxy client (app) is connected
    bool isProxyClientConnected();
    
    // Check if BLE proxy is enabled
    bool isProxyEnabled() const { return proxyEnabled; }
    
    // Check if proxy is actively advertising (only true after V1 connects)
    bool isProxyAdvertising() const;

    void setObdBleArbitrationRequest(ObdBleArbitrationRequest request);
    
    // Debug/test control: force proxy advertising on/off at runtime.
    bool forceProxyAdvertising(bool enable, uint8_t reasonCode = 0);

    // Set proxy client connection status (for internal callback use)
    void setProxyClientConnected(bool connected);
    
    // Register callback for received data
    void onDataReceived(DataCallback callback);

    // Register callback for immediate V1 connect work (critical path only).
    void onV1ConnectImmediate(ConnectionCallback callback);

    // Register callback for stable V1 connection work after the connect burst settles.
    void onV1Connected(ConnectionCallback callback);

    // Record latest loop timings used by the connect-burst settle gate.
    void noteBleProcessDuration(uint32_t us);
    void noteDisplayPipelineDuration(uint32_t us);
    bool isConnectBurstSettling() const;
    
    // Send command to V1 (e.g., request alert data)
    bool sendCommand(const uint8_t* data, size_t length);
    
    // Send command with detailed result for retry logic
    SendResult sendCommandWithResult(const uint8_t* data, size_t length);
    
    // Request V1 to start sending alert data
    bool requestAlertData();
    
    // Request V1 version information (triggers data on B4E0)
    bool requestVersion();
    
    // Turn V1 display on/off (dark mode)
    bool setDisplayOn(bool on);
    
    // Send mute on/off command
    bool setMute(bool muted);
    
    // Change V1 operating mode (All Bogeys, Logic, Advanced Logic)
    bool setMode(uint8_t mode);
    
    // Set V1 volume settings (0-9 for each, 0xFF to keep current)
    bool setVolume(uint8_t mainVolume, uint8_t mutedVolume);
    
    // Request user settings bytes from V1 (6 bytes)
    bool requestUserBytes();
    
    // Write user settings bytes to V1 (6 bytes)
    bool writeUserBytes(const uint8_t* bytes);
    
    // Write user settings with optional verification (verification disabled - see implementation)
    enum WriteVerifyResult { VERIFY_OK = 0, VERIFY_WRITE_FAILED = 1, VERIFY_TIMEOUT = 2, VERIFY_MISMATCH = 3 };
    WriteVerifyResult writeUserBytesVerified(const uint8_t* bytes, int maxRetries = 2);

    // Prepare verification of user bytes on next read-back
    void startUserBytesVerification(const uint8_t* expected);
    
    // Called by main loop when RESP_USER_BYTES received to complete verification
    void onUserBytesReceived(const uint8_t* bytes);
    
    // Disconnect and cleanup
    void disconnect();
    
    // Full cleanup of BLE connection state (clears characteristic refs, unsubscribes)
    void cleanupConnection();
    
    // Hard reset of BLE client stack after repeated failures
    void hardResetBLEClient();
    
    // Process BLE events (call in loop)
    void process();

    // Retry deferred bond backup work outside the ingest phase.
    void serviceDeferredBondBackup(uint32_t nowMs);
    
    // Restart scanning for V1
    void startScanning();

    // Check if currently scanning
    bool isScanning();
    
    // Get current BLE state (for diagnostics)
    BLEState getBLEState() const { return bleState; }
    uint8_t getBLEStateCode() const { return bleStateToCode(bleState); }
    bool isConnectInProgress() const { return connectInProgress; }
    bool isAsyncConnectPending() const { return asyncConnectPending.load(std::memory_order_relaxed); }
    bool hasPendingDisconnectCleanup() const {
        return pendingDisconnectCleanup.load(std::memory_order_relaxed);
    }
    uint8_t getSubscribeStepCode() const { return static_cast<uint8_t>(subscribeStep); }
    const char* getSubscribeStepName() const;
    
    // Get the connected V1's BLE address
    NimBLEAddress getConnectedAddress() const;
    
    // Forward data to proxy clients (queues data for async send)
    // sourceCharUUID: last 16-bit of source characteristic UUID (0xB2CE, 0xB4E0, etc)
    void forwardToProxy(const uint8_t* data, size_t length, uint16_t sourceCharUUID);
    
    // Process pending proxy notifications (call from main loop after display update)
    // Returns number of packets sent
    int processProxyQueue();

    // Phone->V1 command drop counters (observability)
    uint32_t getPhoneCmdDropsOverflow() const;
    uint32_t getPhoneCmdDropsInvalid() const;  // Malformed packets
    uint32_t getPhoneCmdDropsBleFail() const;  // Hard BLE failures
    uint32_t getPhoneCmdDropsLockBusy() const;
    
    // Get proxy metrics (for instrumentation)
    const ProxyMetrics& getProxyMetrics() const { return proxyMetrics; }
    
    // Reset proxy notify/send metrics only; phone command drop counters reset with perfMetricsReset().
    void resetProxyMetrics() { proxyMetrics.reset(); }
    
    // WiFi priority mode - deprioritize BLE when web UI is active
    void setWifiPriority(bool enabled);  // Enable = suppress BLE activity
    bool isWifiPriority() const { return wifiPriorityMode; }

    // Boot readiness gate - blocks BLE scan/connect state machine until setup is ready
    void setBootReady(bool ready);
    bool isBootReady() const { return bootReadyFlag; }

private:
    // Nested callback classes - defined before member declarations that use them
    class ClientCallbacks : public NimBLEClientCallbacks {
    public:
        void onConnect(NimBLEClient* pClient) override;
        void onDisconnect(NimBLEClient* pClient, int reason) override;
        void onPhyUpdate(NimBLEClient* pClient, uint8_t txPhy, uint8_t rxPhy) override;
    };
    
    // NimBLE 2.x uses NimBLEScanCallbacks
    class ScanCallbacks : public NimBLEScanCallbacks {
    public:
        ScanCallbacks(V1BLEClient* client) : bleClient(client) {}
        void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
        void onScanEnd(const NimBLEScanResults& scanResults, int reason) override;
    private:
        V1BLEClient* bleClient;
    };
    
    class ProxyServerCallbacks : public NimBLEServerCallbacks {
    public:
        ProxyServerCallbacks(V1BLEClient* client) : bleClient(client) {}
        void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override;
        void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override;
    private:
        V1BLEClient* bleClient;
    };
    
    class ProxyWriteCallbacks : public NimBLECharacteristicCallbacks {
    public:
        ProxyWriteCallbacks(V1BLEClient* client) : bleClient(client) {}
        void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;
    private:
        V1BLEClient* bleClient;
    };

    enum class ProxyCallbackEventType : uint8_t {
        APP_CONNECTED = 0,
        APP_DISCONNECTED = 1,
        V1_DISCONNECTED = 2,
    };

    struct ProxyCallbackEvent {
        ProxyCallbackEventType type = ProxyCallbackEventType::APP_CONNECTED;
        uint16_t connHandle = 0;
        int reason = 0;
    };

    NimBLEClient* pClient;
    NimBLERemoteService* pRemoteService;
    NimBLERemoteCharacteristic* pDisplayDataChar;
    NimBLERemoteCharacteristic* pCommandChar;
    NimBLERemoteCharacteristic* pCommandCharLong;  // B8D2 - for long commands like voltage request
    // Cached notify characteristic -> short UUID mapping for callback hot path.
    // Written during subscribe steps; read from notify callback without locking.
    std::atomic<NimBLERemoteCharacteristic*> notifyShortChar{nullptr};
    std::atomic<uint16_t> notifyShortCharId{0};
    std::atomic<NimBLERemoteCharacteristic*> notifyLongChar{nullptr};
    std::atomic<uint16_t> notifyLongCharId{0};
    
    // BLE Server (proxy) objects
    NimBLEServer* pServer;
    NimBLEService* pProxyService;
    NimBLECharacteristic* pProxyNotifyChar;     // B2CE proxy - short display data
    NimBLECharacteristic* pProxyNotifyLongChar; // B4E0 proxy - long alert/response data
    NimBLECharacteristic* pProxyWriteChar;
    bool proxyEnabled;
    bool proxyServerInitialized;
    std::atomic<bool> proxyClientConnected{false}; // Atomic for thread safety (set from BLE callbacks)
    String proxyName_;
    
    // Synchronization primitives (mirroring Kenny's approach)
    SemaphoreHandle_t bleMutex = nullptr;
    SemaphoreHandle_t bleNotifyMutex = nullptr;
    SemaphoreHandle_t phoneCmdMutex = nullptr;
    
    // Proxy queue for decoupling notify from hot path
    static constexpr size_t PROXY_QUEUE_SIZE = 8;  // Small queue, drop-oldest on overflow
    static constexpr size_t PROXY_PACKET_MAX = 512; // Max packet size for proxy (handles full V1 packets)
    struct ProxyPacket {
        uint8_t data[PROXY_PACKET_MAX];
        size_t length;
        uint16_t charUUID;
        uint32_t tsMs;
    };
    ProxyPacket* proxyQueue = nullptr;

    // Phone→V1 command queue for safe writes (decoupled from callback context)
    static constexpr size_t PHONE_CMD_QUEUE_SIZE = 16;  // Small burst-tolerant queue for phone commands
    static constexpr size_t MAX_PHONE_CMDS_PER_LOOP = 4;
    ProxyPacket* phone2v1Queue = nullptr;
    bool proxyQueuesInPsram = false;
    volatile size_t phone2v1QueueHead = 0;
    volatile size_t phone2v1QueueTail = 0;
    volatile size_t phone2v1QueueCount = 0;
    volatile size_t proxyQueueHead = 0;  // Next write position
    volatile size_t proxyQueueTail = 0;  // Next read position
    volatile size_t proxyQueueCount = 0; // Current items in queue
    ProxyMetrics proxyMetrics;
    
    DataCallback dataCallback;
    ConnectionCallback connectImmediateCallback;
    ConnectionCallback connectStableCallback;
    std::atomic<bool> connected{false};      // Standalone connection flag; use atomic load/store for all direct accesses
    std::atomic<bool> shouldConnect{false};  // Atomic for thread safety (set from BLE callbacks)
    std::atomic<bool> pendingConnectStateUpdate{false};   // Deferred update from BLE callbacks
    std::atomic<bool> pendingDisconnectCleanup{false};    // Deferred cleanup from BLE callbacks
    std::atomic<bool> pendingDeleteBond{false};             // Deferred bond deletion from BLE callback
    NimBLEAddress pendingDeleteBondAddr;                    // Address to delete bond for
    std::atomic<bool> pendingLastV1AddressValid{false};    // Deferred settings save from BLE scan callback
    char pendingLastV1Address[18] = {0};                  // "AA:BB:CC:DD:EE:FF" + null
    std::atomic<bool> pendingScanEndUpdate{false};         // Deferred scan-end state update from BLE callback
    std::atomic<bool> pendingScanTargetUpdate{false};      // Deferred target update from BLE scan callback
    std::atomic<bool> phoneCmdPendingClear{false};         // Clear stale phone cmd state on reconnect
    static constexpr size_t PROXY_CALLBACK_EVENT_QUEUE_DEPTH = 8;
    std::array<ProxyCallbackEvent, PROXY_CALLBACK_EVENT_QUEUE_DEPTH> proxyCallbackEventQueue_{};
    size_t proxyCallbackEventQueueHead_ = 0;
    size_t proxyCallbackEventQueueCount_ = 0;
    portMUX_TYPE proxyCallbackEventMux_ = portMUX_INITIALIZER_UNLOCKED;

    // Async discovery task (avoids ~2s block on main loop)
    std::atomic<bool> discoveryTaskRunning{false};
    std::atomic<bool> discoveryTaskDone{false};
    std::atomic<bool> discoveryTaskResult{false};
    static void discoveryTaskFunc(void* param);

    char pendingScanTargetAddress[18] = {0};               // "AA:BB:CC:DD:EE:FF" + null
    uint8_t pendingScanTargetAddressType = BLE_ADDR_PUBLIC;
    bool hasTargetDevice = false;
    NimBLEAdvertisedDevice targetDevice;
    NimBLEAddress targetAddress;
    uint8_t targetAddressType = BLE_ADDR_PUBLIC;  // Saved from advertisement
    unsigned long lastScanStart;
    
    // BLE State Machine - centralized connection state
    BLEState bleState = BLEState::DISCONNECTED;
    unsigned long stateEnteredMs = 0;       // When current state was entered
    unsigned long scanStopRequestedMs = 0;  // When scan stop was requested
    bool scanStopResultsCleared_ = false;   // One-shot clearResults gate for SCAN_STOPPING
    // ESP32-S3 BLE: radio needs time after scan to be ready for connect
    // Cold boot needs significantly more time for NimBLE stack to stabilize
    static constexpr unsigned long SCAN_STOP_SETTLE_MS = 100;        // 100ms settle for reconnects
    static constexpr unsigned long SCAN_STOP_SETTLE_FRESH_MS = 200;  // 200ms on cold boot - tuned lower for faster first connect
    bool firstScanAfterBoot = true;  // Use longer settle on first scan
    
    // Connection attempt guard - prevents overlapping attempts
    bool connectInProgress = false;
    unsigned long connectStartMs = 0;  // When connect started (for stuck detection)
    
    // Async connection tracking
    std::atomic<bool> asyncConnectPending{false};   // Async connect in progress
    std::atomic<bool> asyncConnectSuccess{false};   // Result from onConnect callback
    uint8_t connectAttemptNumber = 0;               // Current attempt (1-based)
    static constexpr uint8_t MAX_CONNECT_ATTEMPTS = 5;          // 5 attempts - more retries
    static constexpr uint16_t NIMBLE_CONN_INTERVAL_MIN = 12;
    static constexpr uint16_t NIMBLE_CONN_INTERVAL_MAX = 24;
    static constexpr uint16_t NIMBLE_CONN_LATENCY = 0;
    static constexpr uint16_t NIMBLE_CONN_SUPERVISION_TIMEOUT = 400;
    static constexpr unsigned long CONNECT_TIMEOUT_MS = 3000;   // 3s timeout - if it works, it's fast
    // NimBLE timeout API is milliseconds. Align client-level timeout with the state-machine budget.
    static constexpr unsigned long NIMBLE_CONNECT_TIMEOUT_INIT_MS = CONNECT_TIMEOUT_MS;
    static constexpr unsigned long NIMBLE_CONNECT_TIMEOUT_ACTIVE_MS = CONNECT_TIMEOUT_MS;
    static constexpr unsigned long DISCOVERY_TIMEOUT_MS = 5000; // 5s for discovery
    static constexpr unsigned long SUBSCRIBE_TIMEOUT_MS = 3000;  // 3s for subscriptions
    uint32_t connectPhaseStartUs = 0;  // For timing individual phases
    
    // Fresh flash detection - set when firmware version changed
    bool freshFlashBoot = false;
    // Tracks the bond-count snapshot that has already been persisted to SD.
    // 0xFF means unknown/uninitialized.
    uint8_t lastBondBackupCount = 0xFF;
    bool pendingBondBackup = false;
    uint8_t pendingBondBackupCount = 0xFF;
    uint32_t pendingBondBackupRetryAtMs = 0;
    static constexpr uint32_t DEFERRED_BOND_BACKUP_RETRY_MS = 1000;
    
    // Non-blocking subscribe step machine
    // Each step does one BLE operation then yields to loop()
    enum class SubscribeStep {
        GET_SERVICE,           // Get V1 service reference
        GET_DISPLAY_CHAR,      // Get B2CE display data characteristic
        GET_COMMAND_CHAR,      // Get command write characteristic
        GET_COMMAND_LONG,      // Get B8D2 long command characteristic
        SUBSCRIBE_DISPLAY,     // Subscribe to B2CE notifications
        WRITE_DISPLAY_CCCD,    // Force-write CCCD for B2CE
        GET_DISPLAY_LONG,      // Get B4E0 characteristic
        SUBSCRIBE_LONG,        // Subscribe to B4E0 notifications
        WRITE_LONG_CCCD,       // Force-write CCCD for B4E0
        REQUEST_ALERT_DATA,    // Send alert data request
        REQUEST_VERSION,       // Send version request
        COMPLETE               // All steps done
    };
    enum class ConnectedFollowupStep {
        NONE,
        REQUEST_ALERT_DATA,
        WAIT_CONNECT_BURST_SETTLE,
        REQUEST_VERSION,
        NOTIFY_STABLE_CALLBACK,
        SCHEDULE_PROXY_ADVERTISING,
        BACKUP_BONDS,
    };
    SubscribeStep subscribeStep = SubscribeStep::GET_SERVICE;
    ConnectedFollowupStep connectedFollowupStep = ConnectedFollowupStep::NONE;
    uint32_t subscribeStepStartUs = 0;    // When current step started
    uint32_t subscribeYieldUntilMs = 0;   // When to resume from SUBSCRIBE_YIELD
    static constexpr uint32_t SUBSCRIBE_STEP_BUDGET_US = 50000;  // 50ms per step max
    static constexpr uint32_t SUBSCRIBE_YIELD_MS = 5;            // 5ms yield between steps
    static constexpr uint32_t CONNECT_BURST_STABLE_BLE_MAX_US = 25000;
    static constexpr uint32_t CONNECT_BURST_STABLE_DISP_MAX_US = 50000;
    static constexpr uint8_t CONNECT_BURST_STABLE_CONSECUTIVE_LOOPS = 3;
    static constexpr uint32_t CONNECT_BURST_SETTLE_AFTER_FIRST_RX_MS = 1500;
    static constexpr uint32_t CONNECT_BURST_SETTLE_AFTER_CONNECTED_MS = 2500;
    std::atomic<uint32_t> connectCompletedAtMs{0};
    std::atomic<uint32_t> firstRxAfterConnectMs{0};
    std::atomic<uint32_t> lastBleProcessDurationUs{0};
    std::atomic<uint32_t> lastDisplayPipelineDurationUs{0};
    uint8_t connectBurstStableLoopCount = 0;

    // Async connect step functions
    bool startAsyncConnect();         // Initiate async connect
    void processConnectingWait();     // Handle CONNECTING_WAIT state
    void processDiscovering();        // Handle DISCOVERING state  
    void processSubscribing();        // Handle SUBSCRIBING state (step machine)
    void processSubscribeYield();     // Handle SUBSCRIBE_YIELD state
    void processConnectedFollowup();  // Spread post-connect work across loop turns
    int tryBackupBondsToSD();         // Non-blocking bond backup for deferred maintenance
    bool executeSubscribeStep();      // Execute one subscribe step, return true if done
    
    // Called from connectToServer() after successful sync connect
    bool finishConnection();

    // Queue phone->V1 commands from BLE callback context
    bool enqueuePhoneCommand(const uint8_t* data, size_t length, uint16_t sourceCharUUID);
    int processPhoneCommandQueue();
    // Diagnostic helper to log negotiated connection parameters
    void logConnParams(const char* tag);
    
    // State transition helper
    void setBLEState(BLEState newState, const char* reason);

    // Defer settings writes from BLE scan callback
    void deferLastV1Address(const char* addr);
    
    // Exponential backoff for connection failures (error 13 = BLE_HS_EBUSY)
    uint8_t consecutiveConnectFailures = 0;
    unsigned long nextConnectAllowedMs = 0;  // Backoff until this time
    
    // Deferred proxy advertising start (non-blocking - avoids stall)
    // Tuned lower to reduce post-connect latency while preserving radio settle margin
    unsigned long proxyAdvertisingStartMs = 0;  // When to start advertising (0 = not pending)
    uint8_t proxyAdvertisingStartReasonCode = 0; // PerfProxyAdvertisingTransitionReason
    static constexpr unsigned long PROXY_STABILIZE_MS = 100;
    // When STA is connected, limit idle proxy advertising windows to reduce radio churn.
    unsigned long proxyAdvertisingWindowStartMs = 0;
    unsigned long proxyAdvertisingRetryAtMs = 0;
    static constexpr unsigned long PROXY_ADVERTISING_WINDOW_MS = 60000;
    static constexpr unsigned long PROXY_ADVERTISING_RETRY_MS = 120000;
    // If no proxy client connects within this boot-time window, disable proxy
    // advertising for the rest of the boot to protect DMA headroom.
    static constexpr unsigned long PROXY_NO_CLIENT_TIMEOUT_MS = 120000;  // 2 minutes
    unsigned long proxyNoClientDeadlineMs = 0;
    bool proxyNoClientTimeoutLatched = false;
    bool proxyClientConnectedOnceThisBoot = false;
    bool proxySuppressedForObdHold_ = false;
    bool proxyDisconnectRequestedForObdPreempt_ = false;
    uint8_t proxySuppressedResumeReasonCode_ = 0;
    ObdBleArbitrationRequest obdBleArbitrationRequest_ = ObdBleArbitrationRequest::NONE;
    
    // Write verification state
    bool verifyPending = false;
    uint8_t verifyExpected[6] = {0};
    uint8_t verifyReceived[6] = {0};
    bool verifyComplete = false;
    bool verifyMatch = false;

    // Proxy command telemetry (logs in main loop, not in BLE callback)
    bool proxyCmdPending = false;
    uint8_t proxyCmdId = 0;
    uint8_t proxyCmdLen = 0;
    uint8_t proxyCmdBuf[8] = {0};

    // Callback handlers are RAII-owned to prevent manual delete mistakes.
    std::unique_ptr<ScanCallbacks> pScanCallbacks;
    std::unique_ptr<ClientCallbacks> pClientCallbacks;
    std::unique_ptr<ProxyServerCallbacks> pProxyServerCallbacks;
    std::unique_ptr<ProxyWriteCallbacks> pProxyWriteCallbacks;
    
    // WiFi priority mode flag
    bool wifiPriorityMode = false;
    bool bootReadyFlag = false;
    
    // Initialize BLE server for proxy mode
    bool initProxyServer(const char* deviceName);
    bool allocateProxyQueues();
    void releaseProxyQueues();
    bool enqueueProxyCallbackEvent(const ProxyCallbackEvent& event);
    bool popProxyCallbackEvent(ProxyCallbackEvent& event);
    void drainProxyCallbackEvents();
    void handleProxyCallbackEvent(const ProxyCallbackEvent& event);
    void clearProxyAdvertisingSchedule();
    void clearProxyAdvertisingWindowState();
    void stopProxyAdvertisingFromMainLoop(uint8_t reasonCode);
    
    // Start advertising proxy service
    void startProxyAdvertising(uint8_t reasonCode = 0, bool ignoreWifiPriority = false);
    
    // Internal callbacks
    static void notifyCallback(NimBLERemoteCharacteristic* pChar, 
                               uint8_t* pData, 
                               size_t length, 
                               bool isNotify);
    
    bool connectToServer();
};

// Refresh the shared NimBLE bond backup snapshot after an external module
// adds or deletes bond records.
int refreshBleBondBackup();

#endif // BLE_CLIENT_H

/**
 * BLE Client for Valentine1 Gen2
 * Handles connection and data reception from V1 over BLE
 * Also supports BLE Server mode for proxying to JBV1/other apps
 */

#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>

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
enum class BLEState {
    DISCONNECTED,   // Not connected, not doing anything
    SCANNING,       // Actively scanning for V1
    SCAN_STOPPING,  // Scan stop requested, waiting for settle
    CONNECTING,     // Connection attempt in progress
    CONNECTED,      // Successfully connected to V1
    BACKOFF         // Failed connection, waiting before retry
};

// Convert BLEState to string for logging
inline const char* bleStateToString(BLEState state) {
    switch (state) {
        case BLEState::DISCONNECTED: return "DISCONNECTED";
        case BLEState::SCANNING: return "SCANNING";
        case BLEState::SCAN_STOPPING: return "SCAN_STOPPING";
        case BLEState::CONNECTING: return "CONNECTING";
        case BLEState::CONNECTED: return "CONNECTED";
        case BLEState::BACKOFF: return "BACKOFF";
        default: return "UNKNOWN";
    }
}

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
    V1BLEClient();
    ~V1BLEClient();
    
    // Initialize BLE stack only (no scanning) - call before fastReconnect()
    bool initBLE(bool enableProxy = false, const char* proxyName = "V1C-LE-S3");
    
    // Initialize BLE and start scanning
    // If enableProxy is true, also starts BLE server for JBV1 connections
    bool begin(bool enableProxy = false, const char* proxyName = "V1C-LE-S3");
    
    // Check connection status
    bool isConnected();
    
    // Check if proxy client (JBV1) is connected
    bool isProxyClientConnected();
    
    // Check if BLE proxy is enabled
    bool isProxyEnabled() const { return proxyEnabled; }

    // Set proxy client connection status (for internal callback use)
    void setProxyClientConnected(bool connected);
    
    // Register callback for received data
    void onDataReceived(DataCallback callback);
    
    // Register callback for V1 connection
    void onV1Connected(ConnectionCallback callback);
    
    // Send command to V1 (e.g., request alert data)
    bool sendCommand(const uint8_t* data, size_t length);
    
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
    
    // Called by main loop when RESP_USER_BYTES received to complete verification
    void onUserBytesReceived(const uint8_t* bytes);
    
    // Check if we're waiting for user bytes verification
    bool isAwaitingVerification() const { return verifyPending; }
    
    // Disconnect and cleanup
    void disconnect();
    
    // Full cleanup of BLE connection state (clears characteristic refs, unsubscribes)
    void cleanupConnection();
    
    // Hard reset of BLE client stack after repeated failures
    void hardResetBLEClient();
    
    // Process BLE events (call in loop)
    void process();
    
    // Attempt a fast reconnect to a known address
    bool fastReconnect();
    
    // Set the target address for fast reconnect (must be called before fastReconnect)
    void setTargetAddress(const NimBLEAddress& address);
    
    // Restart scanning for V1
    void startScanning();
    
    // Check if currently scanning
    bool isScanning();
    
    // Get current BLE state (for diagnostics)
    BLEState getBLEState() const { return bleState; }
    
    // Get the connected V1's BLE address
    NimBLEAddress getConnectedAddress() const;
    
    // Forward data to proxy clients (queues data for async send)
    // sourceCharUUID: last 16-bit of source characteristic UUID (0xB2CE, 0xB4E0, etc)
    void forwardToProxy(const uint8_t* data, size_t length, uint16_t sourceCharUUID);
    
    // PERFORMANCE: Immediate proxy forwarding - zero latency path
    // Called directly from BLE callback context - no queue, no delay
    void forwardToProxyImmediate(const uint8_t* data, size_t length, uint16_t sourceCharUUID);
    
    // Process pending proxy notifications (call from main loop after display update)
    // Returns number of packets sent
    int processProxyQueue();
    
    // Get proxy metrics (for instrumentation)
    const ProxyMetrics& getProxyMetrics() const { return proxyMetrics; }
    
    // Reset proxy metrics (call after printing)
    void resetProxyMetrics() { proxyMetrics.reset(); }
    
    // WiFi priority mode - deprioritize BLE when web UI is active
    void setWifiPriority(bool enabled);  // Enable = suppress BLE activity
    bool isWifiPriority() const { return wifiPriorityMode; }

private:
    // Nested callback classes - defined before member declarations that use them
    class ClientCallbacks : public NimBLEClientCallbacks {
    public:
        void onConnect(NimBLEClient* pClient) override;
        void onDisconnect(NimBLEClient* pClient, int reason) override;
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

    NimBLEClient* pClient;
    NimBLERemoteService* pRemoteService;
    NimBLERemoteCharacteristic* pDisplayDataChar;
    NimBLERemoteCharacteristic* pCommandChar;
    NimBLERemoteCharacteristic* pCommandCharLong;  // B8D2 - for long commands like voltage request
    
    // BLE Server (proxy) objects
    NimBLEServer* pServer;
    NimBLEService* pProxyService;
    NimBLECharacteristic* pProxyNotifyChar;     // B2CE proxy - short display data
    NimBLECharacteristic* pProxyNotifyLongChar; // B4E0 proxy - long alert/response data
    NimBLECharacteristic* pProxyWriteChar;
    bool proxyEnabled;
    bool proxyServerInitialized;
    bool proxyClientConnected; // Encapsulated status
    String proxyName_;
    
    // Synchronization primitives (mirroring Kenny's approach)
    SemaphoreHandle_t bleMutex = nullptr;
    SemaphoreHandle_t bleNotifyMutex = nullptr;
    
    // Proxy queue for decoupling notify from hot path
    static constexpr size_t PROXY_QUEUE_SIZE = 8;  // Small queue, drop-oldest on overflow
    static constexpr size_t PROXY_PACKET_MAX = 64; // Max packet size for proxy
    struct ProxyPacket {
        uint8_t data[PROXY_PACKET_MAX];
        size_t length;
        uint16_t charUUID;
    };
    ProxyPacket proxyQueue[PROXY_QUEUE_SIZE];
    volatile size_t proxyQueueHead = 0;  // Next write position
    volatile size_t proxyQueueTail = 0;  // Next read position
    volatile size_t proxyQueueCount = 0; // Current items in queue
    ProxyMetrics proxyMetrics;
    
    DataCallback dataCallback;
    ConnectionCallback connectCallback;
    bool connected;
    bool shouldConnect;
    bool hasTargetDevice = false;
    NimBLEAdvertisedDevice targetDevice;
    NimBLEAddress targetAddress;
    uint8_t targetAddressType = BLE_ADDR_PUBLIC;  // Saved from advertisement
    unsigned long lastScanStart;
    
    // BLE State Machine - centralized connection state
    BLEState bleState = BLEState::DISCONNECTED;
    unsigned long stateEnteredMs = 0;       // When current state was entered
    unsigned long scanStopRequestedMs = 0;  // When scan stop was requested
    static constexpr unsigned long SCAN_STOP_SETTLE_MS = 1000;  // Wait after scan stop (1s for WiFi coexistence)
    
    // Connection attempt guard - prevents overlapping attempts
    bool connectInProgress = false;
    
    // State transition helper
    void setBLEState(BLEState newState, const char* reason);
    
    // Internal connection attempt (called only from state machine)
    bool attemptConnection();
    
    // Exponential backoff for connection failures (error 13 = BLE_HS_EBUSY)
    uint8_t consecutiveConnectFailures = 0;
    unsigned long nextConnectAllowedMs = 0;  // Backoff until this time
    static constexpr uint8_t MAX_BACKOFF_FAILURES = 5;
    static constexpr unsigned long BACKOFF_BASE_MS = 5000;   // 5 seconds base
    static constexpr unsigned long BACKOFF_MAX_MS = 30000;   // 30 seconds max
    
    // Deferred proxy advertising start (non-blocking - avoids stall)
    // 150ms matches Kenny's v1g2-t4s3 approach - just enough for radio to settle
    unsigned long proxyAdvertisingStartMs = 0;  // When to start advertising (0 = not pending)
    static constexpr unsigned long PROXY_STABILIZE_MS = 150;  // Match Kenny's 150ms delay
    
    // Write verification state
    bool verifyPending = false;
    uint8_t verifyExpected[6] = {0};
    uint8_t verifyReceived[6] = {0};
    bool verifyComplete = false;
    bool verifyMatch = false;

    // Pointers to our callback handler instances
    ScanCallbacks* pScanCallbacks;
    ClientCallbacks* pClientCallbacks;
    ProxyServerCallbacks* pProxyServerCallbacks;
    ProxyWriteCallbacks* pProxyWriteCallbacks;
    
    // WiFi priority mode flag
    bool wifiPriorityMode = false;
    
    // Initialize BLE server for proxy mode
    void initProxyServer(const char* deviceName);
    
    // Start advertising proxy service
    void startProxyAdvertising();
    
    // Internal callbacks
    static void notifyCallback(NimBLERemoteCharacteristic* pChar, 
                               uint8_t* pData, 
                               size_t length, 
                               bool isNotify);
    
    bool connectToServer();
    bool setupCharacteristics();
};

#endif // BLE_CLIENT_H

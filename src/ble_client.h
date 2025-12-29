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
    
    // Disconnect and cleanup
    void disconnect();
    
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
    
    // Forward data to proxy clients (called when data is received from V1)
    // sourceCharUUID: last 16-bit of source characteristic UUID (0xB2CE, 0xB4E0, etc)
    void forwardToProxy(const uint8_t* data, size_t length, uint16_t sourceCharUUID);

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
    
    // BLE Server (proxy) objects
    NimBLEServer* pServer;
    NimBLEService* pProxyService;
    NimBLECharacteristic* pProxyNotifyChar;     // B2CE proxy - main display data
    NimBLECharacteristic* pProxyWriteChar;
    bool proxyEnabled;
    bool proxyServerInitialized;
    bool proxyClientConnected; // Encapsulated status
    String proxyName_;
    
    // Synchronization primitives (mirroring Kenny's approach)
    SemaphoreHandle_t bleMutex = nullptr;
    SemaphoreHandle_t bleNotifyMutex = nullptr;
    
    DataCallback dataCallback;
    ConnectionCallback connectCallback;
    bool connected;
    bool shouldConnect;
    bool hasTargetDevice = false;
    NimBLEAdvertisedDevice targetDevice;
    NimBLEAddress targetAddress;
    unsigned long lastScanStart;

    // Pointers to our callback handler instances
    ScanCallbacks* pScanCallbacks;
    ClientCallbacks* pClientCallbacks;
    ProxyServerCallbacks* pProxyServerCallbacks;
    ProxyWriteCallbacks* pProxyWriteCallbacks;
    
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

/**
 * ALP BLE Client for V1 Gen2 Simple Display
 * 
 * Phase 1: Discovery & Logging
 * - Scan for ALP devices
 * - Connect with pairing code
 * - Log ALL BLE traffic (services, characteristics, notifications)
 * - Dump to SD card and serial for analysis
 * 
 * Future phases will add protocol parsing once we understand the data.
 */

#ifndef ALP_CLIENT_H
#define ALP_CLIENT_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <FS.h>

// ALP Connection states
enum class ALPState {
    ALP_DISABLED,       // ALP integration disabled
    ALP_SCANNING,       // Scanning for ALP device
    ALP_FOUND,          // ALP device found, waiting to connect
    ALP_CONNECTING,     // Connection in progress
    ALP_CONNECTED,      // Connected and logging
    ALP_DISCONNECTED,   // Was connected, now disconnected
    ALP_ERROR           // Connection error
};

// Convert ALPState to string for logging
inline const char* alpStateToString(ALPState state) {
    switch (state) {
        case ALPState::ALP_DISABLED: return "DISABLED";
        case ALPState::ALP_SCANNING: return "SCANNING";
        case ALPState::ALP_FOUND: return "FOUND";
        case ALPState::ALP_CONNECTING: return "CONNECTING";
        case ALPState::ALP_CONNECTED: return "CONNECTED";
        case ALPState::ALP_DISCONNECTED: return "DISCONNECTED";
        case ALPState::ALP_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

// Forward declarations
class ALPClient;

// Scan callbacks for ALP scanning (NimBLE 2.x style)
class ALPScanCallbacks : public NimBLEScanCallbacks {
public:
    ALPScanCallbacks(ALPClient* client) : alpClient(client) {}
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
    void onScanEnd(const NimBLEScanResults& scanResults, int reason) override;
private:
    ALPClient* alpClient;
};

// Client callbacks for ALP connection
class ALPClientCallbacks : public NimBLEClientCallbacks {
public:
    ALPClientCallbacks(ALPClient* client) : alpClient(client) {}
    void onConnect(NimBLEClient* pClient) override;
    void onDisconnect(NimBLEClient* pClient, int reason) override;
private:
    ALPClient* alpClient;
};

class ALPClient {
public:
    ALPClient();
    ~ALPClient();
    
    // Initialize (call after NimBLE is initialized)
    bool init();
    
    // Enable/disable ALP integration
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }
    
    // Get current state
    ALPState getState() const { return state_; }
    bool isConnected() const { return state_ == ALPState::ALP_CONNECTED; }
    
    // Pairing
    void setPairingCode(const String& code);
    String getPairingCode() const { return pairingCode_; }
    
    // Scanning and connection
    bool startScan();
    void stopScan();
    bool connect();
    void disconnect();
    
    // Process loop (call from main loop)
    void process();
    
    // Get discovered device info
    String getDeviceName() const { return deviceName_; }
    String getDeviceAddress() const { return deviceAddress_; }
    int getRSSI() const { return rssi_; }
    uint16_t getServicesCount() const { return servicesCount_; }
    uint16_t getNotificationsCount() const { return notificationsCount_; }
    
    // Logging control
    void setLogToSerial(bool enabled) { logToSerial_ = enabled; }
    void setLogToSD(bool enabled) { logToSD_ = enabled; }
    bool isLoggingToSerial() const { return logToSerial_; }
    bool isLoggingToSD() const { return logToSD_; }
    
    // Get log stats
    uint32_t getPacketCount() const { return packetCount_; }
    uint32_t getLogFileSize() const;
    
    // Dump discovered services/characteristics to log
    void dumpServices();
    
    // Callback handlers (called by callback classes)
    void handleScanResult(const NimBLEAdvertisedDevice* advertisedDevice);
    void handleScanEnd(const NimBLEScanResults& scanResults, int reason);
    void handleConnect(NimBLEClient* pClient);
    void handleDisconnect(NimBLEClient* pClient, int reason);
    
private:
    // State
    bool enabled_ = false;
    ALPState state_ = ALPState::ALP_DISABLED;
    String pairingCode_;
    
    // Device info
    String deviceName_;
    String deviceAddress_;
    NimBLEAddress* targetAddress_ = nullptr;
    int rssi_ = 0;
    uint16_t servicesCount_ = 0;
    uint16_t notificationsCount_ = 0;
    
    // BLE objects
    NimBLEClient* pClient_ = nullptr;
    NimBLEScan* pScan_ = nullptr;
    ALPScanCallbacks* pScanCallbacks_ = nullptr;
    ALPClientCallbacks* pClientCallbacks_ = nullptr;
    
    // Logging
    bool logToSerial_ = true;
    bool logToSD_ = true;
    uint32_t packetCount_ = 0;
    File logFile_;
    String logFilePath_;
    
    // Timing
    unsigned long lastScanStart_ = 0;
    unsigned long connectAttemptStart_ = 0;
    
    // Internal helpers
    void setState(ALPState newState);
    void logPacketRaw(const String& serviceUUID, const String& charUUID, char operation,
                      const uint8_t* data, size_t len);
    void openLogFile();
    void closeLogFile();
    bool subscribeToAllNotifications();
    
    // Notification callback (static because NimBLE needs it)
    static void notifyCallback(NimBLERemoteCharacteristic* pChar,
                               uint8_t* pData, size_t length, bool isNotify);
    static ALPClient* instance_;  // For static callback access
};

// Global instance
extern ALPClient alpClient;

#endif // ALP_CLIENT_H

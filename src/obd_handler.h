// OBD-II Handler for BLE adapters
// Provides vehicle speed data via Bluetooth Low Energy OBD-II adapter
// 
// Uses separate NimBLE client instance to connect to OBD adapter
// while main BLE client maintains connection to V1.
//
// Common BLE UART services:
// - Nordic UART Service (NUS):
//   - Service UUID: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
//   - TX Char (notifications): 6e400003-b5a3-f393-e0a9-e50e24dcca9e
//   - RX Char (write): 6e400002-b5a3-f393-e0a9-e50e24dcca9e
// - OBDLink CX custom UART:
//   - Service UUID: 0000fff0-0000-1000-8000-00805f9b34fb
//   - Notify Char: 0000fff1-0000-1000-8000-00805f9b34fb
//   - Write Char:  0000fff2-0000-1000-8000-00805f9b34fb

#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Forward declarations
class NimBLEClient;
class NimBLERemoteService;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;

// OBD-II data structure
struct OBDData {
    float speed_kph;        // Vehicle speed in km/h (PID 0x0D)
    float speed_mph;        // Vehicle speed in mph
    uint16_t rpm;           // Engine RPM (PID 0x0C) / 4
    float voltage;          // Battery voltage (AT RV command)
    int8_t oil_temp_c;      // Engine oil temperature in Celsius (VW Mode 22 PID F40C)
    int8_t dsg_temp_c;      // DSG/transmission oil temp in Celsius (VW Mode 22 PID F40D)
    int8_t intake_air_temp_c; // Intake air temperature in Celsius (PID 0x0F)
    bool valid;             // True if OBD connection is active and data is fresh
    uint32_t timestamp_ms;  // millis() when data was last updated
};

// Found OBD device info (for scan results)
struct OBDDeviceInfo {
    String address;         // BLE address
    String name;            // Device name
    int rssi;              // Signal strength
};

// OBD BLE connection states
enum class OBDState {
    OBD_DISABLED,       // OBD not enabled in settings
    IDLE,               // Waiting to start scan
    SCANNING,           // Scanning for OBD adapter
    CONNECTING,         // Connecting to found device
    INITIALIZING,       // Sending AT init commands
    READY,              // Connected and initialized
    POLLING,            // Actively polling for data
    DISCONNECTED,       // Was connected, now disconnected
    FAILED              // Detection timeout or init failed
};

class OBDHandler {
private:
    OBDData lastData;
    OBDState state;
    
    // Module detection state
    bool moduleDetected;
    bool detectionComplete;
    uint32_t detectionStartMs;
    static constexpr uint32_t DETECTION_TIMEOUT_MS = 120000;  // 120 seconds for auto-reconnect scan
    
    // OBD BLE adapter names typically contain these strings
    // Zurich ZR-BT1 = rebranded Innova 1000 (also Hyper Tough HT500, Blcktec 430)
    static constexpr const char* OBD_ADAPTER_NAME_PATTERNS[] = {
        "OBDLink",     // OBDLink CX, MX+, LX, etc.
        "VLINK",       // OBDLink app naming (iOS-VLINK, ANDROID-VLINK)
        "OBDII",
        "OBD2", 
        "ELM327",
        "Vgate",
        "iCar",
        "KONNWEI",
        "Viecar",
        "Veepeak",
        "ZURICH",
        "ZR-BT",
        "Innova",
        "HT500",
        "Blcktec",
        "BlueDriver"
    };
    static constexpr int OBD_ADAPTER_NAME_PATTERN_COUNT = 16;
    
    // Nordic UART Service UUIDs (used by many BLE OBD adapters)
    static constexpr const char* NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
    static constexpr const char* NUS_RX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";  // Write to this
    static constexpr const char* NUS_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";  // Notifications from this
    
    // BLE connection members
    NimBLEClient* pOBDClient;
    NimBLERemoteService* pNUSService;
    NimBLERemoteCharacteristic* pRXChar;  // Write commands here
    NimBLERemoteCharacteristic* pTXChar;  // Receive responses here
    
    NimBLEAddress targetAddress;
    bool hasTargetDevice;
    String targetDeviceName;
    
    // Found devices during scan
    std::vector<OBDDeviceInfo> foundDevices;
    bool scanActive;
    uint32_t scanStartMs;
    static constexpr uint32_t SCAN_DURATION_MS = 10000;  // 10 second scan
    
    // Response buffer for AT commands
    String responseBuffer;
    static constexpr size_t RESPONSE_BUFFER_SIZE = 256;
    bool responseComplete;

    // Polling timing
    uint32_t lastPollMs;
    static constexpr uint32_t POLL_INTERVAL_MS = 1000;  // Poll speed every 1s (reduced load)
    
    // Connection retry management
    uint8_t connectionFailures;
    static constexpr uint8_t MAX_CONNECTION_FAILURES = 5;   // Give up after 5 consecutive failures
    static constexpr uint32_t BASE_RETRY_DELAY_MS = 5000;   // Base delay between retries (5s)
    static constexpr uint32_t MAX_RETRY_DELAY_MS = 60000;   // Max delay (1 minute)
    
    // RSSI tracking for reconnection decisions
    int lastKnownRssi;                                        // Last RSSI from scan/connect (-127 = unknown)
    static constexpr int MIN_RSSI_FOR_CONNECT = -85;          // Don't attempt connect if RSSI weaker than this
    bool checkDevicePresence();                               // Quick scan to check if device is visible

    // Deferred client deletion (prevents heap crash when deleting from callback context)
    bool pendingClientDelete;

    // Background task
    TaskHandle_t obdTaskHandle;
    volatile bool taskRunning;
    volatile bool taskShouldExit;  // Cooperative shutdown flag
    static void taskEntry(void* param);
    bool runStateMachine();
    void startTask();
    void stopTask();
    
    // Mutex for thread safety
    SemaphoreHandle_t obdMutex;
    
    // Debug logging
    static constexpr bool DEBUG_OBD = false;
    
public:
    OBDHandler();
    ~OBDHandler();
    
    // Initialize OBD handler (prepares for connection, does NOT scan)
    void begin();
    
    // Attempt to connect to saved OBD device
    // Call this after V1 connection has settled (10-15s delay)
    // Only connects if saved device exists, otherwise does nothing
    void tryAutoConnect();
    
    // Update - call in main loop (non-blocking)
    // Returns true if new data was received
    bool update();
    
    // Module detection
    bool isModuleDetected() const { return moduleDetected; }
    bool isDetectionComplete() const { return detectionComplete; }
    bool isConnected() const { return state == OBDState::READY || state == OBDState::POLLING; }
    OBDState getState() const { return state; }
    const char* getStateString() const;
    const String& getConnectedDeviceName() const { return targetDeviceName; }
    
    // Manual scan for OBD devices
    void startScan();
    void stopScan();
    void onScanComplete();  // Called by BLE when scan ends
    bool isScanActive() const { return scanActive; }
    std::vector<OBDDeviceInfo> getFoundDevices() const;  // Returns copy (thread-safe)
    void clearFoundDevices();
    
    // Connect to specific device
    bool connectToAddress(const String& address, const String& name = "");
    
    // Get OBD data (thread-safe - returns copy under mutex)
    OBDData getData() const;
    bool hasValidData() const;
    bool isDataStale(uint32_t maxAge_ms = 2000) const;
    
    // Speed accessors (thread-safe)
    float getSpeedKph() const;
    float getSpeedMph() const;
    
    // Request specific PIDs (for future expansion)
    bool requestSpeed();
    bool requestRPM();
    bool requestVoltage();
    bool requestIntakeAirTemp();     // Standard PID 0x0F
    bool requestOilTemp();           // VW Mode 22 PID F40C (engine ECU 7E0)
    bool requestDsgTemp();           // VW Mode 22 PID F40D (trans ECU 7E1)
    
    // Disconnect
    void disconnect();
    
    // Called by V1 BLE scan when any named device is found during OBD scan
    void onDeviceFound(const NimBLEAdvertisedDevice* device);
    
    // Called by V1 BLE scan when an OBD adapter is found (for auto-connect)
    void onObdAdapterFound(const NimBLEAdvertisedDevice* device);
    
    // Check if a device name matches OBD adapter patterns
    static bool isObdAdapterName(const std::string& name);
    
private:
    // State machine handlers
    void handleScanning();
    void handleConnecting();
    void handleInitializing();
    void handlePolling();
    
    // BLE operations
    bool connectToDevice();
    bool discoverServices();
    bool initializeAdapter();
    bool sendATCommand(const char* cmd, String& response, uint32_t timeout_ms = 2000);
    void sendCommand(const char* cmd);
    
    // Notification callback
    static void notificationCallback(NimBLERemoteCharacteristic* pChar, 
                                     uint8_t* pData, size_t length, bool isNotify);
    
    // Response parsing
    bool parseSpeedResponse(const String& response, uint8_t& speedKph);
    bool parseRPMResponse(const String& response, uint16_t& rpm);
    bool parseVoltageResponse(const String& response, float& voltage);
    bool parseIntakeAirTempResponse(const String& response, int8_t& tempC);
    bool parseVwMode22TempResponse(const String& response, const char* pidEcho, int8_t& tempC);
};

// Global OBD handler instance (extern declaration)
extern OBDHandler obdHandler;

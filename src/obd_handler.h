// OBD-II Handler for ELM327 BLE adapters
// Provides vehicle speed data via Bluetooth Low Energy OBD-II adapter
// 
// Uses separate NimBLE client instance to connect to ELM327 device
// while main BLE client maintains connection to V1.
//
// ELM327 BLE adapters typically use Nordic UART Service (NUS):
// - Service UUID: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
// - TX Char (notifications): 6e400003-b5a3-f393-e0a9-e50e24dcca9e
// - RX Char (write): 6e400002-b5a3-f393-e0a9-e50e24dcca9e

#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

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
    bool valid;             // True if OBD connection is active and data is fresh
    uint32_t timestamp_ms;  // millis() when data was last updated
};

// ELM327 BLE connection states
enum class OBDState {
    OBD_DISABLED,       // OBD not enabled in settings
    IDLE,               // Waiting to start scan
    SCANNING,           // Scanning for ELM327 device
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
    static constexpr uint32_t DETECTION_TIMEOUT_MS = 60000;  // 60 seconds to detect module
    
    // ELM327 BLE device names typically contain these strings
    // Zurich ZR-BT1 = rebranded Innova 1000 (also Hyper Tough HT500, Blcktec 430)
    static constexpr const char* ELM327_NAME_PATTERNS[] = {
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
    static constexpr int ELM327_NAME_PATTERN_COUNT = 14;
    
    // Nordic UART Service UUIDs (used by most ELM327 BLE adapters)
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
    
    // Response buffer for AT commands
    String responseBuffer;
    static constexpr size_t RESPONSE_BUFFER_SIZE = 256;
    bool responseComplete;
    
    // Polling timing
    uint32_t lastPollMs;
    static constexpr uint32_t POLL_INTERVAL_MS = 500;  // Poll speed every 500ms
    
    // Mutex for thread safety
    SemaphoreHandle_t obdMutex;
    
    // Debug logging
    static constexpr bool DEBUG_OBD = false;
    
public:
    OBDHandler();
    ~OBDHandler();
    
    // Initialize OBD handler (starts scanning for ELM327 devices)
    void begin();
    
    // Update - call in main loop (non-blocking)
    // Returns true if new data was received
    bool update();
    
    // Module detection
    bool isModuleDetected() const { return moduleDetected; }
    bool isDetectionComplete() const { return detectionComplete; }
    bool isConnected() const { return state == OBDState::READY || state == OBDState::POLLING; }
    OBDState getState() const { return state; }
    const char* getStateString() const;
    
    // Get OBD data
    OBDData getData() const { return lastData; }
    bool hasValidData() const { return lastData.valid && !isDataStale(); }
    bool isDataStale(uint32_t maxAge_ms = 2000) const;
    
    // Speed accessors
    float getSpeedKph() const { return lastData.speed_kph; }
    float getSpeedMph() const { return lastData.speed_mph; }
    
    // Request specific PIDs (for future expansion)
    bool requestSpeed();
    bool requestRPM();
    bool requestVoltage();
    
    // Disconnect
    void disconnect();
    
    // Called by V1 BLE scan when an ELM327 device is found
    void onELM327Found(const NimBLEAdvertisedDevice* device);
    
    // Check if a device name matches ELM327 patterns
    static bool isELM327Device(const std::string& name);
    
private:
    // State machine handlers
    void handleScanning();
    void handleConnecting();
    void handleInitializing();
    void handlePolling();
    
    // BLE operations
    bool connectToDevice();
    bool discoverServices();
    bool initializeELM327();
    bool sendATCommand(const char* cmd, String& response, uint32_t timeout_ms = 2000);
    void sendCommand(const char* cmd);
    
    // Notification callback
    static void notificationCallback(NimBLERemoteCharacteristic* pChar, 
                                     uint8_t* pData, size_t length, bool isNotify);
    
    // Response parsing
    bool parseSpeedResponse(const String& response, uint8_t& speedKph);
    bool parseRPMResponse(const String& response, uint16_t& rpm);
    bool parseVoltageResponse(const String& response, float& voltage);
};

// Global OBD handler instance (extern declaration)
extern OBDHandler obdHandler;


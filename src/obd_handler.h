// OBD-II Handler for BLE adapters
// Provides vehicle speed data via Bluetooth Low Energy OBD-II adapter.
//
// This module is intentionally self-contained:
// - It owns remembered-device persistence (address/name/pin/autoConnect)
// - It performs manual scan only when requested
// - It can auto-connect only to remembered devices marked autoConnect

#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <atomic>
#include <functional>
#include <string>
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
    float speed_kph;          // Vehicle speed in km/h (PID 0x0D)
    float speed_mph;          // Vehicle speed in mph
    uint16_t rpm;             // Engine RPM (PID 0x0C)
    float voltage;            // Battery voltage (AT RV)
    int8_t oil_temp_c;        // Optional vendor PID (if supported)
    int8_t dsg_temp_c;        // Optional vendor PID (if supported)
    int8_t intake_air_temp_c; // PID 0x0F
    bool valid;               // True if OBD connection active and data is fresh
    uint32_t timestamp_ms;    // millis() when last updated
};

// Found device during manual scan
struct OBDDeviceInfo {
    String address;
    String name;
    int rssi;
};

// Remembered device persisted by OBD handler
struct OBDRememberedDevice {
    String address;
    String name;
    String pin;       // Digits only; empty means use adapter default
    bool autoConnect;
    uint32_t lastSeenMs;
};

// OBD BLE connection states
enum class OBDState {
    IDLE,
    SCANNING,
    CONNECTING,
    INITIALIZING,
    READY,
    POLLING,
    DISCONNECTED,
    FAILED
};

class OBDHandler {
public:
    using BoolCallback = std::function<bool()>;
    using VoidCallback = std::function<void()>;

    OBDHandler();
    ~OBDHandler();

    // Initialize OBD handler. Does NOT start scan or connect.
    void begin();

    // Update state machine. Returns true if a fresh data sample is available.
    bool update();

    // Trigger auto-connect to first remembered autoConnect device.
    // Safe to call frequently; it is internally gated and non-blocking.
    void tryAutoConnect();

    // Connection status
    bool isConnected() const { return state == OBDState::READY || state == OBDState::POLLING; }
    OBDState getState() const { return state; }
    const char* getStateString() const;
    const String& getConnectedDeviceName() const { return targetDeviceName; }
    String getConnectedDeviceAddress() const;

    // Manual scan for nearby BLE devices (for user selection)
    void startScan();
    void stopScan();
    void onScanComplete();
    bool isScanActive() const { return scanActive; }
    std::vector<OBDDeviceInfo> getFoundDevices() const;
    void clearFoundDevices();

    // Called by BLE scan bridge (main loop context)
    void onDeviceFoundDeferred(const char* name, const char* addr, int rssi);

    // Connect to a specific device.
    // remember=true stores device in remembered list.
    // autoConnect controls future boot/V1-triggered auto-connect behavior.
    bool connectToAddress(const String& address,
                          const String& name = "",
                          const String& pin = "",
                          bool remember = true,
                          bool autoConnect = false);

    // Disconnect current OBD link (keeps remembered devices)
    void disconnect();

    // Forget one remembered device (by BLE address). If connected to that address, disconnect.
    bool forgetRemembered(const String& address);

    // Remembered-device management
    std::vector<OBDRememberedDevice> getRememberedDevices() const;
    bool setRememberedAutoConnect(const String& address, bool enabled);

    // Data access
    OBDData getData() const;
    bool hasValidData() const;
    bool isDataStale(uint32_t maxAgeMs = 2000) const;
    float getSpeedKph() const;
    float getSpeedMph() const;

    // Optional direct PID requests
    bool requestSpeed();
    bool requestRPM();
    bool requestVoltage();
    bool requestIntakeAirTemp();
    bool requestOilTemp();

    // Dependency injection hooks: keeps OBD logic independent from V1 BLE implementation.
    void setLinkReadyCallback(BoolCallback cb);
    void setStartScanCallback(VoidCallback cb);

private:
    // BLE UART service UUIDs
    static constexpr const char* NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
    static constexpr const char* NUS_RX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
    static constexpr const char* NUS_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

    static constexpr size_t RESPONSE_BUFFER_SIZE = 256;
    static constexpr uint32_t POLL_INTERVAL_MS = 1000;
    static constexpr uint8_t RPM_POLL_DIV = 2;
    static constexpr uint8_t IAT_POLL_DIV = 15;
    static constexpr uint8_t OIL_TEMP_POLL_DIV = 20;
    static constexpr uint8_t MAX_CONNECTION_FAILURES = 5;
    static constexpr uint32_t BASE_RETRY_DELAY_MS = 3000;
    static constexpr uint32_t MAX_RETRY_DELAY_MS = 30000;
    static constexpr size_t MAX_REMEMBERED_DEVICES = 8;

    OBDData lastData{};
    OBDState state = OBDState::IDLE;

    NimBLEClient* pOBDClient = nullptr;
    NimBLERemoteService* pNUSService = nullptr;
    NimBLERemoteCharacteristic* pRXChar = nullptr;
    NimBLERemoteCharacteristic* pTXChar = nullptr;

    NimBLEAddress targetAddress;
    bool hasTargetDevice = false;
    String targetDeviceName;
    String targetPin;
    bool targetIsObdLink = false;
    bool rememberTargetOnConnect = false;
    bool targetAutoConnect = false;

    std::vector<OBDDeviceInfo> foundDevices;
    bool scanActive = false;
    uint32_t scanStartMs = 0;

    std::vector<OBDRememberedDevice> rememberedDevices;
    uint32_t lastAutoConnectAttemptMs = 0;
    static constexpr uint32_t AUTO_CONNECT_RETRY_MS = 5000;

    char responseBuffer[RESPONSE_BUFFER_SIZE + 1] = {0};
    size_t responseLength = 0;
    bool responseComplete = false;
    std::atomic<uint32_t> notifyDropCount{0};

    uint32_t lastPollMs = 0;
    uint8_t connectionFailures = 0;

    TaskHandle_t obdTaskHandle = nullptr;
    volatile bool taskRunning = false;
    volatile bool taskShouldExit = false;

    SemaphoreHandle_t obdMutex = nullptr;

    Preferences prefs;

    // State machine
    bool runStateMachine();
    void handleConnecting();
    void handleInitializing();
    void handlePolling();

    // BLE operations
    bool connectToDevice();
    bool discoverServices();
    bool initializeAdapter();
    bool sendATCommand(const char* cmd, String& response, uint32_t timeoutMs = 2000);

    // Persistence helpers
    void loadRememberedDevices();
    void saveRememberedDevices();
    void upsertRemembered(const String& address,
                          const String& name,
                          const String& pin,
                          bool autoConnect,
                          uint32_t lastSeenMs);
    bool findAutoConnectTarget(OBDRememberedDevice& out) const;

    // Task helpers
    static void taskEntry(void* param);
    void startTask();
    void stopTask();

    // Parsing helpers
    static bool isValidHexString(const String& str, size_t expectedLen = 0);
    bool parseSpeedResponse(const String& response, uint8_t& speedKph);
    bool parseRPMResponse(const String& response, uint16_t& rpm);
    bool parseVoltageResponse(const String& response, float& voltage);
    bool parseIntakeAirTempResponse(const String& response, int8_t& tempC);
    bool parseVwMode22TempResponse(const String& response, const char* pidEcho, int8_t& tempC);

    static bool isObdLinkName(const std::string& name);
    static uint32_t normalizePin(const String& pin, bool obdLinkDefault);
    bool isLinkReady() const;
    void requestScanStart() const;

    BoolCallback isLinkReadyCb;
    VoidCallback startScanCb;

    // Notification callback
    static void notificationCallback(NimBLERemoteCharacteristic* pChar,
                                     uint8_t* pData,
                                     size_t length,
                                     bool isNotify);
};

extern OBDHandler obdHandler;

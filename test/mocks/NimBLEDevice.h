#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

typedef void* SemaphoreHandle_t;

static constexpr uint8_t BLE_ADDR_PUBLIC = 0;
static constexpr int BLE_UUID_TYPE_16 = 16;
static constexpr int BLE_UUID_TYPE_128 = 128;
static constexpr int ESP_PWR_LVL_P9 = 9;

enum NIMBLE_PROPERTY : uint32_t {
    READ = 1 << 0,
    NOTIFY = 1 << 1,
    WRITE_NR = 1 << 2,
    WRITE = 1 << 3,
};

struct MockNimBLEState {
    uint32_t createServerCalls = 0;
    uint32_t createServiceCalls = 0;
    uint32_t createCharacteristicCalls = 0;
    bool advertising = false;
};

inline MockNimBLEState g_mock_nimble_state{};

inline void mock_reset_nimble_state() {
    g_mock_nimble_state = MockNimBLEState{};
}

class NimBLEAddress {
public:
    NimBLEAddress() = default;
    explicit NimBLEAddress(const std::string&, uint8_t = BLE_ADDR_PUBLIC) {}
    bool isNull() const { return false; }
    std::string toString() const { return ""; }
};

class NimBLEUUID {
public:
    NimBLEUUID() = default;
    explicit NimBLEUUID(const char* uuid)
        : value_(uuid ? uuid : "") {}

    int bitSize() const { return BLE_UUID_TYPE_128; }
    const uint8_t* getValue() const { return raw_; }
    void to16() {}

private:
    std::string value_;
    uint8_t raw_[16] = {0};
};

class NimBLEConnInfo {
public:
    uint16_t getConnHandle() const { return 1; }
    uint16_t getConnInterval() const { return 12; }
    uint16_t getConnLatency() const { return 0; }
};

class NimBLEAdvertisedDevice {};
class NimBLEScanResults {};
class NimBLERemoteService {};

class NimBLERemoteCharacteristic {
public:
    bool writeValue(const uint8_t*, size_t, bool) { return true; }
};

class NimBLEAttValue {
public:
    NimBLEAttValue() = default;

    const uint8_t* data() const { return data_.empty() ? nullptr : data_.data(); }
    size_t size() const { return data_.size(); }
    void assign(const uint8_t* data, size_t size) { data_.assign(data, data + size); }

private:
    std::vector<uint8_t> data_;
};

class NimBLECharacteristicCallbacks;

class NimBLECharacteristic {
public:
    explicit NimBLECharacteristic(const char* uuid = "")
        : uuid_(uuid ? uuid : "") {}

    void setCallbacks(NimBLECharacteristicCallbacks* callbacks) { callbacks_ = callbacks; }
    bool notify(const uint8_t*, size_t) { return notifyResult_; }
    bool writeValue(const uint8_t*, size_t, bool) { return writeValueResult_; }
    NimBLEAttValue getValue() const { return value_; }
    NimBLEUUID getUUID() const { return NimBLEUUID(uuid_.c_str()); }
    void setValue(const uint8_t* data, size_t size) { value_.assign(data, size); }
    void setNotifyResult(bool ok) { notifyResult_ = ok; }
    void setWriteValueResult(bool ok) { writeValueResult_ = ok; }

private:
    std::string uuid_;
    NimBLECharacteristicCallbacks* callbacks_ = nullptr;
    NimBLEAttValue value_;
    bool notifyResult_ = true;
    bool writeValueResult_ = true;
};

class NimBLEService {
public:
    explicit NimBLEService(const char* uuid = "")
        : uuid_(uuid ? uuid : "") {}

    NimBLECharacteristic* createCharacteristic(const char* uuid, uint32_t) {
        g_mock_nimble_state.createCharacteristicCalls++;
        characteristics_.push_back(std::make_unique<NimBLECharacteristic>(uuid));
        return characteristics_.back().get();
    }

    void start() {}
    NimBLEUUID getUUID() const { return NimBLEUUID(uuid_.c_str()); }

private:
    std::string uuid_;
    std::vector<std::unique_ptr<NimBLECharacteristic>> characteristics_;
};

class NimBLEServerCallbacks;

class NimBLEServer {
public:
    NimBLEService* createService(const char* uuid) {
        g_mock_nimble_state.createServiceCalls++;
        services_.push_back(std::make_unique<NimBLEService>(uuid));
        return services_.back().get();
    }

    void setCallbacks(NimBLEServerCallbacks* callbacks) { callbacks_ = callbacks; }
    void updateConnParams(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {}
    int getConnectedCount() const { return connectedCount_; }
    NimBLEConnInfo getPeerInfo(int) const { return NimBLEConnInfo(); }
    void setConnectedCount(int count) { connectedCount_ = count; }

private:
    NimBLEServerCallbacks* callbacks_ = nullptr;
    std::vector<std::unique_ptr<NimBLEService>> services_;
    int connectedCount_ = 0;
};

class NimBLEClientCallbacks;

class NimBLEClient {
public:
    void setClientCallbacks(NimBLEClientCallbacks*) {}
    void setConnectionParams(uint16_t, uint16_t, uint16_t, uint16_t) {}
    void setConnectTimeout(unsigned long) {}
    bool isConnected() const { return connected_; }
    void disconnect() { connected_ = false; }
    int getRssi() const { return -60; }
    NimBLEConnInfo getConnInfo() const { return NimBLEConnInfo(); }

private:
    bool connected_ = false;
};

class NimBLEAdvertisementData {
public:
    void setFlags(uint8_t) {}
    void setCompleteServices(const NimBLEUUID&) {}
    void setAppearance(uint16_t) {}
    void setName(const char*) {}
};

class NimBLEAdvertising {
public:
    void setAdvertisementData(const NimBLEAdvertisementData&) {}
    void setScanResponseData(const NimBLEAdvertisementData&) {}
    void setMinInterval(uint16_t) {}
    void setMaxInterval(uint16_t) {}
    bool start() {
        g_mock_nimble_state.advertising = true;
        return true;
    }
    bool isAdvertising() const { return g_mock_nimble_state.advertising; }
};

class NimBLEScanCallbacks;

class NimBLEScan {
public:
    void setActiveScan(bool) {}
    void setScanCallbacks(NimBLEScanCallbacks*) {}
    void setInterval(uint16_t) {}
    void setWindow(uint16_t) {}
    void setMaxResults(uint16_t) {}
    void setDuplicateFilter(bool) {}
    void clearResults() {}
    bool start(uint32_t, bool, bool) { scanning_ = true; return true; }
    void stop() { scanning_ = false; }
    bool isScanning() const { return scanning_; }

private:
    bool scanning_ = false;
};

class NimBLEDevice {
public:
    static void init(const char*) {}
    static void deinit(bool = false) {}
    static void setDeviceName(const char*) {}
    static void setPower(int) {}
    static void setMTU(uint16_t) {}
    static NimBLEScan* getScan() { static NimBLEScan scan; return &scan; }
    static NimBLEServer* createServer() {
        g_mock_nimble_state.createServerCalls++;
        static NimBLEServer server;
        return &server;
    }
    static NimBLEClient* createClient() {
        static NimBLEClient client;
        return &client;
    }
    static NimBLEAdvertising* getAdvertising() {
        static NimBLEAdvertising advertising;
        return &advertising;
    }
    static bool startAdvertising() {
        g_mock_nimble_state.advertising = true;
        return true;
    }
    static bool startAdvertising(int) {
        g_mock_nimble_state.advertising = true;
        return true;
    }
    static void stopAdvertising() { g_mock_nimble_state.advertising = false; }
    static uint8_t getNumBonds() { return 0; }
    static void deleteAllBonds() {}
    static bool isBonded(const NimBLEAddress&) { return false; }
    static void deleteBond(const NimBLEAddress&) {}
};

class NimBLEClientCallbacks {
public:
    virtual ~NimBLEClientCallbacks() = default;
    virtual void onConnect(NimBLEClient*) {}
    virtual void onDisconnect(NimBLEClient*, int) {}
};

class NimBLEScanCallbacks {
public:
    virtual ~NimBLEScanCallbacks() = default;
    virtual void onResult(const NimBLEAdvertisedDevice*) {}
    virtual void onScanEnd(const NimBLEScanResults&, int) {}
};

class NimBLEServerCallbacks {
public:
    virtual ~NimBLEServerCallbacks() = default;
    virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
    virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
};

class NimBLECharacteristicCallbacks {
public:
    virtual ~NimBLECharacteristicCallbacks() = default;
    virtual void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}
};

inline int ble_gap_conn_rssi(uint16_t, int8_t* rssi) {
    if (rssi) {
        *rssi = -55;
    }
    return 0;
}

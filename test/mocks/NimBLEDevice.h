#pragma once
// Minimal NimBLE stubs for UNIT_TEST builds
#include <stdint.h>
#include <string>

typedef void* SemaphoreHandle_t;

static constexpr uint8_t BLE_ADDR_PUBLIC = 0;

class NimBLEAddress {
public:
    NimBLEAddress() = default;
    explicit NimBLEAddress(const std::string&) {}
    std::string toString() const { return ""; }
};
class NimBLEUUID {
public:
    NimBLEUUID() = default;
    explicit NimBLEUUID(const char*) {}
};
class NimBLEConnInfo {};
class NimBLEAdvertisedDevice {};
class NimBLEScanResults {};
class NimBLERemoteService {};
class NimBLERemoteCharacteristic {};
class NimBLECharacteristic {};
class NimBLEService {};
class NimBLEScan { public: void setActiveScan(bool) {} };
class NimBLEServer {};
class NimBLEClient {};
class NimBLEDevice {
public:
    static void init(const char*) {}
    static void setPower(int) {}
    static NimBLEScan* getScan() { static NimBLEScan scan; return &scan; }
    static NimBLEServer* createServer() { static NimBLEServer server; return &server; }
    static NimBLEClient* createClient() { static NimBLEClient client; return &client; }
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

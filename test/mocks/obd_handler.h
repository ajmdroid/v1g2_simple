#pragma once
#include <climits>
#include <cstdint>
#include <vector>

#include "Arduino.h"

struct OBDData {
    float speed_kph = 0.0f;
    float speed_mph = 0.0f;
    uint16_t rpm = 0;
    float voltage = 0.0f;
    int16_t oil_temp_c = INT16_MIN;
    int8_t dsg_temp_c = -128;
    int8_t intake_air_temp_c = -128;
    bool valid = false;
    unsigned long timestamp_ms = 0;
};

struct OBDDeviceInfo {
    String address;
    String name;
    int rssi = 0;
};

struct OBDRememberedDevice {
    String address;
    String name;
    String pin;
    bool autoConnect = false;
    uint32_t lastSeenMs = 0;
};

class OBDHandler {
public:
    bool isModuleDetected() const { return moduleDetected; }
    const char* getStateString() const { return stateString.c_str(); }
    bool isConnected() const { return connected; }
    bool isScanActive() const { return scanActive; }
    const String& getConnectedDeviceName() const { return connectedDeviceName; }
    String getConnectedDeviceAddress() const { return connectedDeviceAddress; }
    bool hasValidData() const { return validData; }
    OBDData getData() const { return data; }
    std::vector<OBDDeviceInfo> getFoundDevices() const { return foundDevices; }
    std::vector<OBDRememberedDevice> getRememberedDevices() const { return rememberedDevices; }

    void setModuleDetected(bool v) { moduleDetected = v; }
    void setStateString(const String& v) { stateString = v.c_str(); }
    void setConnected(bool v) { connected = v; }
    void setScanActive(bool v) { scanActive = v; }
    void setConnectedDeviceName(const String& v) { connectedDeviceName = v; }
    void setConnectedDeviceAddress(const String& v) { connectedDeviceAddress = v; }
    void setValidData(bool v) { validData = v; }
    void setData(const OBDData& d) { data = d; }
    void setFoundDevices(const std::vector<OBDDeviceInfo>& devices) { foundDevices = devices; }
    void setRememberedDevices(const std::vector<OBDRememberedDevice>& devices) { rememberedDevices = devices; }

    void startScan() {
        startScanCalls++;
        scanActive = true;
    }

    void stopScan() {
        stopScanCalls++;
        scanActive = false;
    }

    void clearFoundDevices() {
        clearFoundDevicesCalls++;
        foundDevices.clear();
    }

    bool connectToAddress(const String& address,
                          const String& name = "",
                          const String& pin = "",
                          bool remember = true,
                          bool autoConnect = false) {
        connectCalls++;
        lastConnectAddress = address;
        lastConnectName = name;
        lastConnectPin = pin;
        lastConnectRemember = remember;
        lastConnectAutoConnect = autoConnect;
        return connectReturn;
    }

    void disconnect() {
        disconnectCalls++;
        connected = false;
    }

    bool setRememberedAutoConnect(const String& address, bool enabled) {
        setRememberedAutoConnectCalls++;
        lastRememberedAutoConnectAddress = address;
        lastRememberedAutoConnectEnabled = enabled;
        return setRememberedAutoConnectReturn;
    }

    bool forgetRemembered(const String& address) {
        forgetRememberedCalls++;
        lastForgottenAddress = address;
        return forgetRememberedReturn;
    }

    void setVwDataEnabled(bool enabled) {
        setVwDataEnabledCalls++;
        lastVwDataEnabled = enabled;
    }

private:
    bool moduleDetected = false;
    std::string stateString = "IDLE";
    bool connected = false;
    bool scanActive = false;
    String connectedDeviceName;
    String connectedDeviceAddress;
    bool validData = false;
    OBDData data{};
    std::vector<OBDDeviceInfo> foundDevices;
    std::vector<OBDRememberedDevice> rememberedDevices;

public:
    bool connectReturn = true;
    bool setRememberedAutoConnectReturn = true;
    bool forgetRememberedReturn = true;
    int startScanCalls = 0;
    int stopScanCalls = 0;
    int clearFoundDevicesCalls = 0;
    int connectCalls = 0;
    int disconnectCalls = 0;
    int setRememberedAutoConnectCalls = 0;
    int forgetRememberedCalls = 0;
    int setVwDataEnabledCalls = 0;
    String lastConnectAddress;
    String lastConnectName;
    String lastConnectPin;
    bool lastConnectRemember = true;
    bool lastConnectAutoConnect = false;
    String lastRememberedAutoConnectAddress;
    bool lastRememberedAutoConnectEnabled = false;
    String lastForgottenAddress;
    bool lastVwDataEnabled = true;
};

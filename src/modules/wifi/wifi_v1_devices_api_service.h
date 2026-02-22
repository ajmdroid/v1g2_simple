#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace WifiV1DevicesApiService {

struct DeviceInfo {
    String address;
    String name;
    uint8_t defaultProfile = 0;  // 0=none/global slot, 1..3=slot override
    bool connected = false;
};

struct Runtime {
    std::function<std::vector<DeviceInfo>()> listDevices;
    std::function<bool(const String&, const String&)> setDeviceName;
    std::function<bool(const String&, uint8_t)> setDeviceDefaultProfile;
    std::function<bool(const String&)> deleteDevice;
};

void handleApiDevicesList(WebServer& server, const Runtime& runtime);

void handleApiDeviceNameSave(WebServer& server,
                             const Runtime& runtime,
                             const std::function<bool()>& checkRateLimit);

void handleApiDeviceProfileSave(WebServer& server,
                                const Runtime& runtime,
                                const std::function<bool()>& checkRateLimit);

void handleApiDeviceDelete(WebServer& server,
                           const Runtime& runtime,
                           const std::function<bool()>& checkRateLimit);

}  // namespace WifiV1DevicesApiService

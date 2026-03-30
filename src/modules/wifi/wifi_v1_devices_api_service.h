#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>
#include <vector>

namespace WifiV1DevicesApiService {

struct DeviceInfo {
    String address;
    String name;
    uint8_t defaultProfile = 0;  // 0=none/global slot, 1..3=slot override
    bool connected = false;
};

struct Runtime {
    std::vector<DeviceInfo> (*listDevices)(void* ctx);
    void* listDevicesCtx;
    bool (*setDeviceName)(const String& address, const String& name, void* ctx);
    void* setDeviceNameCtx;
    bool (*setDeviceDefaultProfile)(const String& address, uint8_t profile, void* ctx);
    void* setDeviceDefaultProfileCtx;
    bool (*deleteDevice)(const String& address, void* ctx);
    void* deleteDeviceCtx;
};

void handleApiDevicesList(WebServer& server, const Runtime& runtime);

void handleApiDeviceNameSave(WebServer& server,
                             const Runtime& runtime,
                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiDeviceProfileSave(WebServer& server,
                                const Runtime& runtime,
                                bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiDeviceDelete(WebServer& server,
                           const Runtime& runtime,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

}  // namespace WifiV1DevicesApiService

#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>
#include <functional>

namespace WifiStatusApiService {

struct StatusRuntime {
    std::function<bool()> setupModeActive;
    std::function<bool()> staConnected;
    std::function<String()> staIp;
    std::function<String()> apIp;
    std::function<String()> connectedSsid;
    std::function<int32_t()> rssi;
    std::function<bool()> staEnabled;
    std::function<String()> staSavedSsid;
    std::function<String()> apSsid;

    std::function<unsigned long()> uptimeSeconds;
    std::function<uint32_t()> heapFree;
    std::function<String()> hostname;
    std::function<String()> firmwareVersion;

    std::function<bool()> timeValid;
    std::function<uint8_t()> timeSource;
    std::function<uint8_t()> timeConfidence;
    std::function<int32_t()> timeTzOffsetMin;
    std::function<int64_t()> timeEpochMsOr0;
    std::function<uint32_t()> timeEpochAgeMsOr0;

    std::function<uint16_t()> batteryVoltageMv;
    std::function<uint8_t()> batteryPercentage;
    std::function<bool()> batteryOnBattery;
    std::function<bool()> batteryHasBattery;

    std::function<bool()> v1Connected;
    std::function<String()> getStatusJson;
    std::function<String()> getAlertJson;
};

void handleApiStatus(WebServer& server,
                     const StatusRuntime& runtime,
                     String& cachedStatusJson,
                     unsigned long& lastStatusJsonTime,
                     unsigned long cacheTtlMs,
                     const std::function<unsigned long()>& millisFn,
                     const std::function<bool()>& checkRateLimit);

void handleApiLegacyStatus(WebServer& server,
                           const StatusRuntime& runtime,
                           String& cachedStatusJson,
                           unsigned long& lastStatusJsonTime,
                           unsigned long cacheTtlMs,
                           const std::function<unsigned long()>& millisFn);

}  // namespace WifiStatusApiService

#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace WifiClientApiService {

struct StatusPayload {
    bool enabled = false;
    String savedSsid;
    const char* state = "unknown";
    bool scanRunning = false;
    bool includeConnectedFields = false;
    String connectedSsid;
    String ip;
    int32_t rssi = 0;
};

struct ScannedNetworkPayload {
    String ssid;
    int32_t rssi = 0;
    bool secure = true;
};

struct ConnectedNetworkPayload {
    String ssid;
    String ip;
    int32_t rssi = 0;
};

struct Runtime {
    std::function<bool()> isEnabled;
    std::function<String()> getSavedSsid;
    std::function<const char*()> getStateName;
    std::function<bool()> isScanRunning;
    std::function<bool()> isConnected;
    std::function<ConnectedNetworkPayload()> getConnectedNetwork;

    std::function<bool()> isScanInProgress;
    std::function<bool()> hasCompletedScanResults;
    std::function<std::vector<ScannedNetworkPayload>()> getScannedNetworks;
    std::function<bool()> startScan;

    std::function<bool(const String&, const String&)> connectToNetwork;
    std::function<void()> disconnectFromNetwork;
    std::function<void()> clearCredentials;
    std::function<void(bool)> setWifiClientEnabled;
    std::function<String()> getSavedPassword;
    std::function<void()> setStateDisabled;
    std::function<void()> setStateDisconnected;
    std::function<void()> setApMode;
};

void handleApiStatus(WebServer& server,
                     const Runtime& runtime,
                     const std::function<void()>& markUiActivity);

void handleApiScan(WebServer& server,
                   const Runtime& runtime,
                   const std::function<bool()>& checkRateLimit,
                   const std::function<void()>& markUiActivity);

void handleApiConnect(WebServer& server,
                      const Runtime& runtime,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity);

void handleApiDisconnect(WebServer& server,
                         const Runtime& runtime,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity);

void handleApiForget(WebServer& server,
                     const Runtime& runtime,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

void handleApiEnable(WebServer& server,
                     const Runtime& runtime,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

}  // namespace WifiClientApiService

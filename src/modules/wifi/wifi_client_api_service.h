#pragma once

#include <Arduino.h>
#include <WebServer.h>
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

void sendStatus(WebServer& server, const StatusPayload& payload);

void sendScanInProgress(WebServer& server);
void sendScanResults(WebServer& server,
                     const std::vector<ScannedNetworkPayload>& networks);
void sendScanStartFailed(WebServer& server);

bool parseConnectRequest(WebServer& server,
                         String& ssidOut,
                         String& passwordOut,
                         const char*& errorMessageOut);
void sendConnectParseError(WebServer& server, const char* message);
void sendConnectStarted(WebServer& server);
void sendConnectStartFailed(WebServer& server);

bool parseEnableRequest(WebServer& server, bool& enabledOut);
void sendEnableParseError(WebServer& server);
void sendEnableResult(WebServer& server, bool enabled);

void sendDisconnected(WebServer& server);
void sendForgotten(WebServer& server);

void handleStatus(WebServer& server, const Runtime& runtime);
void handleScan(WebServer& server, const Runtime& runtime);
void handleConnect(WebServer& server, const Runtime& runtime);
void handleDisconnect(WebServer& server, const Runtime& runtime);
void handleForget(WebServer& server, const Runtime& runtime);
void handleEnable(WebServer& server, const Runtime& runtime);

inline void handleApiStatus(WebServer& server,
                            const Runtime& runtime,
                            const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    handleStatus(server, runtime);
}

inline void handleApiScan(WebServer& server,
                          const Runtime& runtime,
                          const std::function<bool()>& checkRateLimit,
                          const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleScan(server, runtime);
}

inline void handleApiConnect(WebServer& server,
                             const Runtime& runtime,
                             const std::function<bool()>& checkRateLimit,
                             const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleConnect(server, runtime);
}

inline void handleApiDisconnect(WebServer& server,
                                const Runtime& runtime,
                                const std::function<bool()>& checkRateLimit,
                                const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleDisconnect(server, runtime);
}

inline void handleApiForget(WebServer& server,
                            const Runtime& runtime,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleForget(server, runtime);
}

inline void handleApiEnable(WebServer& server,
                            const Runtime& runtime,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleEnable(server, runtime);
}

}  // namespace WifiClientApiService

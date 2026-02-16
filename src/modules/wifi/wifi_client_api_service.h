#pragma once

#include <Arduino.h>
#include <WebServer.h>
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

}  // namespace WifiClientApiService

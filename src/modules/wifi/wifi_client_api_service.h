#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>
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
    bool (*isEnabled)(void* ctx);
    void* isEnabledCtx;
    String (*getSavedSsid)(void* ctx);
    void* getSavedSsidCtx;
    const char* (*getStateName)(void* ctx);
    void* getStateNameCtx;
    bool (*isScanRunning)(void* ctx);
    void* isScanRunningCtx;
    bool (*isConnected)(void* ctx);
    void* isConnectedCtx;
    ConnectedNetworkPayload (*getConnectedNetwork)(void* ctx);
    void* getConnectedNetworkCtx;

    bool (*isScanInProgress)(void* ctx);
    void* isScanInProgressCtx;
    bool (*hasCompletedScanResults)(void* ctx);
    void* hasCompletedScanResultsCtx;
    std::vector<ScannedNetworkPayload> (*getScannedNetworks)(void* ctx);
    void* getScannedNetworksCtx;
    bool (*startScan)(void* ctx);
    void* startScanCtx;

    bool (*connectToNetwork)(const String& ssid, const String& password, void* ctx);
    void* connectToNetworkCtx;
    void (*disconnectFromNetwork)(void* ctx);
    void* disconnectFromNetworkCtx;
    void (*forgetClient)(void* ctx);
    void* forgetClientCtx;
    bool (*enableWithSavedNetwork)(void* ctx);
    void* enableWithSavedNetworkCtx;
    void (*disableClient)(void* ctx);
    void* disableClientCtx;
};

void handleApiStatus(WebServer& server,
                     const Runtime& runtime,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiScan(WebServer& server,
                   const Runtime& runtime,
                   bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                   void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiConnect(WebServer& server,
                      const Runtime& runtime,
                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                      void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiDisconnect(WebServer& server,
                         const Runtime& runtime,
                         bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                         void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiForget(WebServer& server,
                     const Runtime& runtime,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiEnable(WebServer& server,
                     const Runtime& runtime,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

}  // namespace WifiClientApiService

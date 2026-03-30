#pragma once

#include <WebServer.h>

class ObdRuntimeModule;
class SettingsManager;
class SpeedSourceSelector;

namespace ObdApiService {

void handleApiConfigGet(WebServer& server,
                        SettingsManager& settingsManager,
                        void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiStatus(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiDevicesList(WebServer& server,
                          ObdRuntimeModule& obdRuntime,
                          SettingsManager& settingsManager,
                          void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiDeviceNameSave(WebServer& server,
                             SettingsManager& settingsManager,
                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                             void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiScan(WebServer& server,
                   ObdRuntimeModule& obdRuntime,
                   bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                   void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiForget(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     SettingsManager& settingsManager,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiConfig(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     SettingsManager& settingsManager,
                     SpeedSourceSelector& speedSourceSelector,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

}  // namespace ObdApiService

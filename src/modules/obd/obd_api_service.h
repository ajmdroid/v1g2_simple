#pragma once

#include <WebServer.h>

#include <functional>

class ObdRuntimeModule;
class SettingsManager;

namespace ObdApiService {

void handleApiConfigGet(WebServer& server,
                        SettingsManager& settingsManager,
                        const std::function<void()>& markUiActivity);

void handleApiStatus(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     const std::function<void()>& markUiActivity);

void handleApiScan(WebServer& server,
                   ObdRuntimeModule& obdRuntime,
                   const std::function<bool()>& checkRateLimit,
                   const std::function<void()>& markUiActivity);

void handleApiForget(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     SettingsManager& settingsManager,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

void handleApiConfig(WebServer& server,
                     ObdRuntimeModule& obdRuntime,
                     SettingsManager& settingsManager,
                     const std::function<void(bool)>& setSpeedSourceObdEnabled,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

}  // namespace ObdApiService

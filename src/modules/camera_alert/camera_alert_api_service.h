#pragma once

#include <WebServer.h>

#include <functional>

class CameraAlertModule;
class RoadMapReader;
class SettingsManager;

namespace CameraAlertApiService {

void handleApiSettingsGet(WebServer& server,
                          SettingsManager& settingsManager,
                          const std::function<void()>& markUiActivity);

void handleApiSettingsPost(WebServer& server,
                           SettingsManager& settingsManager,
                           const std::function<bool()>& checkRateLimit,
                           const std::function<void()>& markUiActivity);

void handleApiStatus(WebServer& server,
                     CameraAlertModule& cameraAlertModule,
                     RoadMapReader& roadMapReader,
                     const std::function<void()>& markUiActivity);

}  // namespace CameraAlertApiService

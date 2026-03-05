#pragma once

#include <WebServer.h>

#include <functional>

#include "camera_alert_module.h"
#include "../../settings.h"

namespace CameraAlertApiService {

struct Runtime {
    std::function<const V1Settings&()> getSettings;
    std::function<V1Settings&()> getMutableSettings;
    std::function<void()> save;
    std::function<uint32_t()> getCameraCount;
    std::function<bool(CameraAlertStatusSnapshot&)> getCameraStatus;
};

void handleApiSettingsGet(WebServer& server, const Runtime& runtime);

void handleApiSettingsSave(WebServer& server,
                           const Runtime& runtime,
                           const std::function<bool()>& checkRateLimit);

void handleApiStatusGet(WebServer& server, const Runtime& runtime);

}  // namespace CameraAlertApiService

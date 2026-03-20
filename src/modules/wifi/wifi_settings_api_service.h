#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>
#include <functional>

#include "../../settings.h"

namespace WifiSettingsApiService {

struct Runtime {
    std::function<const V1Settings&()> getSettings;
    std::function<void(const DeviceSettingsUpdate&)> applySettingsUpdate;
};

void handleApiDeviceSettingsGet(WebServer& server, const Runtime& runtime);

void handleApiDeviceSettingsSave(WebServer& server,
                                 const Runtime& runtime,
                                 const std::function<bool()>& checkRateLimit);

}  // namespace WifiSettingsApiService

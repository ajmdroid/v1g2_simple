#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>
#include <functional>

#include "../../settings.h"

namespace WifiSettingsApiService {

struct Runtime {
    std::function<const V1Settings&()> getSettings;
    std::function<V1Settings&()> getMutableSettings;
    std::function<void(const String&, const String&)> updateAPCredentials;
    std::function<void()> persistSettings;
};

void handleApiDeviceSettingsGet(WebServer& server, const Runtime& runtime);

void handleApiDeviceSettingsSave(WebServer& server,
                                 const Runtime& runtime,
                                 const std::function<bool()>& checkRateLimit);

}  // namespace WifiSettingsApiService

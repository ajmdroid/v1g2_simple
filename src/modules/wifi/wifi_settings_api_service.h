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
    std::function<void(bool)> setGpsRuntimeEnabled;
    std::function<void(bool)> setSpeedSourceGpsEnabled;
    std::function<void(bool)> setLockoutKaLearningEnabled;
    std::function<void(bool)> setLockoutKLearningEnabled;
    std::function<void(bool)> setLockoutXLearningEnabled;
    std::function<void(bool)> setObdRuntimeEnabled;
    std::function<void(int8_t)> setObdRuntimeMinRssi;
    std::function<void(bool)> setSpeedSourceObdEnabled;
    std::function<void()> save;
};

void handleApiDeviceSettingsGet(WebServer& server, const Runtime& runtime);

void handleApiDeviceSettingsSave(WebServer& server,
                                 const Runtime& runtime,
                                 const std::function<bool()>& checkRateLimit);

void handleApiSettingsSave(WebServer& server,
                           const Runtime& runtime,
                           const std::function<bool()>& checkRateLimit);

}  // namespace WifiSettingsApiService

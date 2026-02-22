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
    std::function<void(uint8_t)> updateBrightness;
    std::function<void(DisplayStyle)> updateDisplayStyle;
    std::function<void()> forceDisplayRedraw;
    std::function<void(bool)> setObdVwDataEnabled;
    std::function<void()> stopObdScan;
    std::function<void()> disconnectObd;
    std::function<void(bool)> setGpsRuntimeEnabled;
    std::function<void(bool)> setSpeedSourceGpsEnabled;
    std::function<void(bool)> setCameraRuntimeEnabled;
    std::function<void(uint16_t, uint8_t)> setCameraRuntimeAlertTuning;
    std::function<void(bool)> setLockoutKaLearningEnabled;
    std::function<void()> save;
};

void handleApiSettingsGet(WebServer& server, const Runtime& runtime);

void handleApiSettingsSave(WebServer& server,
                           const Runtime& runtime,
                           const std::function<bool()>& checkRateLimit);

void handleApiLegacySettingsSave(WebServer& server,
                                 const Runtime& runtime,
                                 const std::function<bool()>& checkRateLimit,
                                 const std::function<void()>& sendDeprecatedHeader,
                                 const std::function<void()>& logLegacyUsage);

}  // namespace WifiSettingsApiService

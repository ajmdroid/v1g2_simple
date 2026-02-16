#pragma once

#include <Arduino.h>
#include <WebServer.h>

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
    std::function<void(bool)> setLockoutKaLearningEnabled;
    std::function<void()> save;
};

void handleSettingsGet(WebServer& server, const Runtime& runtime);

void handleSettingsSave(WebServer& server,
                        const Runtime& runtime,
                        const std::function<bool()>& checkRateLimit);

inline void handleApiSettingsGet(WebServer& server, const Runtime& runtime) {
    handleSettingsGet(server, runtime);
}

inline void handleApiSettingsSave(WebServer& server,
                                  const Runtime& runtime,
                                  const std::function<bool()>& checkRateLimit) {
    handleSettingsSave(server, runtime, checkRateLimit);
}

void handleLegacySettingsSave(WebServer& server,
                              const Runtime& runtime,
                              const std::function<bool()>& checkRateLimit,
                              const std::function<void()>& sendDeprecatedHeader,
                              const std::function<void()>& logLegacyUsage);

inline void handleApiLegacySettingsSave(WebServer& server,
                                        const Runtime& runtime,
                                        const std::function<bool()>& checkRateLimit,
                                        const std::function<void()>& sendDeprecatedHeader,
                                        const std::function<void()>& logLegacyUsage) {
    handleLegacySettingsSave(
        server, runtime, checkRateLimit, sendDeprecatedHeader, logLegacyUsage);
}

}  // namespace WifiSettingsApiService

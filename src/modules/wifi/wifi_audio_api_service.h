#pragma once

#include <WebServer.h>

#include <cstdint>
#include <functional>

#include "../../settings.h"

namespace WifiAudioApiService {

struct Runtime {
    std::function<const V1Settings&()> getSettings;
    std::function<V1Settings&()> getMutableSettings;
    std::function<void(uint8_t)> setAudioVolume;
    std::function<void()> saveSettings;
};

void handleApiGet(WebServer& server, const Runtime& runtime);

void handleApiSave(WebServer& server,
                   const Runtime& runtime,
                   const std::function<bool()>& checkRateLimit);

}  // namespace WifiAudioApiService

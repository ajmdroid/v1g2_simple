#pragma once

#include <WebServer.h>

#include <cstdint>
#include <functional>

#include "../../settings.h"

namespace WifiDisplayColorsApiService {

struct Runtime {
    std::function<const V1Settings&()> getSettings;
    std::function<V1Settings&()> getMutableSettings;
    std::function<void(uint8_t)> setDisplayBrightness;
    std::function<void(DisplayStyle)> updateDisplayStyle;
    std::function<void()> forceDisplayRedraw;
    std::function<void(uint32_t)> requestColorPreviewHoldMs;
    std::function<bool()> isColorPreviewRunning;
    std::function<void()> cancelColorPreview;
    std::function<void()> persistSettings;
};

void handleApiGet(WebServer& server, const Runtime& runtime);

void handleApiSave(WebServer& server,
                   const Runtime& runtime,
                   const std::function<bool()>& checkRateLimit);

void handleApiReset(WebServer& server,
                    const Runtime& runtime,
                    const std::function<bool()>& checkRateLimit);

void handleApiPreview(WebServer& server,
                      const Runtime& runtime,
                      const std::function<bool()>& checkRateLimit);

void handleApiClear(WebServer& server,
                    const Runtime& runtime,
                    const std::function<bool()>& checkRateLimit);

}  // namespace WifiDisplayColorsApiService

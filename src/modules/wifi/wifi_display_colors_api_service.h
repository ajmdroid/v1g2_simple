#pragma once

#include <WebServer.h>

#include <cstdint>

#include "../../settings.h"

namespace WifiDisplayColorsApiService {

struct Runtime {
    const V1Settings& (*getSettings)(void* ctx);
    void* getSettingsCtx;
    void (*applySettingsUpdate)(const DisplaySettingsUpdate& update, void* ctx);
    void* applySettingsUpdateCtx;
    void (*resetDisplaySettings)(void* ctx);
    void* resetDisplaySettingsCtx;
    void (*setDisplayBrightness)(uint8_t brightness, void* ctx);
    void* setDisplayBrightnessCtx;
    void (*forceDisplayRedraw)(void* ctx);
    void* forceDisplayRedrawCtx;
    void (*requestColorPreviewHoldMs)(uint32_t durationMs, void* ctx);
    void* requestColorPreviewHoldMsCtx;
    bool (*isColorPreviewRunning)(void* ctx);
    void* isColorPreviewRunningCtx;
    void (*cancelColorPreview)(void* ctx);
    void* cancelColorPreviewCtx;
};

void handleApiGet(WebServer& server, const Runtime& runtime);

void handleApiSave(WebServer& server,
                   const Runtime& runtime,
                   bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiReset(WebServer& server,
                    const Runtime& runtime,
                    bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiPreview(WebServer& server,
                      const Runtime& runtime,
                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiClear(WebServer& server,
                    const Runtime& runtime,
                    bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

}  // namespace WifiDisplayColorsApiService

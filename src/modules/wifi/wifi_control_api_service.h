#pragma once

#include <WebServer.h>

namespace WifiControlApiService {

enum class ProfilePushResult : uint8_t {
    QUEUED = 0,
    ALREADY_IN_PROGRESS,
    HANDLER_UNAVAILABLE,
};

void handleApiProfilePush(WebServer& server,
                          bool v1Connected,
                          ProfilePushResult (*requestProfilePush)(void* ctx),
                          void* pushCtx,
                          bool (*checkRateLimit)(void* ctx),
                          void* rateLimitCtx);

void handleApiDarkMode(WebServer& server,
                       bool (*sendV1Command)(const char* cmd, bool val, void* ctx),
                       void* cmdCtx,
                       bool (*checkRateLimit)(void* ctx),
                       void* rateLimitCtx);

void handleApiMute(WebServer& server,
                   bool (*sendV1Command)(const char* cmd, bool val, void* ctx),
                   void* cmdCtx,
                   bool (*checkRateLimit)(void* ctx),
                   void* rateLimitCtx);

}  // namespace WifiControlApiService

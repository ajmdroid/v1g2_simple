#pragma once

#include <WebServer.h>

#include <functional>

namespace WifiControlApiService {

enum class ProfilePushResult : uint8_t {
    QUEUED = 0,
    ALREADY_IN_PROGRESS,
    HANDLER_UNAVAILABLE,
};

void handleApiProfilePush(WebServer& server,
                          bool v1Connected,
                          const std::function<ProfilePushResult()>& requestProfilePush,
                          const std::function<bool()>& checkRateLimit);

void handleApiDarkMode(WebServer& server,
                       const std::function<bool(const char*, bool)>& sendV1Command,
                       const std::function<bool()>& checkRateLimit);

void handleApiMute(WebServer& server,
                   const std::function<bool(const char*, bool)>& sendV1Command,
                   const std::function<bool()>& checkRateLimit);

}  // namespace WifiControlApiService

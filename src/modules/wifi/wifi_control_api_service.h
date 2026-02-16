#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <functional>

namespace WifiControlApiService {

void handleApiProfilePush(WebServer& server,
                          bool v1Connected,
                          const std::function<bool()>& requestProfilePush,
                          const std::function<bool()>& checkRateLimit);

void handleProfilePush(WebServer& server,
                       bool v1Connected,
                       const std::function<bool()>& requestProfilePush,
                       const std::function<bool()>& checkRateLimit);

void handleDarkMode(WebServer& server,
                    const std::function<bool(const char*, bool)>& sendV1Command,
                    const std::function<bool()>& checkRateLimit);

void handleMute(WebServer& server,
                const std::function<bool(const char*, bool)>& sendV1Command,
                const std::function<bool()>& checkRateLimit);

inline void handleApiDarkMode(WebServer& server,
                              const std::function<bool(const char*, bool)>& sendV1Command,
                              const std::function<bool()>& checkRateLimit) {
    handleDarkMode(server, sendV1Command, checkRateLimit);
}

inline void handleApiMute(WebServer& server,
                          const std::function<bool(const char*, bool)>& sendV1Command,
                          const std::function<bool()>& checkRateLimit) {
    handleMute(server, sendV1Command, checkRateLimit);
}

}  // namespace WifiControlApiService

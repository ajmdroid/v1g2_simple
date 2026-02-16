#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <functional>

namespace WifiControlApiService {

void handleApiProfilePush(WebServer& server,
                          bool v1Connected,
                          const std::function<bool()>& requestProfilePush,
                          const std::function<bool()>& checkRateLimit);

void handleApiDarkMode(WebServer& server,
                       const std::function<bool(const char*, bool)>& sendV1Command,
                       const std::function<bool()>& checkRateLimit);

void handleApiMute(WebServer& server,
                   const std::function<bool(const char*, bool)>& sendV1Command,
                   const std::function<bool()>& checkRateLimit);

}  // namespace WifiControlApiService

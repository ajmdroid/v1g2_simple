#pragma once

#include <WebServer.h>

#include <functional>

namespace WifiPortalApiService {

void handlePing(WebServer& server,
                const std::function<void()>& markUiActivity);

void handleGenerate204(WebServer& server,
                       const std::function<void()>& markUiActivity);

void handleGen204(WebServer& server,
                  const std::function<void()>& markUiActivity);

void handleNcsiTxt(WebServer& server);

}  // namespace WifiPortalApiService

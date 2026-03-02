#pragma once

#include <WebServer.h>

#include <functional>

namespace WifiPortalApiService {

void handleApiPing(WebServer& server,
                   const std::function<void()>& markUiActivity);

void handleApiGenerate204(WebServer& server,
                          const std::function<void()>& markUiActivity);

void handleApiGen204(WebServer& server,
                     const std::function<void()>& markUiActivity);

void handleApiHotspotDetect(WebServer& server,
                            const std::function<void()>& markUiActivity);

void handleApiFwlink(WebServer& server);

void handleApiRedirectToRoot(WebServer& server);

void handleApiNcsiTxt(WebServer& server);

}  // namespace WifiPortalApiService

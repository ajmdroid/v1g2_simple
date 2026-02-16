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

void handleHotspotDetect(WebServer& server,
                         const std::function<void()>& markUiActivity);

void handleFwlink(WebServer& server);

void handleRedirectToRoot(WebServer& server);

void handleDeprecatedRedirectToRoot(WebServer& server,
                                    const char* deprecationHint);

void handleNcsiTxt(WebServer& server);

inline void handleApiPing(WebServer& server,
                          const std::function<void()>& markUiActivity) {
    handlePing(server, markUiActivity);
}

inline void handleApiGenerate204(WebServer& server,
                                 const std::function<void()>& markUiActivity) {
    handleGenerate204(server, markUiActivity);
}

inline void handleApiGen204(WebServer& server,
                            const std::function<void()>& markUiActivity) {
    handleGen204(server, markUiActivity);
}

inline void handleApiHotspotDetect(WebServer& server,
                                   const std::function<void()>& markUiActivity) {
    handleHotspotDetect(server, markUiActivity);
}

inline void handleApiFwlink(WebServer& server) {
    handleFwlink(server);
}

inline void handleApiNcsiTxt(WebServer& server) {
    handleNcsiTxt(server);
}

}  // namespace WifiPortalApiService

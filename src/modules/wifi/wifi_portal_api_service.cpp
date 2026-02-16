#include "wifi_portal_api_service.h"

namespace WifiPortalApiService {

void handlePing(WebServer& server,
                const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    Serial.println("[HTTP] GET /ping");
    server.send(200, "text/plain", "OK");
}

void handleGenerate204(WebServer& server,
                       const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    Serial.println("[HTTP] GET /generate_204");
    server.send(204, "text/plain", "");
}

void handleGen204(WebServer& server,
                  const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    Serial.println("[HTTP] GET /gen_204");
    server.send(204, "text/plain", "");
}

void handleHotspotDetect(WebServer& server,
                         const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    Serial.println("[HTTP] GET /hotspot-detect.html");
    server.sendHeader("Location", "/settings", true);
    server.send(302, "text/html", "");
}

void handleFwlink(WebServer& server) {
    Serial.println("[HTTP] GET /fwlink");
    server.sendHeader("Location", "/settings", true);
    server.send(302, "text/html", "");
}

void handleRedirectToRoot(WebServer& server) {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "Redirecting to /");
}

void handleDeprecatedRedirectToRoot(WebServer& server,
                                    const char* deprecationHint) {
    if (deprecationHint && deprecationHint[0] != '\0') {
        server.sendHeader("X-API-Deprecated", deprecationHint);
    }
    handleRedirectToRoot(server);
}

void handleNcsiTxt(WebServer& server) {
    Serial.println("[HTTP] GET /ncsi.txt");
    server.send(200, "text/plain", "Microsoft NCSI");
}

}  // namespace WifiPortalApiService

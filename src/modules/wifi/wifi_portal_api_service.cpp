#include "wifi_portal_api_service.h"

namespace WifiPortalApiService {

void handleApiPing(WebServer& server,
                   const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    Serial.println("[HTTP] GET /ping");
    server.send(200, "text/plain", "OK");
}

void handleApiGenerate204(WebServer& server,
                          const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    Serial.println("[HTTP] GET /generate_204");
    server.send(204, "text/plain", "");
}

void handleApiGen204(WebServer& server,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    Serial.println("[HTTP] GET /gen_204");
    server.send(204, "text/plain", "");
}

void handleApiHotspotDetect(WebServer& server,
                            const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    Serial.println("[HTTP] GET /hotspot-detect.html");
    server.sendHeader("Location", "/settings", true);
    server.send(302, "text/html", "");
}

void handleApiFwlink(WebServer& server) {
    Serial.println("[HTTP] GET /fwlink");
    server.sendHeader("Location", "/settings", true);
    server.send(302, "text/html", "");
}

void handleApiNcsiTxt(WebServer& server) {
    Serial.println("[HTTP] GET /ncsi.txt");
    server.send(200, "text/plain", "Microsoft NCSI");
}

}  // namespace WifiPortalApiService

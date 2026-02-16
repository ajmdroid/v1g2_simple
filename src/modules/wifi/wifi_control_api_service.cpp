#include "wifi_control_api_service.h"

#include <ArduinoJson.h>

#ifdef UNIT_TEST
#include <string>
#endif

namespace WifiControlApiService {

namespace {

void sendJsonDocument(WebServer& server, int statusCode, const JsonDocument& doc) {
#ifdef UNIT_TEST
    std::string response;
    serializeJson(doc, response);
    server.send(statusCode, "application/json", response.c_str());
#else
    String response;
    serializeJson(doc, response);
    server.send(statusCode, "application/json", response);
#endif
}

}  // namespace

void handleProfilePush(WebServer& server,
                       bool v1Connected,
                       const std::function<bool()>& requestProfilePush,
                       const std::function<bool()>& checkRateLimit) {
    // Preserve existing rate-limit behavior for this route.
    if (checkRateLimit && !checkRateLimit()) return;

    if (!v1Connected) {
        server.send(503, "application/json",
                    "{\"error\":\"V1 not connected\"}");
        return;
    }

    bool queued = false;
    if (requestProfilePush) {
        queued = requestProfilePush();
    }

    JsonDocument doc;
    doc["ok"] = queued;
    if (queued) {
        doc["message"] = "Profile push queued - check display for progress";
    } else {
        doc["error"] = "Push handler unavailable";
    }
    sendJsonDocument(server, queued ? 200 : 500, doc);
}

void handleDarkMode(WebServer& server,
                    const std::function<bool(const char*, bool)>& sendV1Command,
                    const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("state")) {
        server.send(400, "application/json", "{\"error\":\"Missing state parameter\"}");
        return;
    }

    bool darkMode = server.arg("state") == "1" || server.arg("state") == "true";
    bool success = false;

    if (sendV1Command) {
        // Dark mode = display OFF, so invert the command parameter.
        success = sendV1Command("display", !darkMode);
    }

    Serial.printf("Dark mode request: %s, success: %s\n", darkMode ? "ON" : "OFF", success ? "yes" : "no");

    JsonDocument doc;
    doc["success"] = success;
    doc["darkMode"] = darkMode;
    sendJsonDocument(server, 200, doc);
}

void handleMute(WebServer& server,
                const std::function<bool(const char*, bool)>& sendV1Command,
                const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("state")) {
        server.send(400, "application/json", "{\"error\":\"Missing state parameter\"}");
        return;
    }

    bool muted = server.arg("state") == "1" || server.arg("state") == "true";
    bool success = false;

    if (sendV1Command) {
        success = sendV1Command("mute", muted);
    }

    Serial.printf("Mute request: %s, success: %s\n", muted ? "ON" : "OFF", success ? "yes" : "no");

    JsonDocument doc;
    doc["success"] = success;
    doc["muted"] = muted;
    sendJsonDocument(server, 200, doc);
}

}  // namespace WifiControlApiService

#include "wifi_control_api_service.h"
#include "wifi_api_response.h"

#include <ArduinoJson.h>

namespace WifiControlApiService {

static void handleProfilePushImpl(WebServer& server,
                                  bool v1Connected,
                                  const std::function<ProfilePushResult()>& requestProfilePush,
                                  const std::function<bool()>& checkRateLimit) {
    // Preserve existing rate-limit behavior for this route.
    if (checkRateLimit && !checkRateLimit()) return;

    if (!v1Connected) {
        JsonDocument doc;
        WifiApiResponse::setErrorAndMessage(doc, "V1 not connected");
        WifiApiResponse::sendJsonDocument(server, 503, doc);
        return;
    }

    JsonDocument doc;
    ProfilePushResult result = ProfilePushResult::HANDLER_UNAVAILABLE;
    if (requestProfilePush) {
        result = requestProfilePush();
    }

    switch (result) {
        case ProfilePushResult::QUEUED:
            doc["ok"] = true;
            doc["message"] = "Profile push queued - check display for progress";
            WifiApiResponse::sendJsonDocument(server, 200, doc);
            return;
        case ProfilePushResult::ALREADY_IN_PROGRESS:
            doc["ok"] = false;
            WifiApiResponse::setErrorAndMessage(doc, "Push already in progress");
            WifiApiResponse::sendJsonDocument(server, 409, doc);
            return;
        case ProfilePushResult::HANDLER_UNAVAILABLE:
        default:
            doc["ok"] = false;
            WifiApiResponse::setErrorAndMessage(doc, "Push handler unavailable");
            WifiApiResponse::sendJsonDocument(server, 500, doc);
            return;
    }
}

void handleApiProfilePush(WebServer& server,
                          bool v1Connected,
                          const std::function<ProfilePushResult()>& requestProfilePush,
                          const std::function<bool()>& checkRateLimit) {
    // Preserve existing route behavior: route-level guard + delegate-level guard.
    if (checkRateLimit && !checkRateLimit()) return;
    handleProfilePushImpl(server, v1Connected, requestProfilePush, checkRateLimit);
}

void handleApiDarkMode(WebServer& server,
                       const std::function<bool(const char*, bool)>& sendV1Command,
                       const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("state")) {
        JsonDocument doc;
        WifiApiResponse::setErrorAndMessage(doc, "Missing state parameter");
        WifiApiResponse::sendJsonDocument(server, 400, doc);
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
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiMute(WebServer& server,
                   const std::function<bool(const char*, bool)>& sendV1Command,
                   const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("state")) {
        JsonDocument doc;
        WifiApiResponse::setErrorAndMessage(doc, "Missing state parameter");
        WifiApiResponse::sendJsonDocument(server, 400, doc);
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
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

}  // namespace WifiControlApiService

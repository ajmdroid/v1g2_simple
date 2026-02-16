#include "wifi_client_api_service.h"

#include <ArduinoJson.h>

#ifdef UNIT_TEST
#include <string>
#endif

namespace WifiClientApiService {

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

void sendStatus(WebServer& server, const StatusPayload& payload) {
    JsonDocument doc;
    doc["enabled"] = payload.enabled;
    doc["savedSSID"] = payload.savedSsid;
    doc["state"] = payload.state;

    if (payload.includeConnectedFields) {
        doc["connectedSSID"] = payload.connectedSsid;
        doc["ip"] = payload.ip;
        doc["rssi"] = payload.rssi;
    }

    doc["scanRunning"] = payload.scanRunning;
    sendJsonDocument(server, 200, doc);
}

void sendScanInProgress(WebServer& server) {
    server.send(200, "application/json", "{\"scanning\":true,\"networks\":[]}");
}

void sendScanResults(WebServer& server,
                     const std::vector<ScannedNetworkPayload>& networks) {
    JsonDocument doc;
    doc["scanning"] = false;
    JsonArray arr = doc["networks"].to<JsonArray>();

    for (const auto& net : networks) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = net.ssid;
        obj["rssi"] = net.rssi;
        obj["secure"] = net.secure;
    }

    sendJsonDocument(server, 200, doc);
}

void sendScanStartFailed(WebServer& server) {
    server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to start scan\"}");
}

bool parseConnectRequest(WebServer& server,
                         String& ssidOut,
                         String& passwordOut,
                         const char*& errorMessageOut) {
    ssidOut = "";
    passwordOut = "";
    errorMessageOut = nullptr;

    if (!server.hasArg("plain")) {
        errorMessageOut = "Missing request body";
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain").c_str());
    if (error) {
        errorMessageOut = "Invalid JSON";
        return false;
    }

    ssidOut = doc["ssid"] | "";
    passwordOut = doc["password"] | "";
    if (ssidOut.length() == 0) {
        errorMessageOut = "SSID required";
        return false;
    }

    return true;
}

void sendConnectParseError(WebServer& server, const char* message) {
    JsonDocument doc;
    doc["success"] = false;
    doc["message"] = message ? message : "Invalid request";
    sendJsonDocument(server, 400, doc);
}

void sendConnectStarted(WebServer& server) {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Connecting...\"}");
}

void sendConnectStartFailed(WebServer& server) {
    server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to start connection\"}");
}

bool parseEnableRequest(WebServer& server, bool& enabledOut) {
    enabledOut = false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain").c_str());
    if (err || !doc["enabled"].is<bool>()) {
        return false;
    }
    enabledOut = doc["enabled"].as<bool>();
    return true;
}

void sendEnableParseError(WebServer& server) {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing enabled field\"}");
}

void sendEnableResult(WebServer& server, bool enabled) {
    if (enabled) {
        server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi client enabled\"}");
        return;
    }
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi client disabled\"}");
}

void sendDisconnected(WebServer& server) {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Disconnected\"}");
}

void sendForgotten(WebServer& server) {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi credentials forgotten\"}");
}

}  // namespace WifiClientApiService

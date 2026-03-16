#include "wifi_client_api_service.h"
#include "wifi_api_response.h"
#include "wifi_json_document.h"

#include <ArduinoJson.h>

namespace WifiClientApiService {

namespace {

static void sendStatus(WebServer& server, const StatusPayload& payload) {
    WifiJson::Document doc;
    doc["enabled"] = payload.enabled;
    doc["savedSSID"] = payload.savedSsid;
    doc["state"] = payload.state;

    if (payload.includeConnectedFields) {
        doc["connectedSSID"] = payload.connectedSsid;
        doc["ip"] = payload.ip;
        doc["rssi"] = payload.rssi;
    }

    doc["scanRunning"] = payload.scanRunning;
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

static void sendScanInProgress(WebServer& server) {
    server.send(200, "application/json", "{\"scanning\":true,\"networks\":[]}");
}

static void sendScanResults(WebServer& server,
                            const std::vector<ScannedNetworkPayload>& networks) {
    WifiJson::Document doc;
    doc["scanning"] = false;
    JsonArray arr = doc["networks"].to<JsonArray>();

    for (const auto& net : networks) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = net.ssid;
        obj["rssi"] = net.rssi;
        obj["secure"] = net.secure;
    }

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

static void sendScanStartFailed(WebServer& server) {
    server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to start scan\"}");
}

static bool parseConnectRequest(WebServer& server,
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

    WifiJson::Document doc;
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

static void sendConnectParseError(WebServer& server, const char* message) {
    WifiJson::Document doc;
    doc["success"] = false;
    WifiApiResponse::setErrorAndMessage(doc, message ? message : "Invalid request");
    WifiApiResponse::sendJsonDocument(server, 400, doc);
}

static void sendConnectStarted(WebServer& server) {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Connecting...\"}");
}

static void sendConnectStartFailed(WebServer& server) {
    server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to start connection\"}");
}

static bool parseEnableRequest(WebServer& server, bool& enabledOut) {
    enabledOut = false;
    WifiJson::Document doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain").c_str());
    if (err || !doc["enabled"].is<bool>()) {
        return false;
    }
    enabledOut = doc["enabled"].as<bool>();
    return true;
}

static void sendEnableParseError(WebServer& server) {
    WifiJson::Document doc;
    doc["success"] = false;
    WifiApiResponse::setErrorAndMessage(doc, "Missing enabled field");
    WifiApiResponse::sendJsonDocument(server, 400, doc);
}

static void sendEnableResult(WebServer& server, bool enabled) {
    if (enabled) {
        server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi client enabled\"}");
        return;
    }
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi client disabled\"}");
}

static void sendEnableConnectFailed(WebServer& server) {
    server.send(500, "application/json",
                "{\"success\":false,\"message\":\"Failed to start connection\"}");
}

static void sendDisconnected(WebServer& server) {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Disconnected\"}");
}

static void sendForgotten(WebServer& server) {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi credentials forgotten\"}");
}

}  // namespace

static void handleStatusImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.isEnabled || !runtime.getSavedSsid || !runtime.getStateName ||
        !runtime.isScanRunning || !runtime.isConnected) {
        server.send(500, "application/json", "{\"success\":false,\"message\":\"Runtime unavailable\"}");
        return;
    }

    StatusPayload payload;
    payload.enabled = runtime.isEnabled();
    payload.savedSsid = runtime.getSavedSsid();
    payload.state = runtime.getStateName();
    payload.scanRunning = runtime.isScanRunning();

    if (runtime.isConnected() && runtime.getConnectedNetwork) {
        const ConnectedNetworkPayload connected = runtime.getConnectedNetwork();
        payload.includeConnectedFields = true;
        payload.connectedSsid = connected.ssid;
        payload.ip = connected.ip;
        payload.rssi = connected.rssi;
    }

    sendStatus(server, payload);
}

static void handleScanImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.isScanRunning || !runtime.isScanInProgress ||
        !runtime.hasCompletedScanResults || !runtime.getScannedNetworks ||
        !runtime.startScan) {
        server.send(500, "application/json", "{\"success\":false,\"message\":\"Runtime unavailable\"}");
        return;
    }

    Serial.println("[HTTP] POST /api/wifi/scan");

    if (runtime.isScanRunning() && runtime.isScanInProgress()) {
        sendScanInProgress(server);
        return;
    }

    if (runtime.hasCompletedScanResults()) {
        sendScanResults(server, runtime.getScannedNetworks());
        return;
    }

    if (runtime.startScan()) {
        sendScanInProgress(server);
        return;
    }

    sendScanStartFailed(server);
}

static void handleConnectImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.connectToNetwork) {
        server.send(500, "application/json", "{\"success\":false,\"message\":\"Runtime unavailable\"}");
        return;
    }

    Serial.println("[HTTP] POST /api/wifi/connect");

    String ssid;
    String password;
    const char* errorMessage = nullptr;
    if (!parseConnectRequest(server, ssid, password, errorMessage)) {
        sendConnectParseError(server, errorMessage);
        return;
    }

    if (runtime.connectToNetwork(ssid, password)) {
        sendConnectStarted(server);
        return;
    }

    sendConnectStartFailed(server);
}

static void handleDisconnectImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.disconnectFromNetwork) {
        server.send(500, "application/json", "{\"success\":false,\"message\":\"Runtime unavailable\"}");
        return;
    }

    Serial.println("[HTTP] POST /api/wifi/disconnect");

    runtime.disconnectFromNetwork();
    sendDisconnected(server);
}

static void handleForgetImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.disconnectFromNetwork || !runtime.clearCredentials ||
        !runtime.setStateDisabled || !runtime.setApMode) {
        server.send(500, "application/json", "{\"success\":false,\"message\":\"Runtime unavailable\"}");
        return;
    }

    Serial.println("[HTTP] POST /api/wifi/forget");

    runtime.disconnectFromNetwork();
    runtime.clearCredentials();
    runtime.setStateDisabled();
    runtime.setApMode();

    sendForgotten(server);
}

static void handleEnableImpl(WebServer& server, const Runtime& runtime) {
    if (!runtime.setWifiClientEnabled || !runtime.getSavedSsid ||
        !runtime.getSavedPassword || !runtime.connectToNetwork ||
        !runtime.setStateDisconnected || !runtime.disconnectFromNetwork ||
        !runtime.setStateDisabled || !runtime.setApMode) {
        server.send(500, "application/json", "{\"success\":false,\"message\":\"Runtime unavailable\"}");
        return;
    }

    bool enable = false;
    if (!parseEnableRequest(server, enable)) {
        sendEnableParseError(server);
        return;
    }

    Serial.printf("[HTTP] POST /api/wifi/enable: %s\n", enable ? "true" : "false");

    const String savedSsid = runtime.getSavedSsid();
    if (enable) {
        runtime.setWifiClientEnabled(true);

        if (savedSsid.length() > 0) {
            if (!runtime.connectToNetwork(savedSsid, runtime.getSavedPassword())) {
                runtime.setStateDisconnected();
                sendEnableConnectFailed(server);
                return;
            }
        } else {
            runtime.setStateDisconnected();
        }

        sendEnableResult(server, true);
        return;
    }

    runtime.disconnectFromNetwork();
    runtime.setWifiClientEnabled(false);
    runtime.setStateDisabled();
    runtime.setApMode();
    sendEnableResult(server, false);
}

void handleApiStatus(WebServer& server,
                     const Runtime& runtime,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    handleStatusImpl(server, runtime);
}

void handleApiScan(WebServer& server,
                   const Runtime& runtime,
                   const std::function<bool()>& checkRateLimit,
                   const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleScanImpl(server, runtime);
}

void handleApiConnect(WebServer& server,
                      const Runtime& runtime,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleConnectImpl(server, runtime);
}

void handleApiDisconnect(WebServer& server,
                         const Runtime& runtime,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleDisconnectImpl(server, runtime);
}

void handleApiForget(WebServer& server,
                     const Runtime& runtime,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleForgetImpl(server, runtime);
}

void handleApiEnable(WebServer& server,
                     const Runtime& runtime,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleEnableImpl(server, runtime);
}

}  // namespace WifiClientApiService

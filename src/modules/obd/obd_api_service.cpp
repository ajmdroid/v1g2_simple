#include "obd_api_service.h"

#include <ArduinoJson.h>
#include <climits>
#include <string>

#ifdef UNIT_TEST
#include "../../../test/mocks/ble_client.h"
#include "../../../test/mocks/obd_handler.h"
#include "../../../test/mocks/settings.h"
#else
#include "ble_client.h"
#include "obd_handler.h"
#include "settings.h"
#endif

namespace ObdApiService {

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

void sendError(WebServer& server, int statusCode, const char* message) {
    JsonDocument doc;
    doc["success"] = false;
    doc["message"] = message;
    sendJsonDocument(server, statusCode, doc);
}

}  // namespace

void sendStatus(WebServer& server,
                OBDHandler& obdHandler,
                V1BLEClient& bleClient,
                const V1Settings& settings) {
    JsonDocument doc;
    OBDData data = obdHandler.getData();
    const uint32_t nowMs = millis();
    const uint32_t dataAgeMs = (data.timestamp_ms > 0 && nowMs >= data.timestamp_ms)
                                   ? (nowMs - data.timestamp_ms)
                                   : 0;

    doc["state"] = obdHandler.getStateString();
    doc["connected"] = obdHandler.isConnected();
    doc["scanning"] = obdHandler.isScanActive();
    doc["deviceName"] = obdHandler.getConnectedDeviceName();
    doc["deviceAddress"] = obdHandler.getConnectedDeviceAddress();
    doc["v1Connected"] = bleClient.isConnected();
    doc["hasValidData"] = obdHandler.hasValidData();
    doc["speedMph"] = data.speed_mph;
    doc["speedKph"] = data.speed_kph;
    doc["rpm"] = data.rpm;
    doc["voltage"] = data.voltage;
    doc["sampleTsMs"] = data.timestamp_ms;
    doc["sampleAgeMs"] = dataAgeMs;
    doc["vwDataEnabled"] = settings.obdVwDataEnabled;

    if (data.oil_temp_c == INT16_MIN) {
        doc["oilTempC"] = nullptr;
    } else {
        doc["oilTempC"] = data.oil_temp_c;
    }

    if (data.dsg_temp_c == -128) {
        doc["dsgTempC"] = nullptr;
    } else {
        doc["dsgTempC"] = data.dsg_temp_c;
    }

    if (data.intake_air_temp_c == -128) {
        doc["intakeAirTempC"] = nullptr;
    } else {
        doc["intakeAirTempC"] = data.intake_air_temp_c;
    }

    auto remembered = obdHandler.getRememberedDevices();
    doc["rememberedCount"] = remembered.size();
    size_t autoCount = 0;
    for (const auto& d : remembered) {
        if (d.autoConnect) {
            autoCount++;
        }
    }
    doc["autoConnectCount"] = autoCount;

    sendJsonDocument(server, 200, doc);
}

void sendDevices(WebServer& server, OBDHandler& obdHandler) {
    JsonDocument doc;
    JsonArray devices = doc["devices"].to<JsonArray>();
    auto found = obdHandler.getFoundDevices();
    for (const auto& device : found) {
        JsonObject d = devices.add<JsonObject>();
        d["address"] = device.address;
        d["name"] = device.name;
        d["rssi"] = device.rssi;
    }
    doc["scanning"] = obdHandler.isScanActive();
    doc["count"] = found.size();

    sendJsonDocument(server, 200, doc);
}

void sendRemembered(WebServer& server, OBDHandler& obdHandler) {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    auto remembered = obdHandler.getRememberedDevices();
    const bool obdConnected = obdHandler.isConnected();
    String connectedAddr = obdHandler.getConnectedDeviceAddress();
    for (const auto& device : remembered) {
        JsonObject d = arr.add<JsonObject>();
        d["address"] = device.address;
        d["name"] = device.name;
        d["autoConnect"] = device.autoConnect;
        d["pinSet"] = device.pin.length() > 0;
        d["lastSeenMs"] = device.lastSeenMs;
        d["connected"] = obdConnected &&
                         connectedAddr.length() > 0 &&
                         connectedAddr.equalsIgnoreCase(device.address);
    }
    doc["count"] = remembered.size();

    sendJsonDocument(server, 200, doc);
}

void handleScan(WebServer& server, OBDHandler& obdHandler, V1BLEClient& bleClient) {
    if (!bleClient.isConnected()) {
        sendError(server, 409, "Connect V1 before OBD scan");
        return;
    }

    obdHandler.startScan();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"OBD scan started\"}");
}

void handleScanStop(WebServer& server, OBDHandler& obdHandler) {
    obdHandler.stopScan();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"OBD scan stopped\"}");
}

void handleDevicesClear(WebServer& server, OBDHandler& obdHandler) {
    obdHandler.clearFoundDevices();
    server.send(200, "application/json", "{\"success\":true}");
}

void handleConnect(WebServer& server, OBDHandler& obdHandler, V1BLEClient& bleClient) {
    ConnectRequest request;
    String errorMessage;
    if (!parseConnectRequest(server, request, errorMessage)) {
        sendError(server, 400, errorMessage.c_str());
        return;
    }

    if (!bleClient.isConnected()) {
        sendError(server, 409, "Connect V1 before OBD connect");
        return;
    }

    bool queued = obdHandler.connectToAddress(request.address,
                                              request.name,
                                              request.pin,
                                              request.remember,
                                              request.autoConnect);
    if (!queued) {
        sendError(server, 500, "Failed to queue OBD connect");
        return;
    }

    server.send(200, "application/json", "{\"success\":true,\"message\":\"OBD connect queued\"}");
}

void handleDisconnect(WebServer& server, OBDHandler& obdHandler) {
    obdHandler.disconnect();
    server.send(200, "application/json", "{\"success\":true}");
}

void handleConfig(WebServer& server, OBDHandler& obdHandler, SettingsManager& settingsManager) {
    bool enabled = settingsManager.get().obdVwDataEnabled;
    String errorMessage;
    if (!parseVwDataEnabledRequest(server,
                                   settingsManager.get().obdVwDataEnabled,
                                   enabled,
                                   errorMessage)) {
        sendError(server, 400, errorMessage.c_str());
        return;
    }

    settingsManager.setObdVwDataEnabled(enabled);
    obdHandler.setVwDataEnabled(enabled);

    JsonDocument doc;
    doc["success"] = true;
    doc["vwDataEnabled"] = enabled;
    sendJsonDocument(server, 200, doc);
}

void handleRememberedAutoConnect(WebServer& server, OBDHandler& obdHandler) {
    String address;
    bool enabled = false;
    String errorMessage;
    if (!parseRememberedAutoConnectRequest(server, address, enabled, errorMessage)) {
        sendError(server, 400, errorMessage.c_str());
        return;
    }

    if (!obdHandler.setRememberedAutoConnect(address, enabled)) {
        sendError(server, 404, "Remembered device not found");
        return;
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleForget(WebServer& server, OBDHandler& obdHandler) {
    String address;
    String errorMessage;
    if (!parseForgetAddressRequest(server, address, errorMessage)) {
        sendError(server, 400, errorMessage.c_str());
        return;
    }

    if (!obdHandler.forgetRemembered(address)) {
        sendError(server, 404, "Remembered device not found");
        return;
    }

    server.send(200, "application/json", "{\"success\":true}");
}

bool parseConnectRequest(WebServer& server, ConnectRequest& out, String& errorMessage) {
    errorMessage = "";

    ConnectRequest request;

    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain").c_str());
        if (error) {
            errorMessage = "Invalid JSON";
            return false;
        }

        request.address = doc["address"] | "";
        request.name = doc["name"] | "";
        request.pin = doc["pin"] | "";
        request.remember = doc["remember"].is<bool>() ? doc["remember"].as<bool>() : true;
        request.autoConnect = doc["autoConnect"].is<bool>() ? doc["autoConnect"].as<bool>() : false;
    } else {
        request.address = server.arg("address");
        request.name = server.arg("name");
        request.pin = server.arg("pin");

        if (server.hasArg("remember")) {
            String rememberRaw = server.arg("remember");
            request.remember = (rememberRaw == "1" || rememberRaw == "true");
        }
        if (server.hasArg("autoConnect")) {
            String autoConnectRaw = server.arg("autoConnect");
            request.autoConnect = (autoConnectRaw == "1" || autoConnectRaw == "true");
        }
    }

    if (request.address.length() == 0) {
        errorMessage = "Missing address";
        return false;
    }

    out = request;
    return true;
}

bool parseVwDataEnabledRequest(WebServer& server,
                               bool fallback,
                               bool& enabledOut,
                               String& errorMessage) {
    errorMessage = "";
    enabledOut = fallback;
    bool hasValue = false;

    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain").c_str());
        if (error || !doc["vwDataEnabled"].is<bool>()) {
            errorMessage = "Invalid payload";
            return false;
        }
        enabledOut = doc["vwDataEnabled"].as<bool>();
        hasValue = true;
    } else if (server.hasArg("vwDataEnabled")) {
        String value = server.arg("vwDataEnabled");
        value.toLowerCase();
        enabledOut = (value == "1" || value == "true" || value == "on");
        hasValue = true;
    }

    if (!hasValue) {
        errorMessage = "Missing vwDataEnabled";
        return false;
    }

    return true;
}

bool parseRememberedAutoConnectRequest(WebServer& server,
                                       String& addressOut,
                                       bool& enabledOut,
                                       String& errorMessage) {
    errorMessage = "";
    addressOut = "";
    enabledOut = false;

    if (!server.hasArg("plain")) {
        errorMessage = "Missing request body";
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain").c_str());
    if (error || !doc["address"].is<const char*>() || !doc["enabled"].is<bool>()) {
        errorMessage = "Invalid payload";
        return false;
    }

    addressOut = doc["address"] | "";
    enabledOut = doc["enabled"].as<bool>();
    return true;
}

bool parseForgetAddressRequest(WebServer& server, String& addressOut, String& errorMessage) {
    errorMessage = "";
    addressOut = "";

    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, server.arg("plain").c_str()) == DeserializationError::Ok) {
            addressOut = doc["address"] | "";
        }
    }

    if (addressOut.length() == 0) {
        addressOut = server.arg("address");
    }
    if (addressOut.length() == 0) {
        errorMessage = "Missing address";
        return false;
    }

    return true;
}

}  // namespace ObdApiService

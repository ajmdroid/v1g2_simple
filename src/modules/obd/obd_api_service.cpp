#include "obd_api_service.h"

#include <ArduinoJson.h>
#include <climits>

#include "../../ble_client.h"
#include "../../obd_handler.h"
#include "../../settings.h"

namespace ObdApiService {

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

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
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

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
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

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

bool parseConnectRequest(WebServer& server, ConnectRequest& out, String& errorMessage) {
    errorMessage = "";

    ConnectRequest request;

    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));
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
        DeserializationError error = deserializeJson(doc, server.arg("plain"));
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
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error || !doc["address"].is<const char*>() || !doc["enabled"].is<bool>()) {
        errorMessage = "Invalid payload";
        return false;
    }

    addressOut = doc["address"].as<String>();
    enabledOut = doc["enabled"].as<bool>();
    return true;
}

bool parseForgetAddressRequest(WebServer& server, String& addressOut, String& errorMessage) {
    errorMessage = "";
    addressOut = "";

    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
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

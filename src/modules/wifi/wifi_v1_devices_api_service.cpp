#include "wifi_v1_devices_api_service.h"

#include <ArduinoJson.h>

#ifdef UNIT_TEST
#include <string>
#endif

namespace WifiV1DevicesApiService {

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

void handleApiDevicesList(WebServer& server, const Runtime& runtime) {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();

    std::vector<DeviceInfo> devices;
    if (runtime.listDevices) {
        devices = runtime.listDevices();
    }

    for (const auto& device : devices) {
        JsonObject obj = arr.add<JsonObject>();
        obj["address"] = device.address;
        obj["name"] = device.name;
        obj["defaultProfile"] = device.defaultProfile;
        obj["connected"] = device.connected;
    }

    doc["count"] = devices.size();
    sendJsonDocument(server, 200, doc);
}

void handleApiDeviceNameSave(WebServer& server,
                             const Runtime& runtime,
                             const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("address")) {
        server.send(400, "application/json", "{\"error\":\"Missing address\"}");
        return;
    }

    if (!runtime.setDeviceName) {
        server.send(500, "application/json", "{\"error\":\"Device store unavailable\"}");
        return;
    }

    String address = server.arg("address");
    String name = server.hasArg("name") ? server.arg("name") : "";

    if (!runtime.setDeviceName(address, name)) {
        server.send(400, "application/json", "{\"error\":\"Invalid address or write failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiDeviceProfileSave(WebServer& server,
                                const Runtime& runtime,
                                const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("address") || !server.hasArg("profile")) {
        server.send(400, "application/json", "{\"error\":\"Missing address or profile\"}");
        return;
    }

    if (!runtime.setDeviceDefaultProfile) {
        server.send(500, "application/json", "{\"error\":\"Device store unavailable\"}");
        return;
    }

    String address = server.arg("address");
    int profile = server.arg("profile").toInt();
    if (profile < 0 || profile > 3) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile\"}");
        return;
    }

    if (!runtime.setDeviceDefaultProfile(address, static_cast<uint8_t>(profile))) {
        server.send(400, "application/json", "{\"error\":\"Invalid address or write failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiDeviceDelete(WebServer& server,
                           const Runtime& runtime,
                           const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("address")) {
        server.send(400, "application/json", "{\"error\":\"Missing address\"}");
        return;
    }

    if (!runtime.deleteDevice) {
        server.send(500, "application/json", "{\"error\":\"Device store unavailable\"}");
        return;
    }

    String address = server.arg("address");
    if (!runtime.deleteDevice(address)) {
        server.send(400, "application/json", "{\"error\":\"Invalid address or write failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"success\":true}");
}

}  // namespace WifiV1DevicesApiService

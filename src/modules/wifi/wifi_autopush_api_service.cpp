#include "wifi_autopush_api_service.h"

#include <ArduinoJson.h>

#ifdef UNIT_TEST
#include <string>
#endif

namespace WifiAutoPushApiService {

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

void handleSlots(WebServer& server, const Runtime& runtime) {
    SlotsSnapshot snapshot;
    if (runtime.loadSlotsSnapshot) {
        runtime.loadSlotsSnapshot(snapshot);
    }

    JsonDocument doc;
    doc["enabled"] = snapshot.enabled;
    doc["activeSlot"] = snapshot.activeSlot;

    JsonArray slots = doc["slots"].to<JsonArray>();
    for (const SlotConfig& slot : snapshot.slots) {
        JsonObject obj = slots.add<JsonObject>();
        obj["name"] = slot.name;
        obj["profile"] = slot.profile;
        obj["mode"] = slot.mode;
        obj["color"] = slot.color;
        obj["volume"] = slot.volume;
        obj["muteVolume"] = slot.muteVolume;
        obj["darkMode"] = slot.darkMode;
        obj["muteToZero"] = slot.muteToZero;
        obj["alertPersist"] = slot.alertPersist;
        obj["priorityArrowOnly"] = slot.priorityArrowOnly;
    }

    sendJsonDocument(server, 200, doc);
}

void handleStatus(WebServer& server, const Runtime& runtime) {
    String json;
    if (runtime.loadPushStatusJson && runtime.loadPushStatusJson(json)) {
        server.send(200, "application/json", json);
        return;
    }
    server.send(500, "application/json", "{\"error\":\"Push status not available\"}");
}

}  // namespace WifiAutoPushApiService

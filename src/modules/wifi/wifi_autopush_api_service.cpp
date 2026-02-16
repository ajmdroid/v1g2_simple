#include "wifi_autopush_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>

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

void handleApiSlots(WebServer& server, const Runtime& runtime) {
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

void handleApiStatus(WebServer& server, const Runtime& runtime) {
    String json;
    if (runtime.loadPushStatusJson && runtime.loadPushStatusJson(json)) {
        server.send(200, "application/json", json);
        return;
    }
    server.send(500, "application/json", "{\"error\":\"Push status not available\"}");
}

void handleApiSlotSave(WebServer& server,
                       const Runtime& runtime,
                       const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("slot") || !server.hasArg("profile") || !server.hasArg("mode")) {
        server.send(400, "application/json", "{\"error\":\"Missing parameters\"}");
        return;
    }

    int slot = server.arg("slot").toInt();
    String profile = server.arg("profile");
    int mode = server.arg("mode").toInt();
    String name = server.hasArg("name") ? server.arg("name") : "";
    int color = server.hasArg("color") ? server.arg("color").toInt() : -1;
    int volume = server.hasArg("volume") ? server.arg("volume").toInt() : -1;
    int muteVol = server.hasArg("muteVol") ? server.arg("muteVol").toInt() : -1;
    bool hasDarkMode = server.hasArg("darkMode");
    bool darkMode = hasDarkMode ? (server.arg("darkMode") == "true") : false;
    bool hasMuteToZero = server.hasArg("muteToZero");
    bool muteToZero = hasMuteToZero ? (server.arg("muteToZero") == "true") : false;
    bool hasAlertPersist = server.hasArg("alertPersist");
    int alertPersist = hasAlertPersist ? server.arg("alertPersist").toInt() : -1;

    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }

    if (name.length() > 0 && runtime.setSlotName) {
        runtime.setSlotName(slot, name);
    }

    if (color >= 0 && runtime.setSlotColor) {
        runtime.setSlotColor(slot, static_cast<uint16_t>(color));
    }

    uint8_t existingVol = runtime.getSlotVolume ? runtime.getSlotVolume(slot) : 0;
    uint8_t existingMute = runtime.getSlotMuteVolume ? runtime.getSlotMuteVolume(slot) : 0;
    uint8_t vol = (volume >= 0) ? static_cast<uint8_t>(volume) : existingVol;
    uint8_t mute = (muteVol >= 0) ? static_cast<uint8_t>(muteVol) : existingMute;

    if (runtime.setSlotVolumes) {
        runtime.setSlotVolumes(slot, vol, mute);
    }

    if (hasDarkMode && runtime.setSlotDarkMode) {
        runtime.setSlotDarkMode(slot, darkMode);
    }
    if (hasMuteToZero && runtime.setSlotMuteToZero) {
        runtime.setSlotMuteToZero(slot, muteToZero);
    }

    if (hasAlertPersist && alertPersist >= 0 && runtime.setSlotAlertPersistSec) {
        int clamped = std::max(0, std::min(5, alertPersist));
        runtime.setSlotAlertPersistSec(slot, static_cast<uint8_t>(clamped));
    }

    if (server.hasArg("priorityArrowOnly") && runtime.setSlotPriorityArrowOnly) {
        bool prioArrow = server.arg("priorityArrowOnly") == "true";
        runtime.setSlotPriorityArrowOnly(slot, prioArrow);
    }

    if (runtime.setSlotProfileAndMode) {
        runtime.setSlotProfileAndMode(slot, profile, mode);
    }

    if (runtime.getActiveSlot && runtime.drawProfileIndicator &&
        slot == runtime.getActiveSlot()) {
        runtime.drawProfileIndicator(slot);
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiActivate(WebServer& server,
                       const Runtime& runtime,
                       const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("slot")) {
        server.send(400, "application/json", "{\"error\":\"Missing slot parameter\"}");
        return;
    }

    int slot = server.arg("slot").toInt();
    bool enable = server.hasArg("enable") ? (server.arg("enable") == "true") : true;

    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }

    if (runtime.setActiveSlot) {
        runtime.setActiveSlot(slot);
    }
    if (runtime.setAutoPushEnabled) {
        runtime.setAutoPushEnabled(enable);
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiPushNow(WebServer& server,
                      const Runtime& runtime,
                      const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("slot")) {
        server.send(400, "application/json", "{\"error\":\"Missing slot parameter\"}");
        return;
    }

    int slot = server.arg("slot").toInt();
    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }

    if (!runtime.queuePushNow) {
        server.send(500, "application/json", "{\"error\":\"Failed to load profile\"}");
        return;
    }

    PushNowRequest request;
    request.slot = slot;
    if (server.hasArg("profile") && server.arg("profile").length() > 0) {
        request.hasProfileOverride = true;
        request.profileName = server.arg("profile");
        if (server.hasArg("mode")) {
            request.hasModeOverride = true;
            request.mode = server.arg("mode").toInt();
        }
    }

    switch (runtime.queuePushNow(request)) {
        case PushNowQueueResult::QUEUED:
            server.send(200, "application/json", "{\"success\":true,\"queued\":true}");
            return;
        case PushNowQueueResult::V1_NOT_CONNECTED:
            server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
            return;
        case PushNowQueueResult::ALREADY_IN_PROGRESS:
            server.send(409, "application/json", "{\"error\":\"Push already in progress\"}");
            return;
        case PushNowQueueResult::NO_PROFILE_CONFIGURED:
            server.send(400, "application/json", "{\"error\":\"No profile configured for this slot\"}");
            return;
        case PushNowQueueResult::PROFILE_LOAD_FAILED:
        default:
            server.send(500, "application/json", "{\"error\":\"Failed to load profile\"}");
            return;
    }
}

}  // namespace WifiAutoPushApiService

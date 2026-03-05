#include "camera_alert_api_service.h"

#include <ArduinoJson.h>
#include <string>

namespace CameraAlertApiService {

namespace {

void sendJson(WebServer& server, int statusCode, const JsonDocument& doc) {
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

bool parseBoolPatch(const JsonObjectConst& body, const char* key, bool& out) {
    if (body[key].isNull()) return false;
    if (!body[key].is<bool>()) return false;
    out = body[key].as<bool>();
    return true;
}

bool parseUInt16Patch(const JsonObjectConst& body, const char* key, uint16_t& out) {
    if (body[key].isNull()) return false;
    if (!body[key].is<int>()) return false;
    const int raw = body[key].as<int>();
    if (raw < 0 || raw > 65535) return false;
    out = static_cast<uint16_t>(raw);
    return true;
}

void writeSettingsJson(JsonDocument& doc, const V1Settings& settings, uint32_t cameraCount) {
    doc["cameraAlertsEnabled"] = settings.cameraAlertsEnabled;
    doc["cameraAlertRangeM"] = settings.cameraAlertRangeM;
    doc["cameraTypeAlpr"] = settings.cameraTypeAlpr;
    doc["cameraTypeRedLight"] = settings.cameraTypeRedLight;
    doc["cameraTypeSpeed"] = settings.cameraTypeSpeed;
    doc["cameraTypeBusLane"] = settings.cameraTypeBusLane;
    doc["colorCameraArrow"] = settings.colorCameraArrow;
    doc["colorCameraText"] = settings.colorCameraText;
    doc["cameraVoiceEnabled"] = settings.cameraVoiceEnabled;
    doc["cameraVoiceClose"] = settings.cameraVoiceClose;
    doc["cameraCount"] = cameraCount;
}

}  // namespace

void handleApiSettingsGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    const V1Settings& settings = runtime.getSettings();
    const uint32_t cameraCount = runtime.getCameraCount ? runtime.getCameraCount() : 0;

    JsonDocument doc;
    writeSettingsJson(doc, settings, cameraCount);
    sendJson(server, 200, doc);
}

void handleApiSettingsSave(WebServer& server,
                           const Runtime& runtime,
                           const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!runtime.getMutableSettings || !runtime.save) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    if (!server.hasArg("plain") || server.arg("plain").length() == 0) {
        server.send(400, "application/json", "{\"error\":\"JSON body required\"}");
        return;
    }

    const String rawBody = server.arg("plain");
    JsonDocument body;
    const DeserializationError parseError = deserializeJson(body, rawBody.c_str());
    if (parseError || !body.is<JsonObjectConst>()) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON body\"}");
        return;
    }

    V1Settings& settings = runtime.getMutableSettings();
    const JsonObjectConst json = body.as<JsonObjectConst>();

    bool boolVal = false;
    uint16_t u16Val = 0;

    if (!json["cameraAlertsEnabled"].isNull()) {
        if (!parseBoolPatch(json, "cameraAlertsEnabled", boolVal)) {
            server.send(400, "application/json", "{\"error\":\"cameraAlertsEnabled must be boolean\"}");
            return;
        }
        settings.cameraAlertsEnabled = boolVal;
    }

    if (!json["cameraAlertRangeM"].isNull()) {
        if (!parseUInt16Patch(json, "cameraAlertRangeM", u16Val)) {
            server.send(400, "application/json", "{\"error\":\"cameraAlertRangeM must be uint16\"}");
            return;
        }
        settings.cameraAlertRangeM = clampCameraAlertRangeMValue(static_cast<int>(u16Val));
    }

    if (!json["cameraTypeAlpr"].isNull()) {
        if (!parseBoolPatch(json, "cameraTypeAlpr", boolVal)) {
            server.send(400, "application/json", "{\"error\":\"cameraTypeAlpr must be boolean\"}");
            return;
        }
        settings.cameraTypeAlpr = boolVal;
    }
    if (!json["cameraTypeRedLight"].isNull()) {
        if (!parseBoolPatch(json, "cameraTypeRedLight", boolVal)) {
            server.send(400, "application/json", "{\"error\":\"cameraTypeRedLight must be boolean\"}");
            return;
        }
        settings.cameraTypeRedLight = boolVal;
    }
    if (!json["cameraTypeSpeed"].isNull()) {
        if (!parseBoolPatch(json, "cameraTypeSpeed", boolVal)) {
            server.send(400, "application/json", "{\"error\":\"cameraTypeSpeed must be boolean\"}");
            return;
        }
        settings.cameraTypeSpeed = boolVal;
    }
    if (!json["cameraTypeBusLane"].isNull()) {
        if (!parseBoolPatch(json, "cameraTypeBusLane", boolVal)) {
            server.send(400, "application/json", "{\"error\":\"cameraTypeBusLane must be boolean\"}");
            return;
        }
        settings.cameraTypeBusLane = boolVal;
    }

    if (!json["colorCameraArrow"].isNull()) {
        if (!parseUInt16Patch(json, "colorCameraArrow", u16Val)) {
            server.send(400, "application/json", "{\"error\":\"colorCameraArrow must be uint16\"}");
            return;
        }
        settings.colorCameraArrow = u16Val;
    }
    if (!json["colorCameraText"].isNull()) {
        if (!parseUInt16Patch(json, "colorCameraText", u16Val)) {
            server.send(400, "application/json", "{\"error\":\"colorCameraText must be uint16\"}");
            return;
        }
        settings.colorCameraText = u16Val;
    }

    if (!json["cameraVoiceEnabled"].isNull()) {
        if (!parseBoolPatch(json, "cameraVoiceEnabled", boolVal)) {
            server.send(400, "application/json", "{\"error\":\"cameraVoiceEnabled must be boolean\"}");
            return;
        }
        settings.cameraVoiceEnabled = boolVal;
    }
    if (!json["cameraVoiceClose"].isNull()) {
        if (!parseBoolPatch(json, "cameraVoiceClose", boolVal)) {
            server.send(400, "application/json", "{\"error\":\"cameraVoiceClose must be boolean\"}");
            return;
        }
        settings.cameraVoiceClose = boolVal;
    }

    runtime.save();

    JsonDocument doc;
    const uint32_t cameraCount = runtime.getCameraCount ? runtime.getCameraCount() : 0;
    writeSettingsJson(doc, settings, cameraCount);
    doc["success"] = true;
    sendJson(server, 200, doc);
}

void handleApiStatusGet(WebServer& server, const Runtime& runtime) {
    JsonDocument doc;
    doc["active"] = false;
    doc["cameraCount"] = runtime.getCameraCount ? runtime.getCameraCount() : 0;

    CameraAlertStatusSnapshot snapshot;
    const bool hasStatus = runtime.getCameraStatus && runtime.getCameraStatus(snapshot);
    if (hasStatus) {
        doc["active"] = snapshot.displayActive;
        doc["encounterActive"] = snapshot.encounterActive;
        doc["pendingFar"] = snapshot.pendingFar;
        doc["pendingNear"] = snapshot.pendingNear;
        doc["farAnnounced"] = snapshot.farAnnounced;
        doc["nearAnnounced"] = snapshot.nearAnnounced;
        if (snapshot.hasPayload) {
            doc["distanceCm"] = snapshot.distanceCm;
            doc["distanceMiles"] = static_cast<float>(snapshot.distanceCm) * 0.000006213712f;
            doc["type"] = snapshot.typeName;
            doc["flags"] = snapshot.flags;
        } else {
            doc["distanceCm"] = nullptr;
            doc["distanceMiles"] = nullptr;
            doc["type"] = nullptr;
            doc["flags"] = nullptr;
        }
    }

    sendJson(server, 200, doc);
}

}  // namespace CameraAlertApiService

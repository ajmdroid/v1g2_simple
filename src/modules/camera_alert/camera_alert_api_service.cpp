#include "camera_alert_api_service.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>

#ifdef UNIT_TEST
#include <string>
#include "../../../test/mocks/settings.h"
#else
#include "../../settings.h"
#endif

#include "../../../include/camera_alert_types.h"
#include "../lockout/road_map_reader.h"
#include "camera_alert_module.h"

namespace CameraAlertApiService {

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

void sendError(WebServer& server, int statusCode, const String& message) {
    JsonDocument doc;
    doc["error"] = message;
    sendJsonDocument(server, statusCode, doc);
}

bool parseBoolToken(String token, bool& outValue) {
    std::string normalized = token.c_str();
    const auto first = std::find_if_not(
        normalized.begin(),
        normalized.end(),
        [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(
        normalized.rbegin(),
        normalized.rend(),
        [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (first >= last) {
        return false;
    }
    normalized = std::string(first, last);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (normalized == "true" || normalized == "1") {
        outValue = true;
        return true;
    }
    if (normalized == "false" || normalized == "0") {
        outValue = false;
        return true;
    }
    return false;
}

bool parseUint32Token(const String& token, uint32_t& outValue) {
    if (token.length() == 0) {
        return false;
    }

    uint32_t value = 0;
    const char* chars = token.c_str();
    for (size_t i = 0; i < token.length(); ++i) {
        const char ch = chars[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        const uint32_t nextValue = (value * 10U) + static_cast<uint32_t>(ch - '0');
        if (nextValue < value) {
            return false;
        }
        value = nextValue;
    }

    outValue = value;
    return true;
}

bool parseBoolArg(WebServer& server,
                  const char* key,
                  bool& target,
                  String& invalidField,
                  bool& sawKnownArg) {
    if (!server.hasArg(key)) {
        return true;
    }

    sawKnownArg = true;
    bool parsed = false;
    if (!parseBoolToken(server.arg(key), parsed)) {
        invalidField = key;
        return false;
    }

    target = parsed;
    return true;
}

bool parseClampedUint32Arg(WebServer& server,
                           const char* key,
                           uint32_t minValue,
                           uint32_t maxValue,
                           uint32_t& target,
                           String& invalidField,
                           bool& sawKnownArg) {
    if (!server.hasArg(key)) {
        return true;
    }

    sawKnownArg = true;
    uint32_t parsed = 0;
    if (!parseUint32Token(server.arg(key), parsed)) {
        invalidField = key;
        return false;
    }

    target = std::max(minValue, std::min(parsed, maxValue));
    return true;
}

bool parseClampedUint16Arg(WebServer& server,
                           const char* key,
                           uint16_t& target,
                           String& invalidField,
                           bool& sawKnownArg) {
    uint32_t parsed = target;
    if (!parseClampedUint32Arg(server, key, 0, 65535, parsed, invalidField, sawKnownArg)) {
        return false;
    }
    target = static_cast<uint16_t>(parsed);
    return true;
}

void sendSettings(WebServer& server, const V1Settings& settings) {
    JsonDocument doc;
    doc["cameraAlertsEnabled"] = settings.cameraAlertsEnabled;
    doc["cameraAlertRangeCm"] = settings.cameraAlertRangeCm;
    doc["cameraAlertNearRangeCm"] = settings.cameraAlertNearRangeCm;
    doc["cameraTypeAlpr"] = settings.cameraTypeAlpr;
    doc["cameraTypeRedLight"] = settings.cameraTypeRedLight;
    doc["cameraTypeSpeed"] = settings.cameraTypeSpeed;
    doc["cameraTypeBusLane"] = settings.cameraTypeBusLane;
    doc["colorCameraArrow"] = settings.colorCameraArrow;
    doc["colorCameraText"] = settings.colorCameraText;
    doc["cameraVoiceFarEnabled"] = settings.cameraVoiceFarEnabled;
    doc["cameraVoiceNearEnabled"] = settings.cameraVoiceNearEnabled;
    sendJsonDocument(server, 200, doc);
}

void sendStatus(WebServer& server,
                const CameraAlertModule& cameraAlertModule,
                const RoadMapReader& roadMapReader) {
    const CameraAlertDisplayPayload& payload = cameraAlertModule.displayPayload();

    JsonDocument doc;
    doc["cameraCount"] = roadMapReader.cameraCount();
    doc["displayActive"] = cameraAlertModule.isDisplayActive();

    const char* apiName = (payload.active ? cameraTypeApiName(payload.type) : nullptr);
    if (apiName) {
        doc["type"] = apiName;
    } else {
        doc["type"] = nullptr;
    }

    if (payload.active && payload.distanceCm != CAMERA_DISTANCE_INVALID_CM) {
        doc["distanceCm"] = payload.distanceCm;
    } else {
        doc["distanceCm"] = nullptr;
    }

    sendJsonDocument(server, 200, doc);
}

}  // namespace

void handleApiSettingsGet(WebServer& server,
                          SettingsManager& settingsManager,
                          const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }

    sendSettings(server, settingsManager.get());
}

void handleApiSettingsPost(WebServer& server,
                           SettingsManager& settingsManager,
                           const std::function<bool()>& checkRateLimit,
                           const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) {
        return;
    }
    if (markUiActivity) {
        markUiActivity();
    }

    V1Settings updated = settingsManager.get();
    bool sawKnownArg = false;
    String invalidField;

    if (!parseBoolArg(server, "cameraAlertsEnabled", updated.cameraAlertsEnabled, invalidField, sawKnownArg) ||
        !parseClampedUint32Arg(server,
                               "cameraAlertRangeCm",
                               CAMERA_ALERT_RANGE_CM_MIN,
                               CAMERA_ALERT_RANGE_CM_MAX,
                               updated.cameraAlertRangeCm,
                               invalidField,
                               sawKnownArg) ||
        !parseClampedUint32Arg(server,
                               "cameraAlertNearRangeCm",
                               CAMERA_ALERT_NEAR_RANGE_CM_MIN,
                               CAMERA_ALERT_NEAR_RANGE_CM_MAX,
                               updated.cameraAlertNearRangeCm,
                               invalidField,
                               sawKnownArg) ||
        !parseBoolArg(server, "cameraTypeAlpr", updated.cameraTypeAlpr, invalidField, sawKnownArg) ||
        !parseBoolArg(server, "cameraTypeRedLight", updated.cameraTypeRedLight, invalidField, sawKnownArg) ||
        !parseBoolArg(server, "cameraTypeSpeed", updated.cameraTypeSpeed, invalidField, sawKnownArg) ||
        !parseBoolArg(server, "cameraTypeBusLane", updated.cameraTypeBusLane, invalidField, sawKnownArg) ||
        !parseClampedUint16Arg(server, "colorCameraArrow", updated.colorCameraArrow, invalidField, sawKnownArg) ||
        !parseClampedUint16Arg(server, "colorCameraText", updated.colorCameraText, invalidField, sawKnownArg) ||
        !parseBoolArg(server, "cameraVoiceFarEnabled", updated.cameraVoiceFarEnabled, invalidField, sawKnownArg) ||
        !parseBoolArg(server, "cameraVoiceNearEnabled", updated.cameraVoiceNearEnabled, invalidField, sawKnownArg)) {
        sendError(server, 400, String("invalid ") + invalidField);
        return;
    }

    normalizeCameraAlertRanges(updated.cameraAlertRangeCm, updated.cameraAlertNearRangeCm);

    if (sawKnownArg) {
        settingsManager.mutableSettings() = updated;
        settingsManager.save();
    }

    JsonDocument doc;
    doc["success"] = true;
    sendJsonDocument(server, 200, doc);
}

void handleApiStatus(WebServer& server,
                     CameraAlertModule& cameraAlertModule,
                     RoadMapReader& roadMapReader,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }

    sendStatus(server, cameraAlertModule, roadMapReader);
}

}  // namespace CameraAlertApiService

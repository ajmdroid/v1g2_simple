/**
 * WiFi Time Sync API service implementation.
 */

#include "wifi_time_api_service.h"
#include "wifi_api_response.h"

#include <ArduinoJson.h>

namespace WifiTimeApiService {

namespace {

// Mirrors the validation bounds in time_service.cpp.
constexpr int64_t MIN_VALID_UNIX_MS = 1700000000000LL;  // ~2023-11
constexpr int64_t MAX_VALID_UNIX_MS = 4102444800000LL;  // 2100-01-01

bool isValidEpochMs(int64_t epochMs) {
    return epochMs >= MIN_VALID_UNIX_MS && epochMs <= MAX_VALID_UNIX_MS;
}

int32_t clampTzOffset(int32_t tzOffsetMin) {
    if (tzOffsetMin < -840) return -840;
    if (tzOffsetMin > 840) return 840;
    return tzOffsetMin;
}

}  // namespace

void handleApiTimeSync(WebServer& server,
                       bool (*setTime)(int64_t epochMs, int32_t tzOffsetMinutes, void* ctx),
                       void* setTimeCtx) {
    if (!server.hasArg("plain") || server.arg("plain").length() == 0) {
        JsonDocument doc;
        doc["ok"] = false;
        WifiApiResponse::setErrorAndMessage(doc, "Missing JSON body");
        WifiApiResponse::sendJsonDocument(server, 400, doc);
        return;
    }

    JsonDocument body;
    const String plainBody = server.arg("plain");
    const DeserializationError err = deserializeJson(body, plainBody);
    if (err) {
        JsonDocument doc;
        doc["ok"] = false;
        WifiApiResponse::setErrorAndMessage(doc, "Invalid JSON");
        WifiApiResponse::sendJsonDocument(server, 400, doc);
        return;
    }

    // epochMs is required — must be a numeric type (int64 or double from JSON)
    const JsonVariant epochVar = body["epochMs"];
    if (epochVar.isNull() || epochVar.is<bool>() || epochVar.is<const char*>()) {
        JsonDocument doc;
        doc["ok"] = false;
        WifiApiResponse::setErrorAndMessage(doc, "Missing or invalid epochMs");
        WifiApiResponse::sendJsonDocument(server, 400, doc);
        return;
    }
    const int64_t epochMs = epochVar.as<long long>();
    if (!isValidEpochMs(epochMs)) {
        JsonDocument doc;
        doc["ok"] = false;
        WifiApiResponse::setErrorAndMessage(doc, "epochMs out of valid range");
        WifiApiResponse::sendJsonDocument(server, 400, doc);
        return;
    }

    // tzOffsetMinutes is optional, defaults to 0 when absent.
    int32_t tzOffsetMinutes = 0;
    if (body["tzOffsetMinutes"].is<int>()) {
        tzOffsetMinutes = clampTzOffset(body["tzOffsetMinutes"].as<int>());
    }

    if (setTime) {
        setTime(epochMs, tzOffsetMinutes, setTimeCtx);
    }

    JsonDocument response;
    response["ok"] = true;
    response["source"] = 1;  // TimeService::SOURCE_CLIENT_AP
    response["epochMs"] = epochMs;
    WifiApiResponse::sendJsonDocument(server, 200, response);
}

}  // namespace WifiTimeApiService

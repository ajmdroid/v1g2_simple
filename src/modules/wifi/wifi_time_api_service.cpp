#include "wifi_time_api_service.h"

#include <ArduinoJson.h>

#include <cstdlib>

#ifdef UNIT_TEST
#include <string>
#endif

namespace WifiTimeApiService {

namespace {

bool parseUint64Strict(const String& input, uint64_t& out) {
    if (input.length() == 0) {
        return false;
    }
    char* end = nullptr;
    const char* raw = input.c_str();
    unsigned long long v = strtoull(raw, &end, 10);
    if (end == raw || *end != '\0') {
        return false;
    }
    out = static_cast<uint64_t>(v);
    return true;
}

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

static void handleTimeSetImpl(WebServer& server,
                              const TimeRuntime& runtime,
                              uint8_t clientSource,
                              const std::function<void()>& invalidateStatusCache) {
    uint64_t unixMs = 0;
    int32_t tzOffsetMin = 0;
    bool haveUnixMs = false;
    bool sourceIsClient = true;

    // Preferred JSON: {"unixMs": <ms>, "tzOffsetMin": <minutes>, "source":"client"}
    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain").c_str());
        if (!err) {
            if (doc["source"].is<const char*>()) {
                String source = String(doc["source"].as<const char*>());
                source.toLowerCase();
                sourceIsClient = (source.length() == 0 || source == "client");
            }

            if (doc["unixMs"].is<const char*>()) {
                haveUnixMs = parseUint64Strict(String(doc["unixMs"].as<const char*>()), unixMs);
            } else if (doc["unixMs"].is<uint64_t>()) {
                unixMs = doc["unixMs"].as<uint64_t>();
                haveUnixMs = true;
            } else if (doc["epochMs"].is<const char*>()) {
                // Compatibility key
                haveUnixMs = parseUint64Strict(String(doc["epochMs"].as<const char*>()), unixMs);
            } else if (doc["epochMs"].is<uint64_t>()) {
                unixMs = doc["epochMs"].as<uint64_t>();
                haveUnixMs = true;
            } else if (doc["clientEpochMs"].is<const char*>()) {
                // Compatibility key
                haveUnixMs = parseUint64Strict(String(doc["clientEpochMs"].as<const char*>()), unixMs);
            } else if (doc["clientEpochMs"].is<uint64_t>()) {
                unixMs = doc["clientEpochMs"].as<uint64_t>();
                haveUnixMs = true;
            }

            if (doc["tzOffsetMin"].is<int32_t>()) {
                tzOffsetMin = doc["tzOffsetMin"].as<int32_t>();
            } else if (doc["tzOffsetMinutes"].is<int32_t>()) {
                // Compatibility key
                tzOffsetMin = doc["tzOffsetMinutes"].as<int32_t>();
            }
        }
    }

    // Compatibility fallback: form/query args
    if (!haveUnixMs) {
        if (server.hasArg("unixMs")) {
            haveUnixMs = parseUint64Strict(server.arg("unixMs"), unixMs);
        } else if (server.hasArg("epochMs")) {
            haveUnixMs = parseUint64Strict(server.arg("epochMs"), unixMs);
        } else if (server.hasArg("clientEpochMs")) {
            haveUnixMs = parseUint64Strict(server.arg("clientEpochMs"), unixMs);
        }
    }
    if (server.hasArg("tzOffsetMin")) {
        tzOffsetMin = server.arg("tzOffsetMin").toInt();
    } else if (server.hasArg("tzOffsetMinutes")) {
        tzOffsetMin = server.arg("tzOffsetMinutes").toInt();
    }
    if (server.hasArg("source")) {
        String source = server.arg("source");
        source.toLowerCase();
        sourceIsClient = (source.length() == 0 || source == "client");
    }

    if (!sourceIsClient) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Unsupported source\"}");
        return;
    }

    if (!haveUnixMs) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing or invalid unixMs\"}");
        return;
    }

    // Sanity range: >= ~2023-11 and <= 2100
    static constexpr uint64_t MIN_VALID_UNIX_MS = 1700000000000ULL;
    static constexpr uint64_t MAX_VALID_UNIX_MS = 4102444800000ULL;
    if (unixMs < MIN_VALID_UNIX_MS || unixMs > MAX_VALID_UNIX_MS) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"unixMs out of range\"}");
        return;
    }

    if (tzOffsetMin < -840) tzOffsetMin = -840;
    if (tzOffsetMin > 840) tzOffsetMin = 840;

    const int64_t requestedEpochMs = static_cast<int64_t>(unixMs);
    const bool hasExistingTime = runtime.timeValid();
    const int64_t existingEpochMs = hasExistingTime ? runtime.nowEpochMsOr0() : 0;
    const int32_t existingTzOffsetMin = runtime.tzOffsetMinutes();
    const uint8_t existingSource = runtime.timeSource();
    static constexpr int64_t TIME_SET_NOOP_DELTA_MS = 2000LL;
    const bool nearNoopClientSync = hasExistingTime
        && existingSource == clientSource
        && existingTzOffsetMin == tzOffsetMin
        && llabs(requestedEpochMs - existingEpochMs) <= TIME_SET_NOOP_DELTA_MS;

    if (!nearNoopClientSync) {
        runtime.setEpochBaseMs(
            requestedEpochMs,
            tzOffsetMin,
            clientSource);
        if (invalidateStatusCache) {
            invalidateStatusCache();
        }
    }

    JsonDocument response;
    response["ok"] = true;
    response["timeValid"] = runtime.timeValid();
    response["timeSource"] = runtime.timeSource();
    response["timeConfidence"] = runtime.timeConfidence();
    response["epochMs"] = runtime.nowEpochMsOr0();
    response["tzOffsetMin"] = runtime.tzOffsetMinutes();

    // Backward-compatible fields
    response["success"] = true;
    response["monoMs"] = runtime.nowMonoMs();
    response["epochAgeMs"] = runtime.epochAgeMsOr0();
    response["tzOffsetMinutes"] = runtime.tzOffsetMinutes();

    sendJsonDocument(server, 200, response);
}

void handleApiTimeSet(WebServer& server,
                      const TimeRuntime& runtime,
                      uint8_t clientSource,
                      const std::function<void()>& invalidateStatusCache,
                      const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleTimeSetImpl(server, runtime, clientSource, invalidateStatusCache);
}

}  // namespace WifiTimeApiService

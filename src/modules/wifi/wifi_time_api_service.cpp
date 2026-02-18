#include "wifi_time_api_service.h"

#include <ArduinoJson.h>

#ifdef UNIT_TEST
#include <string>
#endif

namespace WifiTimeApiService {

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

void sendTimeSetDisabled(WebServer& server, const TimeRuntime& runtime) {
    JsonDocument response;
    response["ok"] = false;
    response["success"] = false;
    response["error"] = "Time set disabled; GPS is authoritative";
    response["timeValid"] = runtime.timeValid();
    response["timeSource"] = runtime.timeSource();
    response["timeConfidence"] = runtime.timeConfidence();
    response["epochMs"] = runtime.nowEpochMsOr0();
    response["tzOffsetMin"] = runtime.tzOffsetMinutes();
    response["monoMs"] = runtime.nowMonoMs();
    response["epochAgeMs"] = runtime.epochAgeMsOr0();
    response["tzOffsetMinutes"] = runtime.tzOffsetMinutes();
    sendJsonDocument(server, 409, response);
}

}  // namespace

void handleApiTimeSet(WebServer& server,
                      const TimeRuntime& runtime,
                      uint8_t clientSource,
                      const std::function<void()>& invalidateStatusCache,
                      const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    (void)clientSource;
    (void)invalidateStatusCache;
    sendTimeSetDisabled(server, runtime);
}

}  // namespace WifiTimeApiService

#include "wifi_time_api_service.h"
#include "wifi_api_response.h"

#include <ArduinoJson.h>

namespace WifiTimeApiService {

namespace {

void sendTimeSetDisabled(WebServer& server, const TimeRuntime& runtime) {
    JsonDocument response;
    response["ok"] = false;
    response["success"] = false;
    WifiApiResponse::setErrorAndMessage(response, "Time set disabled; GPS is authoritative");
    response["timeValid"] = runtime.timeValid();
    response["timeSource"] = runtime.timeSource();
    response["timeConfidence"] = runtime.timeConfidence();
    response["epochMs"] = runtime.nowEpochMsOr0();
    response["tzOffsetMin"] = runtime.tzOffsetMinutes();
    response["monoMs"] = runtime.nowMonoMs();
    response["epochAgeMs"] = runtime.epochAgeMsOr0();
    response["tzOffsetMinutes"] = runtime.tzOffsetMinutes();
    WifiApiResponse::sendJsonDocument(server, 409, response);
}

}  // namespace

void handleApiTimeSet(WebServer& server,
                      const TimeRuntime& runtime,
                      uint8_t clientSource,
                      const std::function<void()>& invalidateStatusCache,
                      const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    (void)clientSource;
    // When time-set is enabled, call invalidateStatusCache() after updating time state.
    sendTimeSetDisabled(server, runtime);
}

}  // namespace WifiTimeApiService

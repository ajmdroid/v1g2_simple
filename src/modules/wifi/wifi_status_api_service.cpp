#include "wifi_status_api_service.h"

#include <ArduinoJson.h>

#ifdef UNIT_TEST
#include <string>
#endif

namespace WifiStatusApiService {

namespace {

template <typename T>
T callOr(const std::function<T()>& fn, const T& fallback) {
    return fn ? fn() : fallback;
}

}  // namespace

void sendStatus(WebServer& server,
                const StatusRuntime& runtime,
                String& cachedStatusJson,
                unsigned long& lastStatusJsonTime,
                unsigned long cacheTtlMs,
                const std::function<unsigned long()>& millisFn) {
    const unsigned long now = millisFn ? millisFn() : millis();
    const bool cacheValid = (now - lastStatusJsonTime) < cacheTtlMs;

    if (!cacheValid) {
        JsonDocument doc;

        const bool setupModeActive = callOr<bool>(runtime.setupModeActive, false);
        const bool staConnected = callOr<bool>(runtime.staConnected, false);

        JsonObject wifi = doc["wifi"].to<JsonObject>();
        wifi["setup_mode"] = setupModeActive;
        wifi["ap_active"] = setupModeActive;
        wifi["sta_connected"] = staConnected;
        wifi["sta_ip"] = staConnected ? callOr<String>(runtime.staIp, String("")) : "";
        wifi["ap_ip"] = callOr<String>(runtime.apIp, String(""));
        wifi["ssid"] = staConnected
            ? callOr<String>(runtime.connectedSsid, String(""))
            : callOr<String>(runtime.apSsid, String(""));
        wifi["rssi"] = staConnected ? callOr<int32_t>(runtime.rssi, 0) : 0;
        wifi["sta_enabled"] = callOr<bool>(runtime.staEnabled, false);
        wifi["sta_ssid"] = callOr<String>(runtime.staSavedSsid, String(""));

        JsonObject device = doc["device"].to<JsonObject>();
        device["uptime"] = callOr<unsigned long>(runtime.uptimeSeconds, 0UL);
        device["heap_free"] = callOr<uint32_t>(runtime.heapFree, 0U);
        device["hostname"] = callOr<String>(runtime.hostname, String("v1g2"));
        device["firmware_version"] = callOr<String>(runtime.firmwareVersion, String(""));

        JsonObject time = doc["time"].to<JsonObject>();
        const bool timeValid = callOr<bool>(runtime.timeValid, false);
        time["valid"] = timeValid;
        time["source"] = callOr<uint8_t>(runtime.timeSource, 0);
        time["confidence"] = callOr<uint8_t>(runtime.timeConfidence, 0);
        const int32_t tzOffsetMin = callOr<int32_t>(runtime.timeTzOffsetMin, 0);
        time["tzOffsetMin"] = tzOffsetMin;
        time["tzOffsetMinutes"] = tzOffsetMin;
        if (timeValid) {
            time["epochMs"] = callOr<int64_t>(runtime.timeEpochMsOr0, 0);
            time["ageMs"] = callOr<uint32_t>(runtime.timeEpochAgeMsOr0, 0U);
        }

        JsonObject battery = doc["battery"].to<JsonObject>();
        battery["voltage_mv"] = callOr<uint16_t>(runtime.batteryVoltageMv, 0);
        battery["percentage"] = callOr<uint8_t>(runtime.batteryPercentage, 0);
        battery["on_battery"] = callOr<bool>(runtime.batteryOnBattery, false);
        battery["has_battery"] = callOr<bool>(runtime.batteryHasBattery, false);

        doc["v1_connected"] = callOr<bool>(runtime.v1Connected, false);

        if (runtime.getStatusJson) {
            JsonDocument statusDoc;
            String statusJson = runtime.getStatusJson();
            deserializeJson(statusDoc, statusJson.c_str());
            for (JsonPair kv : statusDoc.as<JsonObject>()) {
                doc[kv.key()] = kv.value();
            }
        }

        if (runtime.getAlertJson) {
            JsonDocument alertDoc;
            String alertJson = runtime.getAlertJson();
            deserializeJson(alertDoc, alertJson.c_str());
            doc["alert"] = alertDoc;
        }

#ifdef UNIT_TEST
        std::string response;
        serializeJson(doc, response);
        cachedStatusJson = response.c_str();
#else
        cachedStatusJson = "";
        serializeJson(doc, cachedStatusJson);
#endif
        lastStatusJsonTime = now;
    }

    server.send(200, "application/json", cachedStatusJson);
}

}  // namespace WifiStatusApiService

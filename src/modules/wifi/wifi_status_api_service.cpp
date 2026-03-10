#include "wifi_status_api_service.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "json_stream_response.h"

namespace WifiStatusApiService {

namespace {

constexpr size_t STATUS_CACHE_GROWTH_QUANTUM = 256u;

template <typename T>
T callOr(const std::function<T()>& fn, const T& fallback) {
    return fn ? fn() : fallback;
}

void buildStatusDoc(const StatusRuntime& runtime, JsonDocument& doc) {
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

    if (runtime.mergeStatus) {
        runtime.mergeStatus(doc.as<JsonObject>());
    }

    if (runtime.mergeAlert) {
        JsonObject alert = doc["alert"].to<JsonObject>();
        runtime.mergeAlert(alert);
    }
}

size_t roundUpStatusCacheCapacity(size_t required) {
    return ((required + STATUS_CACHE_GROWTH_QUANTUM - 1u) / STATUS_CACHE_GROWTH_QUANTUM) *
           STATUS_CACHE_GROWTH_QUANTUM;
}

bool ensureStatusCacheCapacity(StatusJsonCache& cachedStatusJson, size_t required) {
    if (cachedStatusJson.data != nullptr && cachedStatusJson.capacity >= required) {
        return true;
    }

    const size_t newCapacity = roundUpStatusCacheCapacity(required);
    const size_t previousCapacity = cachedStatusJson.capacity;
    char* newData = static_cast<char*>(
        heap_caps_malloc(newCapacity, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    bool inPsram = true;

    if (newData == nullptr) {
        Serial.printf(
            "[WiFiStatus] Cache PSRAM alloc failed; falling back to internal (%lu bytes)\n",
            static_cast<unsigned long>(newCapacity));
        newData = static_cast<char*>(
            heap_caps_malloc(newCapacity, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        inPsram = false;
    }

    if (newData == nullptr) {
        Serial.printf("[WiFiStatus] Cache alloc failed (%lu bytes); sending uncached response\n",
                      static_cast<unsigned long>(newCapacity));
        return false;
    }

    if (cachedStatusJson.data != nullptr) {
        heap_caps_free(cachedStatusJson.data);
    }

    cachedStatusJson.data = newData;
    cachedStatusJson.capacity = newCapacity;
    cachedStatusJson.length = 0;
    cachedStatusJson.inPsram = inPsram;

    Serial.printf("[WiFiStatus] Cache grow %lu -> %lu bytes (%s)\n",
                  static_cast<unsigned long>(previousCapacity),
                  static_cast<unsigned long>(newCapacity),
                  inPsram ? "psram" : "internal");
    return true;
}

void sendStatus(WebServer& server,
                const StatusRuntime& runtime,
                StatusJsonCache& cachedStatusJson,
                unsigned long& lastStatusJsonTime,
                unsigned long cacheTtlMs,
                const std::function<unsigned long()>& millisFn) {
    const unsigned long now = millisFn ? millisFn() : millis();
    const bool cacheValid = cachedStatusJson.data != nullptr &&
                            cachedStatusJson.length > 0 &&
                            (now - lastStatusJsonTime) < cacheTtlMs;

    if (!cacheValid) {
        JsonDocument doc;
        buildStatusDoc(runtime, doc);

        const size_t required = measureJson(doc) + 1u;
        if (!ensureStatusCacheCapacity(cachedStatusJson, required)) {
            sendJsonStream(server, doc);
            return;
        }

        cachedStatusJson.length = serializeJson(doc, cachedStatusJson.data, cachedStatusJson.capacity);
        cachedStatusJson.data[cachedStatusJson.length] = '\0';
        lastStatusJsonTime = now;
    }

    sendSerializedJson(server, cachedStatusJson.data, cachedStatusJson.length);
}

}  // namespace

void invalidateStatusJsonCache(StatusJsonCache& cachedStatusJson,
                               unsigned long& lastStatusJsonTime) {
    cachedStatusJson.length = 0;
    lastStatusJsonTime = 0;
}

void releaseStatusJsonCache(StatusJsonCache& cachedStatusJson,
                            unsigned long& lastStatusJsonTime) {
    if (cachedStatusJson.data != nullptr) {
        heap_caps_free(cachedStatusJson.data);
    }
    cachedStatusJson.data = nullptr;
    cachedStatusJson.capacity = 0;
    cachedStatusJson.length = 0;
    cachedStatusJson.inPsram = false;
    lastStatusJsonTime = 0;
}

void handleApiStatus(WebServer& server,
                     const StatusRuntime& runtime,
                     StatusJsonCache& cachedStatusJson,
                     unsigned long& lastStatusJsonTime,
                     unsigned long cacheTtlMs,
                     const std::function<unsigned long()>& millisFn,
                     const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) {
        return;
    }

    sendStatus(
        server,
        runtime,
        cachedStatusJson,
        lastStatusJsonTime,
        cacheTtlMs,
        millisFn);
}

void handleApiLegacyStatus(WebServer& server,
                           const StatusRuntime& runtime,
                           StatusJsonCache& cachedStatusJson,
                           unsigned long& lastStatusJsonTime,
                           unsigned long cacheTtlMs,
                           const std::function<unsigned long()>& millisFn) {
    sendStatus(
        server,
        runtime,
        cachedStatusJson,
        lastStatusJsonTime,
        cacheTtlMs,
        millisFn);
}

}  // namespace WifiStatusApiService

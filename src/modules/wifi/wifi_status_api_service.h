#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstdint>

namespace WifiStatusApiService {

struct StatusJsonCache {
    char* data = nullptr;
    size_t capacity = 0;
    size_t length = 0;
    bool inPsram = false;
};

struct StatusRuntime {
    bool (*setupModeActive)(void* ctx) = nullptr;
    void* setupModeActiveCtx = nullptr;
    bool (*staConnected)(void* ctx) = nullptr;
    void* staConnectedCtx = nullptr;
    String (*staIp)(void* ctx) = nullptr;
    void* staIpCtx = nullptr;
    String (*apIp)(void* ctx) = nullptr;
    void* apIpCtx = nullptr;
    String (*connectedSsid)(void* ctx) = nullptr;
    void* connectedSsidCtx = nullptr;
    int32_t (*rssi)(void* ctx) = nullptr;
    void* rssiCtx = nullptr;
    bool (*staEnabled)(void* ctx) = nullptr;
    void* staEnabledCtx = nullptr;
    String (*staSavedSsid)(void* ctx) = nullptr;
    void* staSavedSsidCtx = nullptr;
    String (*apSsid)(void* ctx) = nullptr;
    void* apSsidCtx = nullptr;

    unsigned long (*uptimeSeconds)(void* ctx) = nullptr;
    void* uptimeSecondsCtx = nullptr;
    uint32_t (*heapFree)(void* ctx) = nullptr;
    void* heapFreeCtx = nullptr;
    String (*hostname)(void* ctx) = nullptr;
    void* hostnameCtx = nullptr;
    String (*firmwareVersion)(void* ctx) = nullptr;
    void* firmwareVersionCtx = nullptr;

    bool (*timeValid)(void* ctx) = nullptr;
    void* timeValidCtx = nullptr;
    uint8_t (*timeSource)(void* ctx) = nullptr;
    void* timeSourceCtx = nullptr;
    uint8_t (*timeConfidence)(void* ctx) = nullptr;
    void* timeConfidenceCtx = nullptr;
    int32_t (*timeTzOffsetMin)(void* ctx) = nullptr;
    void* timeTzOffsetMinCtx = nullptr;
    int64_t (*timeEpochMsOr0)(void* ctx) = nullptr;
    void* timeEpochMsOr0Ctx = nullptr;
    uint32_t (*timeEpochAgeMsOr0)(void* ctx) = nullptr;
    void* timeEpochAgeMsOr0Ctx = nullptr;

    uint16_t (*batteryVoltageMv)(void* ctx) = nullptr;
    void* batteryVoltageMvCtx = nullptr;
    uint8_t (*batteryPercentage)(void* ctx) = nullptr;
    void* batteryPercentageCtx = nullptr;
    bool (*batteryOnBattery)(void* ctx) = nullptr;
    void* batteryOnBatteryCtx = nullptr;
    bool (*batteryHasBattery)(void* ctx) = nullptr;
    void* batteryHasBatteryCtx = nullptr;

    bool (*v1Connected)(void* ctx) = nullptr;
    void* v1ConnectedCtx = nullptr;
    void (*mergeStatus)(JsonObject, void* ctx) = nullptr;   // Write fields directly into root doc
    void* mergeStatusCtx = nullptr;
    void (*mergeStatus2)(JsonObject, void* ctx) = nullptr;  // optional second contributor
    void* mergeStatus2Ctx = nullptr;
    void (*mergeAlert)(JsonObject, void* ctx) = nullptr;    // Write fields into alert sub-object
    void* mergeAlertCtx = nullptr;
};

void invalidateStatusJsonCache(StatusJsonCache& cachedStatusJson,
                               unsigned long& lastStatusJsonTime);

void releaseStatusJsonCache(StatusJsonCache& cachedStatusJson,
                            unsigned long& lastStatusJsonTime);

void handleApiStatus(WebServer& server,
                     const StatusRuntime& runtime,
                     StatusJsonCache& cachedStatusJson,
                     unsigned long& lastStatusJsonTime,
                     unsigned long cacheTtlMs,
                     unsigned long (*millisFn)(void* ctx), void* millisCtx,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiLegacyStatus(WebServer& server,
                           const StatusRuntime& runtime,
                           StatusJsonCache& cachedStatusJson,
                           unsigned long& lastStatusJsonTime,
                           unsigned long cacheTtlMs,
                           unsigned long (*millisFn)(void* ctx), void* millisCtx);

}  // namespace WifiStatusApiService

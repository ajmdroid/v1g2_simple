"""
Phase 3.10: Migrate WifiStatusApiService + WiFiManager stored callbacks
to fn-ptr + void* ctx pattern.
"""
import re
import sys

def replace_exactly_once(src, old, new, label):
    count = src.count(old)
    if count == 0:
        print(f'  NOT FOUND: {label}')
        return src
    if count > 1:
        print(f'  MULTIPLE MATCHES ({count}): {label}')
        return src
    print(f'  Replaced: {label}')
    return src.replace(old, new, 1)

# ============================================================
# 1. wifi_status_api_service.h
# ============================================================
path = 'src/modules/wifi/wifi_status_api_service.h'
src = open(path).read()

# Remove <functional> include
src = replace_exactly_once(src,
    '#include <functional>\n',
    '',
    'remove <functional>')

# Replace entire StatusRuntime struct
old_struct = '''struct StatusRuntime {
    std::function<bool()> setupModeActive;
    std::function<bool()> staConnected;
    std::function<String()> staIp;
    std::function<String()> apIp;
    std::function<String()> connectedSsid;
    std::function<int32_t()> rssi;
    std::function<bool()> staEnabled;
    std::function<String()> staSavedSsid;
    std::function<String()> apSsid;

    std::function<unsigned long()> uptimeSeconds;
    std::function<uint32_t()> heapFree;
    std::function<String()> hostname;
    std::function<String()> firmwareVersion;

    std::function<bool()> timeValid;
    std::function<uint8_t()> timeSource;
    std::function<uint8_t()> timeConfidence;
    std::function<int32_t()> timeTzOffsetMin;
    std::function<int64_t()> timeEpochMsOr0;
    std::function<uint32_t()> timeEpochAgeMsOr0;

    std::function<uint16_t()> batteryVoltageMv;
    std::function<uint8_t()> batteryPercentage;
    std::function<bool()> batteryOnBattery;
    std::function<bool()> batteryHasBattery;

    std::function<bool()> v1Connected;
    std::function<void(JsonObject)> mergeStatus;   // Write fields directly into root doc
    std::function<void(JsonObject)> mergeAlert;    // Write fields into alert sub-object
};'''

new_struct = '''struct StatusRuntime {
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
};'''

src = replace_exactly_once(src, old_struct, new_struct, 'StatusRuntime struct')

# Replace handleApiStatus signature
old_sig = '''void handleApiStatus(WebServer& server,
                     const StatusRuntime& runtime,
                     StatusJsonCache& cachedStatusJson,
                     unsigned long& lastStatusJsonTime,
                     unsigned long cacheTtlMs,
                     const std::function<unsigned long()>& millisFn,
                     const std::function<bool()>& checkRateLimit);'''

new_sig = '''void handleApiStatus(WebServer& server,
                     const StatusRuntime& runtime,
                     StatusJsonCache& cachedStatusJson,
                     unsigned long& lastStatusJsonTime,
                     unsigned long cacheTtlMs,
                     unsigned long (*millisFn)(void* ctx), void* millisCtx,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);'''

src = replace_exactly_once(src, old_sig, new_sig, 'handleApiStatus signature')

# Replace handleApiLegacyStatus signature
old_legacy = '''void handleApiLegacyStatus(WebServer& server,
                           const StatusRuntime& runtime,
                           StatusJsonCache& cachedStatusJson,
                           unsigned long& lastStatusJsonTime,
                           unsigned long cacheTtlMs,
                           const std::function<unsigned long()>& millisFn);'''

new_legacy = '''void handleApiLegacyStatus(WebServer& server,
                           const StatusRuntime& runtime,
                           StatusJsonCache& cachedStatusJson,
                           unsigned long& lastStatusJsonTime,
                           unsigned long cacheTtlMs,
                           unsigned long (*millisFn)(void* ctx), void* millisCtx);'''

src = replace_exactly_once(src, old_legacy, new_legacy, 'handleApiLegacyStatus signature')

open(path, 'w').write(src)
remaining = src.count('std::function')
print(f'header std::function remaining: {remaining}')

# ============================================================
# 2. wifi_status_api_service.cpp
# ============================================================
path = 'src/modules/wifi/wifi_status_api_service.cpp'
src = open(path).read()

# Replace callOr helper
old_callor = '''template <typename T>
T callOr(const std::function<T()>& fn, const T& fallback) {
    return fn ? fn() : fallback;
}'''
new_callor = '''template <typename T>
T callOr(T (*fn)(void* ctx), void* ctx, const T& fallback) {
    return fn ? fn(ctx) : fallback;
}'''
src = replace_exactly_once(src, old_callor, new_callor, 'callOr template')

# Replace buildStatusDoc (entire function body)
old_build = '''void buildStatusDoc(const StatusRuntime& runtime, JsonDocument& doc) {
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
}'''

new_build = '''void buildStatusDoc(const StatusRuntime& runtime, JsonDocument& doc) {
    const bool setupModeActive = callOr<bool>(runtime.setupModeActive, runtime.setupModeActiveCtx, false);
    const bool staConnected = callOr<bool>(runtime.staConnected, runtime.staConnectedCtx, false);

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["setup_mode"] = setupModeActive;
    wifi["ap_active"] = setupModeActive;
    wifi["sta_connected"] = staConnected;
    wifi["sta_ip"] = staConnected ? callOr<String>(runtime.staIp, runtime.staIpCtx, String("")) : "";
    wifi["ap_ip"] = callOr<String>(runtime.apIp, runtime.apIpCtx, String(""));
    wifi["ssid"] = staConnected
        ? callOr<String>(runtime.connectedSsid, runtime.connectedSsidCtx, String(""))
        : callOr<String>(runtime.apSsid, runtime.apSsidCtx, String(""));
    wifi["rssi"] = staConnected ? callOr<int32_t>(runtime.rssi, runtime.rssiCtx, 0) : 0;
    wifi["sta_enabled"] = callOr<bool>(runtime.staEnabled, runtime.staEnabledCtx, false);
    wifi["sta_ssid"] = callOr<String>(runtime.staSavedSsid, runtime.staSavedSsidCtx, String(""));

    JsonObject device = doc["device"].to<JsonObject>();
    device["uptime"] = callOr<unsigned long>(runtime.uptimeSeconds, runtime.uptimeSecondsCtx, 0UL);
    device["heap_free"] = callOr<uint32_t>(runtime.heapFree, runtime.heapFreeCtx, 0U);
    device["hostname"] = callOr<String>(runtime.hostname, runtime.hostnameCtx, String("v1g2"));
    device["firmware_version"] = callOr<String>(runtime.firmwareVersion, runtime.firmwareVersionCtx, String(""));

    JsonObject time = doc["time"].to<JsonObject>();
    const bool timeValid = callOr<bool>(runtime.timeValid, runtime.timeValidCtx, false);
    time["valid"] = timeValid;
    time["source"] = callOr<uint8_t>(runtime.timeSource, runtime.timeSourceCtx, 0);
    time["confidence"] = callOr<uint8_t>(runtime.timeConfidence, runtime.timeConfidenceCtx, 0);
    const int32_t tzOffsetMin = callOr<int32_t>(runtime.timeTzOffsetMin, runtime.timeTzOffsetMinCtx, 0);
    time["tzOffsetMin"] = tzOffsetMin;
    time["tzOffsetMinutes"] = tzOffsetMin;
    if (timeValid) {
        time["epochMs"] = callOr<int64_t>(runtime.timeEpochMsOr0, runtime.timeEpochMsOr0Ctx, 0);
        time["ageMs"] = callOr<uint32_t>(runtime.timeEpochAgeMsOr0, runtime.timeEpochAgeMsOr0Ctx, 0U);
    }

    JsonObject battery = doc["battery"].to<JsonObject>();
    battery["voltage_mv"] = callOr<uint16_t>(runtime.batteryVoltageMv, runtime.batteryVoltageMvCtx, 0);
    battery["percentage"] = callOr<uint8_t>(runtime.batteryPercentage, runtime.batteryPercentageCtx, 0);
    battery["on_battery"] = callOr<bool>(runtime.batteryOnBattery, runtime.batteryOnBatteryCtx, false);
    battery["has_battery"] = callOr<bool>(runtime.batteryHasBattery, runtime.batteryHasBatteryCtx, false);

    doc["v1_connected"] = callOr<bool>(runtime.v1Connected, runtime.v1ConnectedCtx, false);

    if (runtime.mergeStatus) {
        runtime.mergeStatus(doc.as<JsonObject>(), runtime.mergeStatusCtx);
    }
    if (runtime.mergeStatus2) {
        runtime.mergeStatus2(doc.as<JsonObject>(), runtime.mergeStatus2Ctx);
    }
    if (runtime.mergeAlert) {
        JsonObject alert = doc["alert"].to<JsonObject>();
        runtime.mergeAlert(alert, runtime.mergeAlertCtx);
    }
}'''

src = replace_exactly_once(src, old_build, new_build, 'buildStatusDoc')

# Replace sendStatus signature + body opening
old_send_sig = '''void sendStatus(WebServer& server,
                const StatusRuntime& runtime,
                StatusJsonCache& cachedStatusJson,
                unsigned long& lastStatusJsonTime,
                unsigned long cacheTtlMs,
                const std::function<unsigned long()>& millisFn) {
    const unsigned long now = millisFn ? millisFn() : millis();'''

new_send_sig = '''void sendStatus(WebServer& server,
                const StatusRuntime& runtime,
                StatusJsonCache& cachedStatusJson,
                unsigned long& lastStatusJsonTime,
                unsigned long cacheTtlMs,
                unsigned long (*millisFn)(void* ctx), void* millisCtx) {
    const unsigned long now = millisFn ? millisFn(millisCtx) : millis();'''

src = replace_exactly_once(src, old_send_sig, new_send_sig, 'sendStatus signature')

# Replace sendStatus tail call inside sendStatus (passes millisFn)
# The sendStatus function calls itself recursively? No, it calls sendStatus(server, runtime, ..., millisFn)
# Actually sendStatus doesn't call itself - let me check.
# It calls sendStatus in handleApiStatus and handleApiLegacyStatus.
# The body of sendStatus just uses millisFn directly. Nothing to change inside.

# Replace handleApiStatus signature + body
old_handle = '''void handleApiStatus(WebServer& server,
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
}'''

new_handle = '''void handleApiStatus(WebServer& server,
                     const StatusRuntime& runtime,
                     StatusJsonCache& cachedStatusJson,
                     unsigned long& lastStatusJsonTime,
                     unsigned long cacheTtlMs,
                     unsigned long (*millisFn)(void* ctx), void* millisCtx,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) {
        return;
    }

    sendStatus(
        server,
        runtime,
        cachedStatusJson,
        lastStatusJsonTime,
        cacheTtlMs,
        millisFn,
        millisCtx);
}'''

src = replace_exactly_once(src, old_handle, new_handle, 'handleApiStatus impl')

# Replace handleApiLegacyStatus
old_legacy = '''void handleApiLegacyStatus(WebServer& server,
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
}'''

new_legacy = '''void handleApiLegacyStatus(WebServer& server,
                           const StatusRuntime& runtime,
                           StatusJsonCache& cachedStatusJson,
                           unsigned long& lastStatusJsonTime,
                           unsigned long cacheTtlMs,
                           unsigned long (*millisFn)(void* ctx), void* millisCtx) {
    sendStatus(
        server,
        runtime,
        cachedStatusJson,
        lastStatusJsonTime,
        cacheTtlMs,
        millisFn,
        millisCtx);
}'''

src = replace_exactly_once(src, old_legacy, new_legacy, 'handleApiLegacyStatus impl')

open(path, 'w').write(src)
remaining = src.count('std::function')
print(f'cpp std::function remaining: {remaining}')

# ============================================================
# 3. wifi_manager.h - stored callbacks + setters
# ============================================================
path = 'src/wifi_manager.h'
src = open(path).read()

# Remove <functional>
src = replace_exactly_once(src, '#include <functional>\n', '', 'remove <functional>')

# Replace setter methods - setAlertCallback
src = replace_exactly_once(src,
    '    void setAlertCallback(std::function<void(JsonObject)> callback) { mergeAlert = callback; }',
    '    void setAlertCallback(void (*fn)(JsonObject, void*), void* ctx) { mergeAlert = fn; mergeAlertCtx = ctx; }',
    'setAlertCallback')

# setStatusCallback
src = replace_exactly_once(src,
    '    void setStatusCallback(std::function<void(JsonObject)> callback) { mergeStatus = callback; }',
    '    void setStatusCallback(void (*fn)(JsonObject, void*), void* ctx) { mergeStatus = fn; mergeStatusCtx = ctx; }',
    'setStatusCallback')

# appendStatusCallback - replace entire method
old_append = '''    void appendStatusCallback(std::function<void(JsonObject)> callback) {
        if (!callback) {
            return;
        }
        if (!mergeStatus) {
            mergeStatus = std::move(callback);
            return;
        }

        auto previous = std::move(mergeStatus);
        mergeStatus = [previous = std::move(previous), callback = std::move(callback)](JsonObject obj) {
            previous(obj);
            callback(obj);
        };
    }'''

new_append = '''    void appendStatusCallback(void (*fn)(JsonObject, void*), void* ctx) {
        mergeStatus2 = fn;
        mergeStatus2Ctx = ctx;
    }'''

src = replace_exactly_once(src, old_append, new_append, 'appendStatusCallback')

# setCommandCallback
src = replace_exactly_once(src,
    '    void setCommandCallback(std::function<bool(const char*, bool)> callback) { sendV1Command = callback; }',
    '    void setCommandCallback(bool (*fn)(const char*, bool, void*), void* ctx) { sendV1Command = fn; sendV1CommandCtx = ctx; }',
    'setCommandCallback')

# setProfilePushCallback
src = replace_exactly_once(src,
    '    void setProfilePushCallback(std::function<WifiControlApiService::ProfilePushResult()> callback) { requestProfilePush = callback; }',
    '    void setProfilePushCallback(WifiControlApiService::ProfilePushResult (*fn)(void*), void* ctx) { requestProfilePush = fn; requestProfilePushCtx = ctx; }',
    'setProfilePushCallback')

# setFilesystemCallback
src = replace_exactly_once(src,
    '    void setFilesystemCallback(std::function<fs::FS*()> callback) { getFilesystem = callback; }',
    '    void setFilesystemCallback(fs::FS* (*fn)(void*), void* ctx) { getFilesystem = fn; getFilesystemCtx = ctx; }',
    'setFilesystemCallback')

# setPushStatusCallback
src = replace_exactly_once(src,
    '    void setPushStatusCallback(std::function<String()> callback) { getPushStatusJson = callback; }',
    '    void setPushStatusCallback(String (*fn)(void*), void* ctx) { getPushStatusJson = fn; getPushStatusJsonCtx = ctx; }',
    'setPushStatusCallback')

# setPushNowCallback - multi-line
old_pushnow = '''    void setPushNowCallback(
        std::function<WifiAutoPushApiService::PushNowQueueResult(
            const WifiAutoPushApiService::PushNowRequest&)> callback) {
        queuePushNow = callback;
    }'''
new_pushnow = '''    void setPushNowCallback(
        WifiAutoPushApiService::PushNowQueueResult (*fn)(
            const WifiAutoPushApiService::PushNowRequest&, void*),
        void* ctx) {
        queuePushNow = fn;
        queuePushNowCtx = ctx;
    }'''
src = replace_exactly_once(src, old_pushnow, new_pushnow, 'setPushNowCallback')

# setV1ConnectedCallback
src = replace_exactly_once(src,
    '    void setV1ConnectedCallback(std::function<bool()> callback) { isV1Connected = callback; }',
    '    void setV1ConnectedCallback(bool (*fn)(void*), void* ctx) { isV1Connected = fn; isV1ConnectedCtx = ctx; }',
    'setV1ConnectedCallback')

# Replace private stored fields
old_fields = '''    std::function<void(JsonObject)> mergeAlert;
    std::function<void(JsonObject)> mergeStatus;
    std::function<bool(const char*, bool)> sendV1Command;
    std::function<WifiControlApiService::ProfilePushResult()> requestProfilePush;
    std::function<fs::FS*()> getFilesystem;
    std::function<String()> getPushStatusJson;
    std::function<WifiAutoPushApiService::PushNowQueueResult(
        const WifiAutoPushApiService::PushNowRequest&)> queuePushNow;
    std::function<bool()> isV1Connected;  // Returns true when V1 is connected (defer WiFi ops until then)'''

new_fields = '''    void (*mergeAlert)(JsonObject, void* ctx) = nullptr;
    void* mergeAlertCtx = nullptr;
    void (*mergeStatus)(JsonObject, void* ctx) = nullptr;
    void* mergeStatusCtx = nullptr;
    void (*mergeStatus2)(JsonObject, void* ctx) = nullptr;   // appended by appendStatusCallback
    void* mergeStatus2Ctx = nullptr;
    bool (*sendV1Command)(const char*, bool, void* ctx) = nullptr;
    void* sendV1CommandCtx = nullptr;
    WifiControlApiService::ProfilePushResult (*requestProfilePush)(void* ctx) = nullptr;
    void* requestProfilePushCtx = nullptr;
    fs::FS* (*getFilesystem)(void* ctx) = nullptr;
    void* getFilesystemCtx = nullptr;
    String (*getPushStatusJson)(void* ctx) = nullptr;
    void* getPushStatusJsonCtx = nullptr;
    WifiAutoPushApiService::PushNowQueueResult (*queuePushNow)(
        const WifiAutoPushApiService::PushNowRequest&, void* ctx) = nullptr;
    void* queuePushNowCtx = nullptr;
    bool (*isV1Connected)(void* ctx) = nullptr;   // Returns true when V1 is connected (defer WiFi ops until then)
    void* isV1ConnectedCtx = nullptr;'''

src = replace_exactly_once(src, old_fields, new_fields, 'private stored fields')

open(path, 'w').write(src)
remaining = src.count('std::function')
print(f'wifi_manager.h std::function remaining: {remaining}')

# ============================================================
# 4. wifi_orchestrator_module.cpp - all setter calls
# ============================================================
path = 'src/modules/wifi/wifi_orchestrator_module.cpp'
src = open(path).read()

# setStatusCallback
old_status_cb = '''    wifiManager.setStatusCallback([this](JsonObject obj) {
        obj["v1_connected"] = bleClient.isConnected();
    });'''
new_status_cb = '''    wifiManager.setStatusCallback(
        [](JsonObject obj, void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            obj["v1_connected"] = self->bleClient.isConnected();
        }, this);'''
src = replace_exactly_once(src, old_status_cb, new_status_cb, 'setStatusCallback call')

# setAlertCallback
old_alert_cb = '''    wifiManager.setAlertCallback([this](JsonObject obj) {
        if (parser.hasAlerts()) {
            AlertData alert = parser.getPriorityAlert();
            obj["active"] = true;
            const char* bandStr = "None";
            if (alert.band == BAND_KA) bandStr = "Ka";
            else if (alert.band == BAND_K) bandStr = "K";
            else if (alert.band == BAND_X) bandStr = "X";
            else if (alert.band == BAND_LASER) bandStr = "LASER";
            obj["band"] = bandStr;
            obj["strength"] = alert.frontStrength;
            obj["frequency"] = alert.frequency;
            obj["direction"] = alert.direction;
        } else {
            obj["active"] = false;
        }
    });'''
new_alert_cb = '''    wifiManager.setAlertCallback(
        [](JsonObject obj, void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            if (self->parser.hasAlerts()) {
                AlertData alert = self->parser.getPriorityAlert();
                obj["active"] = true;
                const char* bandStr = "None";
                if (alert.band == BAND_KA) bandStr = "Ka";
                else if (alert.band == BAND_K) bandStr = "K";
                else if (alert.band == BAND_X) bandStr = "X";
                else if (alert.band == BAND_LASER) bandStr = "LASER";
                obj["band"] = bandStr;
                obj["strength"] = alert.frontStrength;
                obj["frequency"] = alert.frequency;
                obj["direction"] = alert.direction;
            } else {
                obj["active"] = false;
            }
        }, this);'''
src = replace_exactly_once(src, old_alert_cb, new_alert_cb, 'setAlertCallback call')

# setCommandCallback
old_cmd_cb = '''    wifiManager.setCommandCallback([this](const char* cmd, bool state) {
        if (strcmp(cmd, "display") == 0) {
            return bleClient.setDisplayOn(state);
        } else if (strcmp(cmd, "mute") == 0) {
            return quietCoordinator.sendMute(QuietOwner::WifiCommand, state);
        }
        return false;
    });'''
new_cmd_cb = '''    wifiManager.setCommandCallback(
        [](const char* cmd, bool state, void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            if (strcmp(cmd, "display") == 0) {
                return self->bleClient.setDisplayOn(state);
            } else if (strcmp(cmd, "mute") == 0) {
                return self->quietCoordinator.sendMute(QuietOwner::WifiCommand, state);
            }
            return false;
        }, this);'''
src = replace_exactly_once(src, old_cmd_cb, new_cmd_cb, 'setCommandCallback call')

# setFilesystemCallback
old_fs_cb = '''    wifiManager.setFilesystemCallback([this]() -> fs::FS* {
        return storageManager.isReady() ? storageManager.getFilesystem() : nullptr;
    });'''
new_fs_cb = '''    wifiManager.setFilesystemCallback(
        [](void* ctx) -> fs::FS* {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->storageManager.isReady() ? self->storageManager.getFilesystem() : nullptr;
        }, this);'''
src = replace_exactly_once(src, old_fs_cb, new_fs_cb, 'setFilesystemCallback call')

# setProfilePushCallback
old_push_cb = '''    wifiManager.setProfilePushCallback([this]() {
        const V1Settings& s = settingsManager.get();
        switch (autoPushModule.queueSlotPush(s.activeSlot)) {
            case AutoPushModule::QueueResult::QUEUED:
                return WifiControlApiService::ProfilePushResult::QUEUED;
            case AutoPushModule::QueueResult::ALREADY_IN_PROGRESS:
                return WifiControlApiService::ProfilePushResult::ALREADY_IN_PROGRESS;
            case AutoPushModule::QueueResult::V1_NOT_CONNECTED:
            case AutoPushModule::QueueResult::NO_PROFILE_CONFIGURED:
            case AutoPushModule::QueueResult::PROFILE_LOAD_FAILED:
            default:
                return WifiControlApiService::ProfilePushResult::HANDLER_UNAVAILABLE;
        }
    });'''
new_push_cb = '''    wifiManager.setProfilePushCallback(
        [](void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            const V1Settings& s = self->settingsManager.get();
            switch (self->autoPushModule.queueSlotPush(s.activeSlot)) {
                case AutoPushModule::QueueResult::QUEUED:
                    return WifiControlApiService::ProfilePushResult::QUEUED;
                case AutoPushModule::QueueResult::ALREADY_IN_PROGRESS:
                    return WifiControlApiService::ProfilePushResult::ALREADY_IN_PROGRESS;
                case AutoPushModule::QueueResult::V1_NOT_CONNECTED:
                case AutoPushModule::QueueResult::NO_PROFILE_CONFIGURED:
                case AutoPushModule::QueueResult::PROFILE_LOAD_FAILED:
                default:
                    return WifiControlApiService::ProfilePushResult::HANDLER_UNAVAILABLE;
            }
        }, this);'''
src = replace_exactly_once(src, old_push_cb, new_push_cb, 'setProfilePushCallback call')

# setPushStatusCallback
old_psc = '''    wifiManager.setPushStatusCallback([this]() {
        return autoPushModule.getStatusJson();
    });'''
new_psc = '''    wifiManager.setPushStatusCallback(
        [](void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->autoPushModule.getStatusJson();
        }, this);'''
src = replace_exactly_once(src, old_psc, new_psc, 'setPushStatusCallback call')

# setPushNowCallback
old_pnc = '''    wifiManager.setPushNowCallback([this](const WifiAutoPushApiService::PushNowRequest& request) {
        AutoPushModule::PushNowRequest pushRequest;
        pushRequest.slotIndex = request.slot;
        pushRequest.activateSlot = true;
        pushRequest.hasProfileOverride = request.hasProfileOverride;
        pushRequest.hasModeOverride = request.hasModeOverride;

        if (request.hasProfileOverride) {
            pushRequest.profileName = sanitizeProfileNameValue(request.profileName);
        }
        if (request.hasModeOverride) {
            pushRequest.mode = normalizeV1ModeValue(request.mode);
        }

        switch (autoPushModule.queuePushNow(pushRequest)) {
            case AutoPushModule::QueueResult::QUEUED:
                return WifiAutoPushApiService::PushNowQueueResult::QUEUED;
            case AutoPushModule::QueueResult::V1_NOT_CONNECTED:
                return WifiAutoPushApiService::PushNowQueueResult::V1_NOT_CONNECTED;
            case AutoPushModule::QueueResult::ALREADY_IN_PROGRESS:
                return WifiAutoPushApiService::PushNowQueueResult::ALREADY_IN_PROGRESS;
            case AutoPushModule::QueueResult::NO_PROFILE_CONFIGURED:
                return WifiAutoPushApiService::PushNowQueueResult::NO_PROFILE_CONFIGURED;
            case AutoPushModule::QueueResult::PROFILE_LOAD_FAILED:
            default:
                return WifiAutoPushApiService::PushNowQueueResult::PROFILE_LOAD_FAILED;
        }
    });'''
new_pnc = '''    wifiManager.setPushNowCallback(
        [](const WifiAutoPushApiService::PushNowRequest& request, void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            AutoPushModule::PushNowRequest pushRequest;
            pushRequest.slotIndex = request.slot;
            pushRequest.activateSlot = true;
            pushRequest.hasProfileOverride = request.hasProfileOverride;
            pushRequest.hasModeOverride = request.hasModeOverride;

            if (request.hasProfileOverride) {
                pushRequest.profileName = sanitizeProfileNameValue(request.profileName);
            }
            if (request.hasModeOverride) {
                pushRequest.mode = normalizeV1ModeValue(request.mode);
            }

            switch (self->autoPushModule.queuePushNow(pushRequest)) {
                case AutoPushModule::QueueResult::QUEUED:
                    return WifiAutoPushApiService::PushNowQueueResult::QUEUED;
                case AutoPushModule::QueueResult::V1_NOT_CONNECTED:
                    return WifiAutoPushApiService::PushNowQueueResult::V1_NOT_CONNECTED;
                case AutoPushModule::QueueResult::ALREADY_IN_PROGRESS:
                    return WifiAutoPushApiService::PushNowQueueResult::ALREADY_IN_PROGRESS;
                case AutoPushModule::QueueResult::NO_PROFILE_CONFIGURED:
                    return WifiAutoPushApiService::PushNowQueueResult::NO_PROFILE_CONFIGURED;
                case AutoPushModule::QueueResult::PROFILE_LOAD_FAILED:
                default:
                    return WifiAutoPushApiService::PushNowQueueResult::PROFILE_LOAD_FAILED;
            }
        }, this);'''
src = replace_exactly_once(src, old_pnc, new_pnc, 'setPushNowCallback call')

# setV1ConnectedCallback
old_v1c = '''    wifiManager.setV1ConnectedCallback([this]() {
        return bleClient.isConnected();
    });'''
new_v1c = '''    wifiManager.setV1ConnectedCallback(
        [](void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->bleClient.isConnected();
        }, this);'''
src = replace_exactly_once(src, old_v1c, new_v1c, 'setV1ConnectedCallback call')

open(path, 'w').write(src)
remaining = src.count('std::function')
print(f'orchestrator std::function remaining: {remaining}')

# ============================================================
# 5. main.cpp - appendStatusCallback
# ============================================================
path = 'src/main.cpp'
src = open(path).read()

src = replace_exactly_once(src,
    '        wifiManager.appendStatusCallback([](JsonObject obj) {',
    '        wifiManager.appendStatusCallback([](JsonObject obj, void* /*ctx*/) {',
    'appendStatusCallback lambda param')

# close paren - needs to add nullptr
src = replace_exactly_once(src,
    '        });\n        wifiStatusObservabilityCallbackConfigured = true;',
    '        }, nullptr);\n        wifiStatusObservabilityCallbackConfigured = true;',
    'appendStatusCallback close + nullptr')

open(path, 'w').write(src)
remaining = src.count('appendStatusCallback')
print(f'main.cpp appendStatusCallback calls: {remaining}')

# ============================================================
# 6. wifi_client.cpp - isV1Connected() call
# ============================================================
path = 'src/wifi_client.cpp'
src = open(path).read()

src = replace_exactly_once(src,
    'bool v1Connected = isV1Connected ? isV1Connected() : bleClient.isConnected();',
    'bool v1Connected = isV1Connected ? isV1Connected(isV1ConnectedCtx) : bleClient.isConnected();',
    'isV1Connected call')

open(path, 'w').write(src)
print(f'wifi_client.cpp: updated isV1Connected call')

# ============================================================
# 7. wifi_runtimes.cpp - makeStatusRuntime + stored callback usages
# ============================================================
path = 'src/wifi_runtimes.cpp'
src = open(path).read()

# Replace makeStatusRuntime - entire function
old_make_status = '''WifiStatusApiService::StatusRuntime WiFiManager::makeStatusRuntime() {
    WifiStatusApiService::StatusRuntime runtime{
        [this]() { return isSetupModeActive(); },
        [this]() { return wifiClientState == WIFI_CLIENT_CONNECTED; },
        []() { return WiFi.localIP().toString(); },
        [this]() { return getAPIPAddress(); },
        []() { return WiFi.SSID(); },
        []() { return WiFi.RSSI(); },
        [this]() { return settingsManager.get().wifiClientEnabled; },
        [this]() { return settingsManager.get().wifiClientSSID; },
        [this]() { return settingsManager.get().apSSID; },
        []() { return millis() / 1000; },
        []() { return ESP.getFreeHeap(); },
        []() { return String("v1g2"); },
        []() { return String(FIRMWARE_VERSION); },
        [this]() { return timeService.timeValid(); },
        [this]() { return timeService.timeSource(); },
        [this]() { return timeService.timeConfidence(); },
        [this]() { return timeService.tzOffsetMinutes(); },
        [this]() { return timeService.nowEpochMsOr0(); },
        [this]() { return timeService.epochAgeMsOr0(); },
        [this]() { return batteryManager.getVoltageMillivolts(); },
        [this]() { return batteryManager.getPercentage(); },
        [this]() { return batteryManager.isOnBattery(); },
        [this]() { return batteryManager.hasBattery(); },
        [this]() { return bleClient.isConnected(); },
        mergeStatus,
        mergeAlert,
    };
    return runtime;
}'''

new_make_status = '''WifiStatusApiService::StatusRuntime WiFiManager::makeStatusRuntime() {
    return WifiStatusApiService::StatusRuntime{
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->isSetupModeActive(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->wifiClientState == WIFI_CLIENT_CONNECTED; }, this,
        [](void* /*ctx*/) { return WiFi.localIP().toString(); }, nullptr,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->getAPIPAddress(); }, this,
        [](void* /*ctx*/) { return WiFi.SSID(); }, nullptr,
        [](void* /*ctx*/) { return static_cast<int32_t>(WiFi.RSSI()); }, nullptr,
        [](void* /*ctx*/) { return settingsManager.get().wifiClientEnabled; }, nullptr,
        [](void* /*ctx*/) { return settingsManager.get().wifiClientSSID; }, nullptr,
        [](void* /*ctx*/) { return settingsManager.get().apSSID; }, nullptr,
        [](void* /*ctx*/) -> unsigned long { return millis() / 1000; }, nullptr,
        [](void* /*ctx*/) { return ESP.getFreeHeap(); }, nullptr,
        [](void* /*ctx*/) { return String("v1g2"); }, nullptr,
        [](void* /*ctx*/) { return String(FIRMWARE_VERSION); }, nullptr,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->timeService.timeValid(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->timeService.timeSource(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->timeService.timeConfidence(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->timeService.tzOffsetMinutes(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->timeService.nowEpochMsOr0(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->timeService.epochAgeMsOr0(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->batteryManager.getVoltageMillivolts(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->batteryManager.getPercentage(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->batteryManager.isOnBattery(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->batteryManager.hasBattery(); }, this,
        [](void* /*ctx*/) { return bleClient.isConnected(); }, nullptr,
        mergeStatus, mergeStatusCtx,
        mergeStatus2, mergeStatus2Ctx,
        mergeAlert, mergeAlertCtx,
    };
}'''

src = replace_exactly_once(src, old_make_status, new_make_status, 'makeStatusRuntime')

# Update getPushStatusJson call inside makeAutoPushRuntime
src = replace_exactly_once(src,
    '            json = mgr->getPushStatusJson();',
    '            json = mgr->getPushStatusJson(mgr->getPushStatusJsonCtx);',
    'getPushStatusJson call')

open(path, 'w').write(src)
remaining = src.count('std::function')
print(f'wifi_runtimes.cpp std::function remaining: {remaining}')

# ============================================================
# 8. wifi_routes.cpp - status route, darkmode/mute, requestProfilePush
# ============================================================
path = 'src/wifi_routes.cpp'
src = open(path).read()

# Remove rateLimitCallback and markUiActivityCallback, update /api/status route
old_status_route = '''    auto rateLimitCallback = [this]() { return checkRateLimit(); };
    auto markUiActivityCallback = [this]() { markUiActivity(); };
    // New API endpoints (PHASE A)
    server.on("/api/status", HTTP_GET, [this, rateLimitCallback]() {
        WifiStatusApiService::handleApiStatus(
            server,
            makeStatusRuntime(),
            cachedStatusJson,
            lastStatusJsonTime,
            STATUS_CACHE_TTL_MS,
            []() { return millis(); },
            rateLimitCallback);
    });'''

new_status_route = '''    // New API endpoints (PHASE A)
    server.on("/api/status", HTTP_GET, [this]() {
        WifiStatusApiService::handleApiStatus(
            server,
            makeStatusRuntime(),
            cachedStatusJson,
            lastStatusJsonTime,
            STATUS_CACHE_TTL_MS,
            [](void* /*ctx*/) -> unsigned long { return millis(); }, nullptr,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });'''

src = replace_exactly_once(src, old_status_route, new_status_route, 'status route + remove callbacks')

# Update legacy /status route
old_legacy_route = '''    server.on("/status", HTTP_GET, [this]() {
        WifiStatusApiService::handleApiLegacyStatus(
            server,
            makeStatusRuntime(),
            cachedStatusJson,
            lastStatusJsonTime,
            STATUS_CACHE_TTL_MS,
            []() { return millis(); });
    });'''

new_legacy_route = '''    server.on("/status", HTTP_GET, [this]() {
        WifiStatusApiService::handleApiLegacyStatus(
            server,
            makeStatusRuntime(),
            cachedStatusJson,
            lastStatusJsonTime,
            STATUS_CACHE_TTL_MS,
            [](void* /*ctx*/) -> unsigned long { return millis(); }, nullptr);
    });'''

src = replace_exactly_once(src, old_legacy_route, new_legacy_route, 'legacy status route')

# Update requestProfilePush call
old_rpush = '''            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->requestProfilePush(); },'''
new_rpush = '''            [](void* ctx) -> WifiControlApiService::ProfilePushResult {
                auto* self = static_cast<WiFiManager*>(ctx);
                return self->requestProfilePush ? self->requestProfilePush(self->requestProfilePushCtx) : WifiControlApiService::ProfilePushResult::HANDLER_UNAVAILABLE;
            },'''
src = replace_exactly_once(src, old_rpush, new_rpush, 'requestProfilePush lambda')

# Update sendV1Command calls (2 occurrences: darkmode + mute)
old_svc = '''            [](const char* cmd, bool val, void* ctx) {
                return static_cast<WiFiManager*>(ctx)->sendV1Command(cmd, val);
            },
            this,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    server.on("/mute", HTTP_POST, [this]() {
        WifiControlApiService::handleApiMute(
            server,
            [](const char* cmd, bool val, void* ctx) {
                return static_cast<WiFiManager*>(ctx)->sendV1Command(cmd, val);
            },
            this,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });'''

new_svc = '''            [](const char* cmd, bool val, void* ctx) {
                auto* self = static_cast<WiFiManager*>(ctx);
                return self->sendV1Command ? self->sendV1Command(cmd, val, self->sendV1CommandCtx) : false;
            },
            this,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    server.on("/mute", HTTP_POST, [this]() {
        WifiControlApiService::handleApiMute(
            server,
            [](const char* cmd, bool val, void* ctx) {
                auto* self = static_cast<WiFiManager*>(ctx);
                return self->sendV1Command ? self->sendV1Command(cmd, val, self->sendV1CommandCtx) : false;
            },
            this,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });'''

src = replace_exactly_once(src, old_svc, new_svc, 'sendV1Command in darkmode+mute')

open(path, 'w').write(src)
remaining = src.count('rateLimitCallback\|markUiActivityCallback')
remaining2 = src.count('std::function')
print(f'wifi_routes.cpp std::function remaining: {remaining2}')

# ============================================================
# 9. test_wifi_orchestrator_module stub
# ============================================================
path = 'test/test_wifi_orchestrator_module/wifi_manager_test_stub.h'
src = open(path).read()

# Replace entire setter methods + private storage section
old_setters_and_storage = '''    void setStatusCallback(std::function<void(ArduinoJson::JsonObject)> callback) {
        ++statusCallbackCalls;
        statusCallback = std::move(callback);
    }

    void appendStatusCallback(std::function<void(ArduinoJson::JsonObject)> callback) {
        ++statusCallbackCalls;
        if (!callback) {
            return;
        }
        if (!statusCallback) {
            statusCallback = std::move(callback);
            return;
        }
        auto previous = std::move(statusCallback);
        statusCallback = [previous = std::move(previous), callback = std::move(callback)](
                             ArduinoJson::JsonObject obj) {
            previous(obj);
            callback(obj);
        };
    }

    void setAlertCallback(std::function<void(ArduinoJson::JsonObject)> callback) {
        ++alertCallbackCalls;
        alertCallback = std::move(callback);
    }

    void setCommandCallback(std::function<bool(const char*, bool)> callback) {
        ++commandCallbackCalls;
        commandCallback = std::move(callback);
    }

    void setFilesystemCallback(std::function<fs::FS*()> callback) {
        ++filesystemCallbackCalls;
        filesystemCallback = std::move(callback);
    }

    void setProfilePushCallback(std::function<WifiControlApiService::ProfilePushResult()> callback) {
        ++profilePushCallbackCalls;
        profilePushCallback = std::move(callback);
    }

    void setPushStatusCallback(std::function<String()> callback) {
        ++pushStatusCallbackCalls;
        pushStatusCallback = std::move(callback);
    }

    void setPushNowCallback(
        std::function<WifiAutoPushApiService::PushNowQueueResult(
            const WifiAutoPushApiService::PushNowRequest&)> callback) {
        ++pushNowCallbackCalls;
        pushNowCallback = std::move(callback);
    }

    void setV1ConnectedCallback(std::function<bool()> callback) {
        ++v1ConnectedCallbackCalls;
        v1ConnectedCallback = std::move(callback);
    }

private:
    std::function<void(ArduinoJson::JsonObject)> statusCallback;
    std::function<void(ArduinoJson::JsonObject)> alertCallback;
    std::function<bool(const char*, bool)> commandCallback;
    std::function<fs::FS*()> filesystemCallback;
    std::function<WifiControlApiService::ProfilePushResult()> profilePushCallback;
    std::function<String()> pushStatusCallback;
    std::function<WifiAutoPushApiService::PushNowQueueResult(
        const WifiAutoPushApiService::PushNowRequest&)> pushNowCallback;
    std::function<bool()> v1ConnectedCallback;
};'''

new_setters_and_storage = '''    void setStatusCallback(void (*fn)(ArduinoJson::JsonObject, void*), void* ctx) {
        ++statusCallbackCalls;
        statusCallback = fn;
        statusCallbackCtx = ctx;
    }

    void appendStatusCallback(void (*fn)(ArduinoJson::JsonObject, void*), void* ctx) {
        ++statusCallbackCalls;
        statusCallback2 = fn;
        statusCallback2Ctx = ctx;
    }

    void setAlertCallback(void (*fn)(ArduinoJson::JsonObject, void*), void* ctx) {
        ++alertCallbackCalls;
        alertCallback = fn;
        alertCallbackCtx = ctx;
    }

    void setCommandCallback(bool (*fn)(const char*, bool, void*), void* ctx) {
        ++commandCallbackCalls;
        commandCallback = fn;
        commandCallbackCtx = ctx;
    }

    void setFilesystemCallback(fs::FS* (*fn)(void*), void* ctx) {
        ++filesystemCallbackCalls;
        filesystemCallback = fn;
        filesystemCallbackCtx = ctx;
    }

    void setProfilePushCallback(WifiControlApiService::ProfilePushResult (*fn)(void*), void* ctx) {
        ++profilePushCallbackCalls;
        profilePushCallback = fn;
        profilePushCallbackCtx = ctx;
    }

    void setPushStatusCallback(String (*fn)(void*), void* ctx) {
        ++pushStatusCallbackCalls;
        pushStatusCallback = fn;
        pushStatusCallbackCtx = ctx;
    }

    void setPushNowCallback(
        WifiAutoPushApiService::PushNowQueueResult (*fn)(
            const WifiAutoPushApiService::PushNowRequest&, void*),
        void* ctx) {
        ++pushNowCallbackCalls;
        pushNowCallback = fn;
        pushNowCallbackCtx = ctx;
    }

    void setV1ConnectedCallback(bool (*fn)(void*), void* ctx) {
        ++v1ConnectedCallbackCalls;
        v1ConnectedCallback = fn;
        v1ConnectedCallbackCtx = ctx;
    }

private:
    void (*statusCallback)(ArduinoJson::JsonObject, void*) = nullptr;
    void* statusCallbackCtx = nullptr;
    void (*statusCallback2)(ArduinoJson::JsonObject, void*) = nullptr;
    void* statusCallback2Ctx = nullptr;
    void (*alertCallback)(ArduinoJson::JsonObject, void*) = nullptr;
    void* alertCallbackCtx = nullptr;
    bool (*commandCallback)(const char*, bool, void*) = nullptr;
    void* commandCallbackCtx = nullptr;
    fs::FS* (*filesystemCallback)(void*) = nullptr;
    void* filesystemCallbackCtx = nullptr;
    WifiControlApiService::ProfilePushResult (*profilePushCallback)(void*) = nullptr;
    void* profilePushCallbackCtx = nullptr;
    String (*pushStatusCallback)(void*) = nullptr;
    void* pushStatusCallbackCtx = nullptr;
    WifiAutoPushApiService::PushNowQueueResult (*pushNowCallback)(
        const WifiAutoPushApiService::PushNowRequest&, void*) = nullptr;
    void* pushNowCallbackCtx = nullptr;
    bool (*v1ConnectedCallback)(void*) = nullptr;
    void* v1ConnectedCallbackCtx = nullptr;
};'''

src = replace_exactly_once(src, old_setters_and_storage, new_setters_and_storage, 'test stub setters + storage')

# Update reset() to zero fn-ptrs instead of {}
old_reset_callbacks = '''        statusCallback = {};
        alertCallback = {};
        commandCallback = {};
        filesystemCallback = {};
        profilePushCallback = {};
        pushStatusCallback = {};
        pushNowCallback = {};
        v1ConnectedCallback = {};'''

new_reset_callbacks = '''        statusCallback = nullptr;
        statusCallbackCtx = nullptr;
        statusCallback2 = nullptr;
        statusCallback2Ctx = nullptr;
        alertCallback = nullptr;
        alertCallbackCtx = nullptr;
        commandCallback = nullptr;
        commandCallbackCtx = nullptr;
        filesystemCallback = nullptr;
        filesystemCallbackCtx = nullptr;
        profilePushCallback = nullptr;
        profilePushCallbackCtx = nullptr;
        pushStatusCallback = nullptr;
        pushStatusCallbackCtx = nullptr;
        pushNowCallback = nullptr;
        pushNowCallbackCtx = nullptr;
        v1ConnectedCallback = nullptr;
        v1ConnectedCallbackCtx = nullptr;'''

src = replace_exactly_once(src, old_reset_callbacks, new_reset_callbacks, 'reset() callbacks')

# Remove #include <functional> from stub
src = replace_exactly_once(src, '#include <functional>\n\n', '', 'remove <functional> from stub')

open(path, 'w').write(src)
remaining = src.count('std::function')
print(f'orchestrator test stub std::function remaining: {remaining}')

# ============================================================
# 10. test_wifi_status_api_service.cpp
# ============================================================
path = 'test/test_wifi_status_api_service/test_wifi_status_api_service.cpp'
src = open(path).read()

# Update FakeStatusRuntime - mergeStatus/mergeAlert remain as std::function for test convenience
# (they're wrapped in makeRuntime)
# No change to FakeStatusRuntime needed since makeRuntime wraps them.

# Replace makeRuntime function - all [&rt]() lambdas to fn-ptr pairs
old_make_rt = '''static WifiStatusApiService::StatusRuntime makeRuntime(FakeStatusRuntime& rt) {
    return WifiStatusApiService::StatusRuntime{
        [&rt]() {
            rt.setupModeActiveCalls++;
            return rt.setupModeActive;
        },
        [&rt]() { return rt.staConnected; },
        [&rt]() { return rt.staIp; },
        [&rt]() { return rt.apIp; },
        [&rt]() { return rt.connectedSsid; },
        [&rt]() { return rt.rssi; },
        [&rt]() { return rt.staEnabled; },
        [&rt]() { return rt.staSavedSsid; },
        [&rt]() { return rt.apSsid; },

        [&rt]() { return rt.uptimeSeconds; },
        [&rt]() { return rt.heapFree; },
        [&rt]() { return rt.hostname; },
        [&rt]() { return rt.firmwareVersion; },

        [&rt]() { return rt.timeValid; },
        [&rt]() { return rt.timeSource; },
        [&rt]() { return rt.timeConfidence; },
        [&rt]() { return rt.timeTzOffsetMin; },
        [&rt]() { return rt.timeEpochMs; },
        [&rt]() { return rt.timeAgeMs; },

        [&rt]() { return rt.batteryVoltageMv; },
        [&rt]() { return rt.batteryPercentage; },
        [&rt]() { return rt.batteryOnBattery; },
        [&rt]() { return rt.batteryHasBattery; },

        [&rt]() { return rt.v1Connected; },
        [&rt](JsonObject obj) { if (rt.mergeStatus) rt.mergeStatus(obj); },
        [&rt](JsonObject obj) { if (rt.mergeAlert) rt.mergeAlert(obj); },
    };
}'''

new_make_rt = '''static WifiStatusApiService::StatusRuntime makeRuntime(FakeStatusRuntime& rt) {
    WifiStatusApiService::StatusRuntime r;
    r.setupModeActive = [](void* ctx) {
        auto* rtp = static_cast<FakeStatusRuntime*>(ctx);
        rtp->setupModeActiveCalls++;
        return rtp->setupModeActive;
    };
    r.setupModeActiveCtx = &rt;
    r.staConnected = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->staConnected; };
    r.staConnectedCtx = &rt;
    r.staIp = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->staIp; };
    r.staIpCtx = &rt;
    r.apIp = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->apIp; };
    r.apIpCtx = &rt;
    r.connectedSsid = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->connectedSsid; };
    r.connectedSsidCtx = &rt;
    r.rssi = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->rssi; };
    r.rssiCtx = &rt;
    r.staEnabled = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->staEnabled; };
    r.staEnabledCtx = &rt;
    r.staSavedSsid = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->staSavedSsid; };
    r.staSavedSsidCtx = &rt;
    r.apSsid = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->apSsid; };
    r.apSsidCtx = &rt;
    r.uptimeSeconds = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->uptimeSeconds; };
    r.uptimeSecondsCtx = &rt;
    r.heapFree = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->heapFree; };
    r.heapFreeCtx = &rt;
    r.hostname = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->hostname; };
    r.hostnameCtx = &rt;
    r.firmwareVersion = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->firmwareVersion; };
    r.firmwareVersionCtx = &rt;
    r.timeValid = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->timeValid; };
    r.timeValidCtx = &rt;
    r.timeSource = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->timeSource; };
    r.timeSourceCtx = &rt;
    r.timeConfidence = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->timeConfidence; };
    r.timeConfidenceCtx = &rt;
    r.timeTzOffsetMin = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->timeTzOffsetMin; };
    r.timeTzOffsetMinCtx = &rt;
    r.timeEpochMsOr0 = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->timeEpochMs; };
    r.timeEpochMsOr0Ctx = &rt;
    r.timeEpochAgeMsOr0 = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->timeAgeMs; };
    r.timeEpochAgeMsOr0Ctx = &rt;
    r.batteryVoltageMv = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->batteryVoltageMv; };
    r.batteryVoltageMvCtx = &rt;
    r.batteryPercentage = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->batteryPercentage; };
    r.batteryPercentageCtx = &rt;
    r.batteryOnBattery = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->batteryOnBattery; };
    r.batteryOnBatteryCtx = &rt;
    r.batteryHasBattery = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->batteryHasBattery; };
    r.batteryHasBatteryCtx = &rt;
    r.v1Connected = [](void* ctx) { return static_cast<FakeStatusRuntime*>(ctx)->v1Connected; };
    r.v1ConnectedCtx = &rt;
    r.mergeStatus = [](JsonObject obj, void* ctx) {
        auto* rtp = static_cast<FakeStatusRuntime*>(ctx);
        if (rtp->mergeStatus) rtp->mergeStatus(obj);
    };
    r.mergeStatusCtx = &rt;
    r.mergeAlert = [](JsonObject obj, void* ctx) {
        auto* rtp = static_cast<FakeStatusRuntime*>(ctx);
        if (rtp->mergeAlert) rtp->mergeAlert(obj);
    };
    r.mergeAlertCtx = &rt;
    return r;
}'''

src = replace_exactly_once(src, old_make_rt, new_make_rt, 'test makeRuntime')

# Update all handleApiStatus calls: [&now]() { return now; } -> fn-ptr + &now
# Pattern handles both true/false rate limit variants
src = re.sub(
    r'\[&now\]\(\) \{ return now; \},\s*\n\s*\[\]\(\) \{ return (true|false); \}\);',
    lambda m: f'[](void* ctx) -> unsigned long {{ return *static_cast<unsigned long*>(ctx); }}, &now,\n        [](void* /*ctx*/) {{ return {m.group(1)}; }}, nullptr);',
    src
)

# Static millis values: []() { return NUL; }, []() { return true/false; }
src = re.sub(
    r'\[\]\(\) \{ return (\d+)UL; \},\s*\n\s*\[\]\(\) \{ return (true|false); \}\);',
    lambda m: f'[](void* /*ctx*/) -> unsigned long {{ return {m.group(1)}UL; }}, nullptr,\n        [](void* /*ctx*/) {{ return {m.group(2)}; }}, nullptr);',
    src
)

# handleApiLegacyStatus: []() { return 3000UL; }); - single trailing arg
src = re.sub(
    r'\[\]\(\) \{ return (\d+)UL; \}\);',
    lambda m: f'[](void* /*ctx*/) -> unsigned long {{ return {m.group(1)}UL; }}, nullptr);',
    src
)

open(path, 'w').write(src)
remaining = src.count('std::function')
# std::function is still used in FakeStatusRuntime for mergeStatus/mergeAlert - that's expected
print(f'test std::function remaining (FakeStatusRuntime mergeStatus/mergeAlert): {remaining}')
old_style = src.count('[]() {')
print(f'test old []() {{ remaining: {old_style}')
print('Migration complete.')

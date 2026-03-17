#pragma once

#include <functional>

#include "../mocks/Arduino.h"
#include "../mocks/FS.h"
#include <ArduinoJson.h>

// Keep this guard aligned with the production header so the real wifi_manager.h
// becomes a no-op when the orchestrator header is included in this test.
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

namespace WifiControlApiService {
enum class ProfilePushResult : uint8_t {
    QUEUED = 0,
    ALREADY_IN_PROGRESS,
    HANDLER_UNAVAILABLE,
};
}  // namespace WifiControlApiService

namespace WifiAutoPushApiService {
struct PushNowRequest {
    int slot = 0;
    bool hasProfileOverride = false;
    String profileName;
    bool hasModeOverride = false;
    int mode = 0;
};

enum class PushNowQueueResult : uint8_t {
    QUEUED = 0,
    V1_NOT_CONNECTED,
    ALREADY_IN_PROGRESS,
    NO_PROFILE_CONFIGURED,
    PROFILE_LOAD_FAILED,
};
}  // namespace WifiAutoPushApiService

class WiFiManager {
public:
    bool setupModeActive = false;
    bool startSetupModeResult = true;
    int startSetupModeCalls = 0;
    bool lastStartAutoStarted = false;

    int statusCallbackCalls = 0;
    int alertCallbackCalls = 0;
    int commandCallbackCalls = 0;
    int filesystemCallbackCalls = 0;
    int profilePushCallbackCalls = 0;
    int pushStatusCallbackCalls = 0;
    int pushNowCallbackCalls = 0;
    int v1ConnectedCallbackCalls = 0;

    void reset() {
        setupModeActive = false;
        startSetupModeResult = true;
        startSetupModeCalls = 0;
        lastStartAutoStarted = false;
        statusCallbackCalls = 0;
        alertCallbackCalls = 0;
        commandCallbackCalls = 0;
        filesystemCallbackCalls = 0;
        profilePushCallbackCalls = 0;
        pushStatusCallbackCalls = 0;
        pushNowCallbackCalls = 0;
        v1ConnectedCallbackCalls = 0;
        statusCallback = {};
        alertCallback = {};
        commandCallback = {};
        filesystemCallback = {};
        profilePushCallback = {};
        pushStatusCallback = {};
        pushNowCallback = {};
        v1ConnectedCallback = {};
    }

    bool isSetupModeActive() const { return setupModeActive; }

    bool startSetupMode(bool autoStarted = false) {
        ++startSetupModeCalls;
        lastStartAutoStarted = autoStarted;
        return startSetupModeResult;
    }

    void setStatusCallback(std::function<void(ArduinoJson::JsonObject)> callback) {
        ++statusCallbackCalls;
        statusCallback = std::move(callback);
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
};

#endif  // WIFI_MANAGER_H

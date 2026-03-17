#include "wifi_orchestrator_module.h"

#include "settings_sanitize.h"

WifiOrchestrator::WifiOrchestrator(WiFiManager& wifiManager,
                                   V1BLEClient& bleClient,
                                   PacketParser& parser,
                                   SettingsManager& settingsManager,
                                   StorageManager& storageManager,
                                   AutoPushModule& autoPushModule)
    : wifiManager(wifiManager),
      bleClient(bleClient),
      parser(parser),
      settingsManager(settingsManager),
      storageManager(storageManager),
      autoPushModule(autoPushModule) {}

void WifiOrchestrator::ensureCallbacksConfigured() {
    if (!callbacksConfigured) {
        configureCallbacks();
        callbacksConfigured = true;
    }
}

void WifiOrchestrator::configureCallbacks() {
    // V1 connection status
    wifiManager.setStatusCallback([this](JsonObject obj) {
        obj["v1_connected"] = bleClient.isConnected();
    });

    // Current alert state
    wifiManager.setAlertCallback([this](JsonObject obj) {
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
    });

    // Command handling (dark mode / mute)
    wifiManager.setCommandCallback([this](const char* cmd, bool state) {
        if (strcmp(cmd, "display") == 0) {
            return bleClient.setDisplayOn(state);
        } else if (strcmp(cmd, "mute") == 0) {
            return bleClient.setMute(state);
        }
        return false;
    });

    // Filesystem for web APIs
    wifiManager.setFilesystemCallback([this]() -> fs::FS* {
        return storageManager.isReady() ? storageManager.getFilesystem() : nullptr;
    });

    // Manual profile push
    wifiManager.setProfilePushCallback([this]() {
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
    });

    // Auto-push executor status
    wifiManager.setPushStatusCallback([this]() {
        return autoPushModule.getStatusJson();
    });

    wifiManager.setPushNowCallback([this](const WifiAutoPushApiService::PushNowRequest& request) {
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
    });

    // V1 connection state (used to defer WiFi client operations until V1 is connected)
    wifiManager.setV1ConnectedCallback([this]() {
        return bleClient.isConnected();
    });
}

#include "wifi_orchestrator_module.h"

#include "settings_sanitize.h"
#include "../quiet/quiet_coordinator_module.h"

WifiOrchestrator::WifiOrchestrator(WiFiManager& wifiManager,
                                   V1BLEClient& bleClient,
                                   PacketParser& parser,
                                   SettingsManager& settingsManager,
                                   StorageManager& storageManager,
                                   AutoPushModule& autoPushModule,
                                   QuietCoordinatorModule& quietCoordinator)
    : wifiManager(wifiManager),
      bleClient(bleClient),
      parser(parser),
      settingsManager(settingsManager),
      storageManager(storageManager),
      autoPushModule(autoPushModule),
      quietCoordinator(quietCoordinator) {}

void WifiOrchestrator::ensureCallbacksConfigured() {
    if (!callbacksConfigured) {
        configureCallbacks();
        callbacksConfigured = true;
    }
}

void WifiOrchestrator::configureCallbacks() {
    // V1 connection status
    wifiManager.setStatusCallback(
        [](JsonObject obj, void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            obj["v1_connected"] = self->bleClient.isConnected();
        }, this);

    // Current alert state
    wifiManager.setAlertCallback(
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
        }, this);

    // Command handling (dark mode / mute)
    wifiManager.setCommandCallback(
        [](const char* cmd, bool state, void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            if (strcmp(cmd, "display") == 0) {
                return self->bleClient.setDisplayOn(state);
            } else if (strcmp(cmd, "mute") == 0) {
                return self->quietCoordinator.sendMute(QuietOwner::WifiCommand, state);
            }
            return false;
        }, this);

    // Filesystem for web APIs
    wifiManager.setFilesystemCallback(
        [](void* ctx) -> fs::FS* {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->storageManager.isReady() ? self->storageManager.getFilesystem() : nullptr;
        }, this);

    // Manual profile push
    wifiManager.setProfilePushCallback(
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
        }, this);

    // Auto-push executor status
    wifiManager.setPushStatusCallback(
        [](void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->autoPushModule.getStatusJson();
        }, this);

    wifiManager.setPushNowCallback(
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
        }, this);

    // V1 connection state (used to defer WiFi client operations until V1 is connected)
    wifiManager.setV1ConnectedCallback(
        [](void* ctx) {
            auto* self = static_cast<WifiOrchestrator*>(ctx);
            return self->bleClient.isConnected();
        }, this);
}

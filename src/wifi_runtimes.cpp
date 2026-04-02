/**
 * WiFi Runtimes — make*Runtime() factory methods.
 * Extracted from wifi_manager.cpp for maintainability.
 */

#include "wifi_manager_internals.h"
#include "perf_metrics.h"
#include "settings.h"
#include "settings_sanitize.h"
#include "display.h"
#include "v1_profiles.h"
#include "v1_devices.h"
#include "audio_beep.h"
#include "battery_manager.h"
#include "modules/wifi/wifi_autopush_api_service.h"
#include "modules/wifi/wifi_audio_api_service.h"
#include "modules/wifi/wifi_display_colors_api_service.h"
#include "modules/wifi/wifi_settings_api_service.h"
#include "modules/wifi/wifi_status_api_service.h"
#include "modules/wifi/wifi_client_api_service.h"
#include "modules/wifi/wifi_v1_profile_api_service.h"
#include "modules/wifi/wifi_v1_devices_api_service.h"
#include "modules/wifi/backup_api_service.h"
#include "modules/debug/debug_perf_files_service.h"
#include "backup_payload_builder.h"
#include "storage_manager.h"
#include "perf_sd_logger.h"
#include "settings_runtime_sync.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/obd/obd_runtime_module.h"
#include "time_service.h"
#include "../include/config.h"

extern SpeedSourceSelector speedSourceSelector;
extern ObdRuntimeModule    obdRuntimeModule;

WifiAutoPushApiService::Runtime WiFiManager::makeAutoPushRuntime() {
    return WifiAutoPushApiService::Runtime{
        [](WifiAutoPushApiService::SlotsSnapshot& snapshot, void* /*ctx*/) {
            const V1Settings& s = settingsManager.get();
            snapshot.enabled = s.autoPushEnabled;
            snapshot.activeSlot = s.activeSlot;

            for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
                const V1Settings::ConstAutoPushSlotView slot = s.autoPushSlotView(slotIndex);
                snapshot.slots[slotIndex].name = slot.name;
                snapshot.slots[slotIndex].profile = slot.config.profileName;
                snapshot.slots[slotIndex].mode = slot.config.mode;
                snapshot.slots[slotIndex].color = slot.color;
                snapshot.slots[slotIndex].volume = slot.volume;
                snapshot.slots[slotIndex].muteVolume = slot.muteVolume;
                snapshot.slots[slotIndex].darkMode = slot.darkMode;
                snapshot.slots[slotIndex].muteToZero = slot.muteToZero;
                snapshot.slots[slotIndex].alertPersist = slot.alertPersist;
                snapshot.slots[slotIndex].priorityArrowOnly = slot.priorityArrow;
            }
        }, nullptr,
        [](String& json, void* ctx) {
            auto* mgr = static_cast<WiFiManager*>(ctx);
            if (!mgr->getPushStatusJson_) {
                return false;
            }
            json = mgr->getPushStatusJson_(mgr->getPushStatusJsonCtx_);
            return true;
        }, this,
        [](const WifiAutoPushApiService::SlotUpdateRequest& request, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = request.slot;
            update.hasName = request.hasName;
            update.name = request.name;
            update.hasColor = request.hasColor;
            update.color = request.color;
            update.hasVolume = request.hasVolume;
            update.volume = request.volume;
            update.hasMuteVolume = request.hasMuteVolume;
            update.muteVolume = request.muteVolume;
            update.hasDarkMode = request.hasDarkMode;
            update.darkMode = request.darkMode;
            update.hasMuteToZero = request.hasMuteToZero;
            update.muteToZero = request.muteToZero;
            update.hasAlertPersist = request.hasAlertPersist;
            update.alertPersist = request.alertPersist;
            update.hasPriorityArrowOnly = request.hasPriorityArrowOnly;
            update.priorityArrowOnly = request.priorityArrowOnly;
            update.hasProfileName = true;
            update.profileName = request.profile;
            update.hasMode = true;
            update.mode = normalizeV1ModeValue(request.mode);
            return settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, const String& name, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasName = true;
            update.name = name;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, uint16_t color, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasColor = true;
            update.color = color;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, void* /*ctx*/) {
            return settingsManager.getSlotVolume(slot);
        }, nullptr,
        [](int slot, void* /*ctx*/) {
            return settingsManager.getSlotMuteVolume(slot);
        }, nullptr,
        [](int slot, uint8_t volume, uint8_t muteVolume, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasVolume = true;
            update.volume = volume;
            update.hasMuteVolume = true;
            update.muteVolume = muteVolume;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, bool darkMode, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasDarkMode = true;
            update.darkMode = darkMode;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, bool muteToZero, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasMuteToZero = true;
            update.muteToZero = muteToZero;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, uint8_t alertPersistSec, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasAlertPersist = true;
            update.alertPersist = alertPersistSec;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, bool priorityArrowOnly, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasPriorityArrowOnly = true;
            update.priorityArrowOnly = priorityArrowOnly;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, const String& profile, int mode, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasProfileName = true;
            update.profileName = profile;
            update.hasMode = true;
            update.mode = normalizeV1ModeValue(mode);
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](void* /*ctx*/) {
            return static_cast<int>(settingsManager.get().activeSlot);
        }, nullptr,
        [](int slot, void* /*ctx*/) {
            display.drawProfileIndicator(slot);
        }, nullptr,
        [](const WifiAutoPushApiService::ActivationRequest& request, void* /*ctx*/) {
            AutoPushStateUpdate update;
            update.hasActiveSlot = true;
            update.activeSlot = request.slot;
            update.hasEnabled = true;
            update.enabled = request.enable;
            return settingsManager.applyAutoPushStateUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, void* /*ctx*/) {
            AutoPushStateUpdate update;
            update.hasActiveSlot = true;
            update.activeSlot = slot;
            (void)settingsManager.applyAutoPushStateUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](bool enabled, void* /*ctx*/) {
            AutoPushStateUpdate update;
            update.hasEnabled = true;
            update.enabled = enabled;
            (void)settingsManager.applyAutoPushStateUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](const WifiAutoPushApiService::PushNowRequest& request, void* ctx) {
            auto* mgr = static_cast<WiFiManager*>(ctx);
            if (!mgr->queuePushNow_) {
                return WifiAutoPushApiService::PushNowQueueResult::PROFILE_LOAD_FAILED;
            }
            return mgr->queuePushNow_(request, mgr->queuePushNowCtx_);
        }, this,
    };
}

WifiDisplayColorsApiService::Runtime WiFiManager::makeDisplayColorsRuntime() {
    return WifiDisplayColorsApiService::Runtime{
        [](void* /*ctx*/) -> const V1Settings& {
            return settingsManager.get();
        }, nullptr,
        [](const DisplaySettingsUpdate& update, void* /*ctx*/) {
            settingsManager.applyDisplaySettingsUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](void* /*ctx*/) {
            settingsManager.resetDisplaySettings(SettingsPersistMode::Deferred);
        }, nullptr,
        [](uint8_t brightness, void* /*ctx*/) {
            display.setBrightness(brightness);
        }, nullptr,
        [](void* /*ctx*/) {
            display.forceNextRedraw();
        }, nullptr,
        [](uint32_t durationMs, void* /*ctx*/) {
            requestColorPreviewHold(durationMs);
        }, nullptr,
        [](void* /*ctx*/) {
            return isDisplayPreviewRunning();
        }, nullptr,
        [](void* /*ctx*/) {
            cancelDisplayPreview();
        }, nullptr,
    };
}

WifiAudioApiService::Runtime WiFiManager::makeAudioRuntime() {
    WifiAudioApiService::Runtime r;
    r.ctx = this;
    r.getSettings = [](void* /*ctx*/) -> const V1Settings& {
        return settingsManager.get();
    };
    r.applySettingsUpdate = [](const AudioSettingsUpdate& update, void* /*ctx*/) {
        settingsManager.applyAudioSettingsUpdate(update, SettingsPersistMode::Deferred);
    };
    r.setAudioVolume = [](uint8_t volume, void* /*ctx*/) {
        audio_set_volume(volume);
    };
    r.checkRateLimit = [](void* ctx) {
        return static_cast<WiFiManager*>(ctx)->checkRateLimit();
    };
    return r;
}

WifiStatusApiService::StatusRuntime WiFiManager::makeStatusRuntime() {
    return WifiStatusApiService::StatusRuntime{
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->isSetupModeActive(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->wifiClientState_ == WIFI_CLIENT_CONNECTED; }, this,
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
        [](void* /*ctx*/) { return timeService.timeValid(); }, nullptr,
        [](void* /*ctx*/) { return timeService.timeSource(); }, nullptr,
        [](void* /*ctx*/) { return timeService.timeConfidence(); }, nullptr,
        [](void* /*ctx*/) { return timeService.tzOffsetMinutes(); }, nullptr,
        [](void* /*ctx*/) { return timeService.nowEpochMsOr0(); }, nullptr,
        [](void* /*ctx*/) { return timeService.epochAgeMsOr0(); }, nullptr,
        [](void* /*ctx*/) { return batteryManager.getVoltageMillivolts(); }, nullptr,
        [](void* /*ctx*/) { return batteryManager.getPercentage(); }, nullptr,
        [](void* /*ctx*/) { return batteryManager.isOnBattery(); }, nullptr,
        [](void* /*ctx*/) { return batteryManager.hasBattery(); }, nullptr,
        [](void* /*ctx*/) { return bleClient.isConnected(); }, nullptr,
        mergeStatus_, mergeStatusCtx_,
        mergeStatus2_, mergeStatus2Ctx_,
        mergeAlert_, mergeAlertCtx_,
    };
}

WifiSettingsApiService::Runtime WiFiManager::makeSettingsRuntime() {
    WifiSettingsApiService::Runtime r;
    r.ctx = this;
    r.getSettings = [](void* /*ctx*/) -> const V1Settings& {
        return settingsManager.get();
    };
    r.applySettingsUpdate = [](const DeviceSettingsUpdate& update, void* /*ctx*/) {
        settingsManager.applyDeviceSettingsUpdate(update, SettingsPersistMode::Deferred);
    };
    r.checkRateLimit = [](void* ctx) {
        return static_cast<WiFiManager*>(ctx)->checkRateLimit();
    };
    return r;
}

WifiClientApiService::Runtime WiFiManager::makeWifiClientRuntime() {
    return WifiClientApiService::Runtime{
        [](void* /*ctx*/) { return settingsManager.get().wifiClientEnabled; }, nullptr,
        [](void* /*ctx*/) { return settingsManager.get().wifiClientSSID; }, nullptr,
        [](void* ctx) { return wifiClientStateApiName(static_cast<WiFiManager*>(ctx)->wifiClientState_); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->wifiScanRunning_; }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->wifiClientState_ == WIFI_CLIENT_CONNECTED; }, this,
        [](void* /*ctx*/) {
            WifiClientApiService::ConnectedNetworkPayload payload;
            payload.ssid = WiFi.SSID();
            payload.ip = WiFi.localIP().toString();
            payload.rssi = WiFi.RSSI();
            return payload;
        }, nullptr,
        [](void* /*ctx*/) { return WiFi.scanComplete() == WIFI_SCAN_RUNNING; }, nullptr,
        [](void* /*ctx*/) { return WiFi.scanComplete() > 0; }, nullptr,
        [](void* ctx) {
            auto* self = static_cast<WiFiManager*>(ctx);
            std::vector<ScannedNetwork> networks = self->getScannedNetworks();
            std::vector<WifiClientApiService::ScannedNetworkPayload> payloads;
            payloads.reserve(networks.size());
            for (const auto& net : networks) {
                WifiClientApiService::ScannedNetworkPayload payload;
                payload.ssid = net.ssid;
                payload.rssi = net.rssi;
                payload.secure = !net.isOpen();
                payloads.push_back(payload);
            }
            return payloads;
        }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->startWifiScan(); }, this,
        [](const String& ssid, const String& password, void* ctx) {
            return static_cast<WiFiManager*>(ctx)->connectToNetwork(ssid, password);
        }, this,
        [](void* ctx) { static_cast<WiFiManager*>(ctx)->disconnectFromNetwork(); }, this,
        [](void* ctx) { static_cast<WiFiManager*>(ctx)->forgetWifiClient(); }, this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->enableWifiClientFromSavedCredentials(); }, this,
        [](void* ctx) { static_cast<WiFiManager*>(ctx)->disableWifiClient(); }, this,
    };
}

WifiV1ProfileApiService::Runtime WiFiManager::makeV1ProfileRuntime() {
    return WifiV1ProfileApiService::Runtime{
        [](void* /*ctx*/) { return v1ProfileManager.listProfiles(); }, nullptr,
        [](const String& name, WifiV1ProfileApiService::ProfileSummary& summary, void* /*ctx*/) {
            V1Profile profile;
            if (!v1ProfileManager.loadProfile(name, profile)) {
                return false;
            }
            summary.name = profile.name;
            summary.description = profile.description;
            summary.displayOn = profile.displayOn;
            return true;
        }, nullptr,
        [](const String& name, String& json, void* /*ctx*/) {
            V1Profile profile;
            if (!v1ProfileManager.loadProfile(name, profile)) {
                return false;
            }
            json = v1ProfileManager.profileToJson(profile);
            return true;
        }, nullptr,
        [](const String& name, uint8_t outBytes[6], bool& displayOn, void* /*ctx*/) {
            V1Profile profile;
            if (!v1ProfileManager.loadProfile(name, profile)) {
                return false;
            }
            memcpy(outBytes, profile.settings.bytes, 6);
            displayOn = profile.displayOn;
            return true;
        }, nullptr,
        [](const JsonObject& settingsObj, uint8_t outBytes[6], void* /*ctx*/) {
            V1UserSettings settings;
            if (!v1ProfileManager.jsonToSettings(settingsObj, settings)) {
                return false;
            }
            memcpy(outBytes, settings.bytes, 6);
            return true;
        }, nullptr,
        [](const String& name,
           const String& description,
           bool displayOn,
           const uint8_t inBytes[6],
           String& error,
           void* /*ctx*/) {
            V1Profile profile;
            profile.name = name;
            profile.description = description;
            profile.displayOn = displayOn;
            memcpy(profile.settings.bytes, inBytes, 6);
            ProfileSaveResult result = v1ProfileManager.saveProfile(profile);
            if (!result.success) {
                error = result.error;
                return false;
            }
            return true;
        }, nullptr,
        [](const String& name, void* /*ctx*/) { return v1ProfileManager.deleteProfile(name); }, nullptr,
        [](void* /*ctx*/) { return bleClient.requestUserBytes(); }, nullptr,
        [](const uint8_t inBytes[6], void* /*ctx*/) {
            return bleClient.writeUserBytesVerified(inBytes, 3) == V1BLEClient::VERIFY_OK;
        }, nullptr,
        [](bool displayOn, void* /*ctx*/) { bleClient.setDisplayOn(displayOn); }, nullptr,
        [](void* /*ctx*/) { return v1ProfileManager.hasCurrentSettings(); }, nullptr,
        [](void* /*ctx*/) { return v1ProfileManager.settingsToJson(v1ProfileManager.getCurrentSettings()); }, nullptr,
        [](void* /*ctx*/) { return bleClient.isConnected(); }, nullptr,
        [](void* /*ctx*/) { settingsManager.requestDeferredBackupFromCurrentState(); }, nullptr,
    };
}

WifiV1DevicesApiService::Runtime WiFiManager::makeV1DevicesRuntime() {
    return WifiV1DevicesApiService::Runtime{
        [](void* /*ctx*/) {
            std::vector<WifiV1DevicesApiService::DeviceInfo> payload;
            if (!v1DeviceStore.isReady()) {
                return payload;
            }

            auto devices = v1DeviceStore.listDevices();
            auto hasAddress = [&](const String& address) {
                if (address.length() == 0) {
                    return true;
                }
                for (const auto& device : devices) {
                    if (device.address.equalsIgnoreCase(address)) {
                        return true;
                    }
                }
                return false;
            };

            const String lastV1Address = normalizeV1DeviceAddress(settingsManager.get().lastV1Address);
            if (!hasAddress(lastV1Address)) {
                v1DeviceStore.touchDeviceInMemory(lastV1Address);
                devices = v1DeviceStore.listDevices();
            }

            String connectedAddress;
            NimBLEAddress connected = bleClient.getConnectedAddress();
            if (!connected.isNull()) {
                connectedAddress = normalizeV1DeviceAddress(String(connected.toString().c_str()));
                if (!hasAddress(connectedAddress)) {
                    v1DeviceStore.touchDeviceInMemory(connectedAddress);
                    devices = v1DeviceStore.listDevices();
                }
            }

            payload.reserve(devices.size());
            for (const auto& device : devices) {
                WifiV1DevicesApiService::DeviceInfo info;
                info.address = device.address;
                info.name = device.name;
                info.defaultProfile = device.defaultProfile;
                info.connected = connectedAddress.length() > 0 &&
                                 connectedAddress.equalsIgnoreCase(device.address);
                payload.push_back(info);
            }
            return payload;
        }, nullptr,
        [](const String& address, const String& name, void* /*ctx*/) {
            return v1DeviceStore.setDeviceName(address, name);
        }, nullptr,
        [](const String& address, uint8_t defaultProfile, void* /*ctx*/) {
            return v1DeviceStore.setDeviceDefaultProfile(address, defaultProfile);
        }, nullptr,
        [](const String& address, void* /*ctx*/) {
            return v1DeviceStore.removeDevice(address);
        }, nullptr,
    };
}

BackupApiService::BackupRuntime WiFiManager::makeBackupRuntime() {
    return BackupApiService::BackupRuntime{
        // getBackupRevision
        [](void* /*ctx*/) -> uint32_t { return settingsManager.backupRevision(); },
        // getCatalogRevision
        [](void* /*ctx*/) -> uint32_t { return v1ProfileManager.catalogRevision(); },
        // buildDocument
        [](JsonDocument& doc, uint32_t snapshotMs, void* /*ctx*/) {
            BackupPayloadBuilder::buildBackupDocument(
                doc,
                settingsManager.get(),
                v1ProfileManager,
                BackupPayloadBuilder::BackupTransport::HttpDownload,
                snapshotMs);
        },
        // isStorageReady
        [](void* /*ctx*/) -> bool { return storageManager.isReady(); },
        // isSDCard
        [](void* /*ctx*/) -> bool { return storageManager.isSDCard(); },
        // backupToSD
        [](void* /*ctx*/) -> bool { return settingsManager.backupToSD(); },
        // applyBackup
        [](const JsonDocument& doc, bool fullRestore, int& profilesRestored, void* /*ctx*/) -> bool {
            const SettingsBackupApplyResult result = settingsManager.applyBackupDocument(doc, fullRestore);
            profilesRestored = result.profilesRestored;
            return result.success;
        },
        // syncAfterRestore
        [](void* /*ctx*/) {
            const V1Settings& settings = settingsManager.get();
            SettingsRuntimeSync::syncVehicleRuntimeInputs(settings,
                                                          obdRuntimeModule,
                                                          speedSourceSelector);
        },
        // ctx
        nullptr,
    };
}

DebugPerfFilesService::PerfFilesRuntime WiFiManager::makePerfFilesRuntime() {
    return DebugPerfFilesService::PerfFilesRuntime{
        // isStorageReady
        [](void* /*ctx*/) -> bool { return storageManager.isReady(); },
        // isSDCard
        [](void* /*ctx*/) -> bool { return storageManager.isSDCard(); },
        // getSDMutex (returns SemaphoreHandle_t as void*)
        [](void* /*ctx*/) -> void* { return storageManager.getSDMutex(); },
        // getFilesystem (returns fs::FS* as void*)
        [](void* /*ctx*/) -> void* { return storageManager.getFilesystem(); },
        // isPerfLoggingEnabled
        [](void* /*ctx*/) -> bool { return perfSdLogger.isEnabled(); },
        // getPerfCsvPath
        [](void* /*ctx*/) -> const char* { return perfSdLogger.csvPath(); },
        // ctx
        nullptr,
    };
}

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
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_lockout_safety.h"
#include "modules/lockout/lockout_band_policy.h"
#include "modules/wifi/wifi_autopush_api_service.h"
#include "modules/wifi/wifi_audio_api_service.h"
#include "modules/wifi/wifi_display_colors_api_service.h"
#include "modules/wifi/wifi_settings_api_service.h"
#include "modules/wifi/wifi_status_api_service.h"
#include "modules/wifi/wifi_client_api_service.h"
#include "modules/wifi/wifi_v1_profile_api_service.h"
#include "modules/wifi/wifi_v1_devices_api_service.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/obd/obd_runtime_module.h"
#include "time_service.h"
#include "../include/config.h"

WifiAutoPushApiService::Runtime WiFiManager::makeAutoPushRuntime() {
    return WifiAutoPushApiService::Runtime{
        [this](WifiAutoPushApiService::SlotsSnapshot& snapshot) {
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
        },
        [this](String& json) {
            if (!getPushStatusJson) {
                return false;
            }
            json = getPushStatusJson();
            return true;
        },
        [this](int slot, const String& name) {
            settingsManager.setSlotName(slot, name);
        },
        [this](int slot, uint16_t color) {
            settingsManager.setSlotColor(slot, color);
        },
        [this](int slot) {
            return settingsManager.getSlotVolume(slot);
        },
        [this](int slot) {
            return settingsManager.getSlotMuteVolume(slot);
        },
        [this](int slot, uint8_t volume, uint8_t muteVolume) {
            settingsManager.setSlotVolumes(slot, volume, muteVolume);
        },
        [this](int slot, bool darkMode) {
            settingsManager.setSlotDarkMode(slot, darkMode);
        },
        [this](int slot, bool muteToZero) {
            settingsManager.setSlotMuteToZero(slot, muteToZero);
        },
        [this](int slot, uint8_t alertPersistSec) {
            settingsManager.setSlotAlertPersistSec(slot, alertPersistSec);
        },
        [this](int slot, bool priorityArrowOnly) {
            settingsManager.setSlotPriorityArrowOnly(slot, priorityArrowOnly);
        },
        [this](int slot, const String& profile, int mode) {
            settingsManager.setSlot(slot, profile, normalizeV1ModeValue(mode));
        },
        [this]() {
            return static_cast<int>(settingsManager.get().activeSlot);
        },
        [this](int slot) {
            display.drawProfileIndicator(slot);
        },
        [this](int slot) {
            settingsManager.setActiveSlot(slot);
        },
        [this](bool enabled) {
            settingsManager.setAutoPushEnabled(enabled);
        },
        [this](const WifiAutoPushApiService::PushNowRequest& request) {
            if (!queuePushNow) {
                return WifiAutoPushApiService::PushNowQueueResult::PROFILE_LOAD_FAILED;
            }
            return queuePushNow(request);
        },
    };
}

WifiDisplayColorsApiService::Runtime WiFiManager::makeDisplayColorsRuntime() {
    return WifiDisplayColorsApiService::Runtime{
        [this]() -> const V1Settings& {
            return settingsManager.get();
        },
        [this]() -> V1Settings& {
            return settingsManager.mutableSettings();
        },
        [this](uint8_t brightness) {
            display.setBrightness(brightness);
        },
        [this](DisplayStyle style) {
            settingsManager.updateDisplayStyle(style);
        },
        [this]() {
            display.forceNextRedraw();
        },
        [](uint32_t durationMs) {
            requestColorPreviewHold(durationMs);
        },
        []() {
            return isColorPreviewRunning();
        },
        []() {
            cancelColorPreview();
        },
        [this]() { settingsManager.saveDeferredBackup(); },
    };
}

WifiAudioApiService::Runtime WiFiManager::makeAudioRuntime() {
    return WifiAudioApiService::Runtime{
        [this]() -> const V1Settings& {
            return settingsManager.get();
        },
        [this]() -> V1Settings& {
            return settingsManager.mutableSettings();
        },
        [](uint8_t volume) {
            audio_set_volume(volume);
        },
        [this]() { settingsManager.saveDeferredBackup(); },
    };
}

WifiStatusApiService::StatusRuntime WiFiManager::makeStatusRuntime() {
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
}

WifiSettingsApiService::Runtime WiFiManager::makeSettingsRuntime() {
    return WifiSettingsApiService::Runtime{
        [this]() -> const V1Settings& {
            return settingsManager.get();
        },
        [this]() -> V1Settings& {
            return settingsManager.mutableSettings();
        },
        [this](const String& ssid, const String& password) {
            settingsManager.updateAPCredentials(ssid, password);
        },
        [this](bool enabled) {
            gpsRuntimeModule.setEnabled(enabled);
        },
        [this](bool enabled) {
            speedSourceSelector.setGpsEnabled(enabled);
        },
        [this](bool enabled) {
            lockoutSetKaLearningEnabled(enabled);
        },
        [this](bool enabled) {
            lockoutSetKLearningEnabled(enabled);
        },
        [this](bool enabled) {
            lockoutSetXLearningEnabled(enabled);
        },
        [](bool enabled) {
            obdRuntimeModule.setEnabled(enabled);
        },
        [](int8_t minRssi) {
            obdRuntimeModule.setMinRssi(minRssi);
        },
        [](bool enabled) {
            speedSourceSelector.setObdEnabled(enabled);
        },
        [this]() { settingsManager.saveDeferredBackup(); },
    };
}

WifiClientApiService::Runtime WiFiManager::makeWifiClientRuntime() {
    return WifiClientApiService::Runtime{
        [this]() { return settingsManager.get().wifiClientEnabled; },
        [this]() { return settingsManager.get().wifiClientSSID; },
        [this]() { return wifiClientStateApiName(wifiClientState); },
        [this]() { return wifiScanRunning; },
        [this]() { return wifiClientState == WIFI_CLIENT_CONNECTED; },
        []() {
            WifiClientApiService::ConnectedNetworkPayload payload;
            payload.ssid = WiFi.SSID();
            payload.ip = WiFi.localIP().toString();
            payload.rssi = WiFi.RSSI();
            return payload;
        },
        []() { return WiFi.scanComplete() == WIFI_SCAN_RUNNING; },
        []() { return WiFi.scanComplete() > 0; },
        [this]() {
            std::vector<ScannedNetwork> networks = this->getScannedNetworks();
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
        },
        [this]() { return startWifiScan(); },
        [this](const String& ssid, const String& password) {
            return connectToNetwork(ssid, password);
        },
        [this]() { disconnectFromNetwork(); },
        [this]() { forgetWifiClient(); },
        [this]() { return enableWifiClientFromSavedCredentials(); },
        [this]() { disableWifiClient(); },
    };
}

WifiV1ProfileApiService::Runtime WiFiManager::makeV1ProfileRuntime() {
    return WifiV1ProfileApiService::Runtime{
        []() { return v1ProfileManager.listProfiles(); },
        [](const String& name, WifiV1ProfileApiService::ProfileSummary& summary) {
            V1Profile profile;
            if (!v1ProfileManager.loadProfile(name, profile)) {
                return false;
            }
            summary.name = profile.name;
            summary.description = profile.description;
            summary.displayOn = profile.displayOn;
            return true;
        },
        [](const String& name, String& json) {
            V1Profile profile;
            if (!v1ProfileManager.loadProfile(name, profile)) {
                return false;
            }
            json = v1ProfileManager.profileToJson(profile);
            return true;
        },
        [](const String& name, uint8_t outBytes[6], bool& displayOn) {
            V1Profile profile;
            if (!v1ProfileManager.loadProfile(name, profile)) {
                return false;
            }
            memcpy(outBytes, profile.settings.bytes, 6);
            displayOn = profile.displayOn;
            return true;
        },
        [](const JsonObject& settingsObj, uint8_t outBytes[6]) {
            V1UserSettings settings;
            if (!v1ProfileManager.jsonToSettings(settingsObj, settings)) {
                return false;
            }
            memcpy(outBytes, settings.bytes, 6);
            return true;
        },
        [](const String& name,
           const String& description,
           bool displayOn,
           const uint8_t inBytes[6],
           String& error) {
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
        },
        [](const String& name) { return v1ProfileManager.deleteProfile(name); },
        []() { return bleClient.requestUserBytes(); },
        [](const uint8_t inBytes[6]) {
            return bleClient.writeUserBytesVerified(inBytes, 3) == V1BLEClient::VERIFY_OK;
        },
        [](bool displayOn) { bleClient.setDisplayOn(displayOn); },
        []() { return v1ProfileManager.hasCurrentSettings(); },
        []() { return v1ProfileManager.settingsToJson(v1ProfileManager.getCurrentSettings()); },
        []() { return bleClient.isConnected(); },
        [this]() { settingsManager.requestDeferredBackupFromCurrentState(); },
    };
}

WifiV1DevicesApiService::Runtime WiFiManager::makeV1DevicesRuntime() {
    return WifiV1DevicesApiService::Runtime{
        [this]() {
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
                v1DeviceStore.upsertDevice(lastV1Address);
                devices = v1DeviceStore.listDevices();
            }

            String connectedAddress;
            NimBLEAddress connected = bleClient.getConnectedAddress();
            if (!connected.isNull()) {
                connectedAddress = normalizeV1DeviceAddress(String(connected.toString().c_str()));
                if (!hasAddress(connectedAddress)) {
                    v1DeviceStore.upsertDevice(connectedAddress);
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
        },
        [](const String& address, const String& name) {
            return v1DeviceStore.setDeviceName(address, name);
        },
        [](const String& address, uint8_t defaultProfile) {
            return v1DeviceStore.setDeviceDefaultProfile(address, defaultProfile);
        },
        [](const String& address) {
            return v1DeviceStore.removeDevice(address);
        },
    };
}

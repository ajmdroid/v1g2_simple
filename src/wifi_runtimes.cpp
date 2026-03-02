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
#include "obd_handler.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_lockout_safety.h"
#include "modules/lockout/lockout_band_policy.h"
#include "modules/wifi/wifi_autopush_api_service.h"
#include "modules/wifi/wifi_display_colors_api_service.h"
#include "modules/wifi/wifi_settings_api_service.h"
#include "modules/wifi/wifi_status_api_service.h"
#include "modules/wifi/wifi_time_api_service.h"
#include "modules/wifi/wifi_client_api_service.h"
#include "modules/wifi/wifi_v1_profile_api_service.h"
#include "modules/wifi/wifi_v1_devices_api_service.h"
#include "modules/speed/speed_source_selector.h"
#include "time_service.h"
#include "../include/config.h"

WifiAutoPushApiService::Runtime WiFiManager::makeAutoPushRuntime() {
    return WifiAutoPushApiService::Runtime{
        [this](WifiAutoPushApiService::SlotsSnapshot& snapshot) {
            const V1Settings& s = settingsManager.get();
            snapshot.enabled = s.autoPushEnabled;
            snapshot.activeSlot = s.activeSlot;

            snapshot.slots[0].name = s.slot0Name;
            snapshot.slots[0].profile = s.slot0_default.profileName;
            snapshot.slots[0].mode = s.slot0_default.mode;
            snapshot.slots[0].color = s.slot0Color;
            snapshot.slots[0].volume = s.slot0Volume;
            snapshot.slots[0].muteVolume = s.slot0MuteVolume;
            snapshot.slots[0].darkMode = s.slot0DarkMode;
            snapshot.slots[0].muteToZero = s.slot0MuteToZero;
            snapshot.slots[0].alertPersist = s.slot0AlertPersist;
            snapshot.slots[0].priorityArrowOnly = s.slot0PriorityArrow;

            snapshot.slots[1].name = s.slot1Name;
            snapshot.slots[1].profile = s.slot1_highway.profileName;
            snapshot.slots[1].mode = s.slot1_highway.mode;
            snapshot.slots[1].color = s.slot1Color;
            snapshot.slots[1].volume = s.slot1Volume;
            snapshot.slots[1].muteVolume = s.slot1MuteVolume;
            snapshot.slots[1].darkMode = s.slot1DarkMode;
            snapshot.slots[1].muteToZero = s.slot1MuteToZero;
            snapshot.slots[1].alertPersist = s.slot1AlertPersist;
            snapshot.slots[1].priorityArrowOnly = s.slot1PriorityArrow;

            snapshot.slots[2].name = s.slot2Name;
            snapshot.slots[2].profile = s.slot2_comfort.profileName;
            snapshot.slots[2].mode = s.slot2_comfort.mode;
            snapshot.slots[2].color = s.slot2Color;
            snapshot.slots[2].volume = s.slot2Volume;
            snapshot.slots[2].muteVolume = s.slot2MuteVolume;
            snapshot.slots[2].darkMode = s.slot2DarkMode;
            snapshot.slots[2].muteToZero = s.slot2MuteToZero;
            snapshot.slots[2].alertPersist = s.slot2AlertPersist;
            snapshot.slots[2].priorityArrowOnly = s.slot2PriorityArrow;
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
            if (!bleClient.isConnected()) {
                return WifiAutoPushApiService::PushNowQueueResult::V1_NOT_CONNECTED;
            }

            if (pushNowState.step != PushNowStep::IDLE) {
                return WifiAutoPushApiService::PushNowQueueResult::ALREADY_IN_PROGRESS;
            }

            String profileName;
            V1Mode mode = V1_MODE_UNKNOWN;

            if (request.hasProfileOverride) {
                profileName = sanitizeProfileNameValue(request.profileName);
                if (request.hasModeOverride) {
                    mode = normalizeV1ModeValue(request.mode);
                }
            } else {
                const V1Settings& s = settingsManager.get();
                AutoPushSlot pushSlot;

                switch (request.slot) {
                    case 0: pushSlot = s.slot0_default; break;
                    case 1: pushSlot = s.slot1_highway; break;
                    case 2: pushSlot = s.slot2_comfort; break;
                    default: break;
                }

                profileName = sanitizeProfileNameValue(pushSlot.profileName);
                mode = normalizeV1ModeValue(static_cast<int>(pushSlot.mode));
            }

            if (profileName.length() == 0) {
                return WifiAutoPushApiService::PushNowQueueResult::NO_PROFILE_CONFIGURED;
            }

            V1Profile profile;
            if (!v1ProfileManager.loadProfile(profileName, profile)) {
                return WifiAutoPushApiService::PushNowQueueResult::PROFILE_LOAD_FAILED;
            }

            bool slotDarkMode = settingsManager.getSlotDarkMode(request.slot);
            uint8_t mainVol = settingsManager.getSlotVolume(request.slot);
            uint8_t muteVol = settingsManager.getSlotMuteVolume(request.slot);

            Serial.printf("[PushNow] Slot %d volumes - main: %d, mute: %d\n",
                          request.slot,
                          mainVol,
                          muteVol);

            settingsManager.setActiveSlot(request.slot);
            display.drawProfileIndicator(request.slot);

            pushNowState.slot = request.slot;
            memcpy(pushNowState.profileBytes,
                   profile.settings.bytes,
                   sizeof(pushNowState.profileBytes));
            pushNowState.displayOn = !slotDarkMode;  // Dark mode=true => display off
            pushNowState.applyMode = (mode != V1_MODE_UNKNOWN);
            pushNowState.mode = mode;
            pushNowState.applyVolume = (mainVol != 0xFF && muteVol != 0xFF);
            pushNowState.mainVol = mainVol;
            pushNowState.muteVol = muteVol;
            pushNowState.retries = 0;
            pushNowState.step = PushNowStep::WRITE_PROFILE;
            pushNowState.nextAtMs = millis();

            Serial.printf("[PushNow] Queued slot=%d profile='%s' mode=%d displayOn=%d volume=%s\n",
                          request.slot,
                          profileName.c_str(),
                          static_cast<int>(mode),
                          pushNowState.displayOn ? 1 : 0,
                          pushNowState.applyVolume ? "set" : "skip");

            return WifiAutoPushApiService::PushNowQueueResult::QUEUED;
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
        [this]() {
            obdHandler.stopScan();
        },
        [this]() {
            obdHandler.disconnect();
        },
        [this](bool enabled) {
            gpsRuntimeModule.setEnabled(enabled);
        },
        [this](bool enabled) {
            speedSourceSelector.setGpsEnabled(enabled);
        },
        [this](uint8_t brightness) {
            display.setBrightness(brightness);
        },
        [](uint8_t volume) {
            audio_set_volume(volume);
        },
        [this]() {
            display.showDemo();
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
        [this]() {
            settingsManager.save();
        },
    };
}

WifiTimeApiService::TimeRuntime WiFiManager::makeTimeRuntime() {
    return WifiTimeApiService::TimeRuntime{
        [this]() { return timeService.timeValid(); },
        [this]() { return timeService.nowEpochMsOr0(); },
        [this]() { return timeService.tzOffsetMinutes(); },
        [this]() { return timeService.timeSource(); },
        [this](int64_t epochMs, int32_t tzOffsetMin, uint8_t source) {
            timeService.setEpochBaseMs(epochMs, tzOffsetMin, static_cast<TimeService::Source>(source));
        },
        [this]() { return timeService.timeConfidence(); },
        [this]() { return timeService.nowMonoMs(); },
        [this]() { return timeService.epochAgeMsOr0(); },
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
        [this](uint8_t brightness) {
            settingsManager.updateBrightness(brightness);
        },
        [this](DisplayStyle style) {
            settingsManager.updateDisplayStyle(style);
        },
        [this]() {
            display.forceNextRedraw();
        },
        [this](bool enabled) {
            obdHandler.setVwDataEnabled(enabled);
        },
        [this]() {
            obdHandler.stopScan();
        },
        [this]() {
            obdHandler.disconnect();
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
        [this]() {
            settingsManager.save();
        },
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
        [this]() { settingsManager.clearWifiClientCredentials(); },
        [this](bool enabled) { settingsManager.setWifiClientEnabled(enabled); },
        [this]() { return settingsManager.getWifiClientPassword(); },
        [this]() { wifiClientState = WIFI_CLIENT_DISABLED; },
        [this]() { wifiClientState = WIFI_CLIENT_DISCONNECTED; },
        []() { WiFi.mode(WIFI_AP); },
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
        [this]() { settingsManager.backupToSD(); },
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

#include "backup_payload_builder.h"
#include "../include/settings_internals.h"

#include <ArduinoJson.h>
#include <cstring>

namespace BackupPayloadBuilder {

namespace {

constexpr const char* HTTP_BACKUP_TYPE = "v1simple_backup";
constexpr const char* SD_BACKUP_TYPE = "v1simple_sd_backup";

void appendProfile(JsonArray profilesArr, const V1Profile& profile) {
    JsonObject p = profilesArr.add<JsonObject>();
    p["name"] = profile.name;
    p["description"] = profile.description;
    p["displayOn"] = profile.displayOn;
    p["mainVolume"] = profile.mainVolume;
    p["mutedVolume"] = profile.mutedVolume;

    JsonArray bytes = p["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(profile.settings.bytes[i]);
    }
}

}  // namespace

const char* backupTypeForTransport(BackupTransport transport) {
    return transport == BackupTransport::SdBackup ? SD_BACKUP_TYPE : HTTP_BACKUP_TYPE;
}

bool isRecognizedBackupType(const char* type) {
    if (type == nullptr) {
        return false;
    }
    return strcmp(type, HTTP_BACKUP_TYPE) == 0 || strcmp(type, SD_BACKUP_TYPE) == 0;
}

BuildResult buildBackupDocument(JsonDocument& doc,
                                const V1Settings& settings,
                                const V1ProfileManager& profileManager,
                                BackupTransport transport,
                                uint32_t snapshotMs) {
    doc.clear();

    doc["_type"] = backupTypeForTransport(transport);
    doc["_version"] = SD_BACKUP_VERSION;
    doc["_timestamp"] = snapshotMs;
    doc["timestamp"] = snapshotMs;

    doc["enableWifi"] = settings.enableWifi;
    doc["wifiMode"] = static_cast<int>(settings.wifiMode);
    doc["apSSID"] = settings.apSSID;
    doc["wifiClientEnabled"] = settings.wifiClientEnabled;
    doc["wifiClientSSID"] = settings.wifiClientSSID;
    doc["proxyBLE"] = settings.proxyBLE;
    doc["proxyName"] = settings.proxyName;
    doc["lastV1Address"] = settings.lastV1Address;
    doc["autoPowerOffMinutes"] = settings.autoPowerOffMinutes;
    doc["apTimeoutMinutes"] = settings.apTimeoutMinutes;

    // OBD settings
    doc["obdEnabled"] = settings.obdEnabled;
    doc["obdSavedAddress"] = settings.obdSavedAddress;
    doc["obdSavedName"] = settings.obdSavedName;
    doc["obdSavedAddrType"] = settings.obdSavedAddrType;
    doc["obdMinRssi"] = settings.obdMinRssi;

    // ALP settings
    doc["alpEnabled"] = settings.alpEnabled;

    doc["brightness"] = settings.brightness;
    doc["turnOffDisplay"] = settings.turnOffDisplay;
    doc["displayStyle"] = static_cast<int>(settings.displayStyle);

    doc["colorBogey"] = settings.colorBogey;
    doc["colorFrequency"] = settings.colorFrequency;
    doc["colorArrowFront"] = settings.colorArrowFront;
    doc["colorArrowSide"] = settings.colorArrowSide;
    doc["colorArrowRear"] = settings.colorArrowRear;
    doc["colorBandL"] = settings.colorBandL;
    doc["colorBandKa"] = settings.colorBandKa;
    doc["colorBandK"] = settings.colorBandK;
    doc["colorBandX"] = settings.colorBandX;
    doc["colorBandPhoto"] = settings.colorBandPhoto;
    doc["colorWiFiIcon"] = settings.colorWiFiIcon;
    doc["colorWiFiConnected"] = settings.colorWiFiConnected;
    doc["colorBleConnected"] = settings.colorBleConnected;
    doc["colorBleDisconnected"] = settings.colorBleDisconnected;
    doc["colorBar1"] = settings.colorBar1;
    doc["colorBar2"] = settings.colorBar2;
    doc["colorBar3"] = settings.colorBar3;
    doc["colorBar4"] = settings.colorBar4;
    doc["colorBar5"] = settings.colorBar5;
    doc["colorBar6"] = settings.colorBar6;
    doc["colorMuted"] = settings.colorMuted;
    doc["colorPersisted"] = settings.colorPersisted;
    doc["colorVolumeMain"] = settings.colorVolumeMain;
    doc["colorVolumeMute"] = settings.colorVolumeMute;
    doc["colorRssiV1"] = settings.colorRssiV1;
    doc["colorRssiProxy"] = settings.colorRssiProxy;
    doc["colorObd"] = settings.colorObd;
    doc["colorAlpConnected"] = settings.colorAlpConnected;
    doc["colorAlpScan"] = settings.colorAlpScan;
    doc["colorAlpArmed"] = settings.colorAlpArmed;
    doc["freqUseBandColor"] = settings.freqUseBandColor;

    doc["hideWifiIcon"] = settings.hideWifiIcon;
    doc["hideProfileIndicator"] = settings.hideProfileIndicator;
    doc["hideBatteryIcon"] = settings.hideBatteryIcon;
    doc["showBatteryPercent"] = settings.showBatteryPercent;
    doc["hideBleIcon"] = settings.hideBleIcon;
    doc["hideVolumeIndicator"] = settings.hideVolumeIndicator;
    doc["hideRssiIndicator"] = settings.hideRssiIndicator;
    doc["enableWifiAtBoot"] = settings.enableWifiAtBoot;

    doc["voiceAlertMode"] = static_cast<int>(settings.voiceAlertMode);
    doc["voiceDirectionEnabled"] = settings.voiceDirectionEnabled;
    doc["announceBogeyCount"] = settings.announceBogeyCount;
    doc["muteVoiceIfVolZero"] = settings.muteVoiceIfVolZero;
    doc["voiceVolume"] = settings.voiceVolume;
    doc["announceSecondaryAlerts"] = settings.announceSecondaryAlerts;
    doc["secondaryLaser"] = settings.secondaryLaser;
    doc["secondaryKa"] = settings.secondaryKa;
    doc["secondaryK"] = settings.secondaryK;
    doc["secondaryX"] = settings.secondaryX;
    doc["alertVolumeFadeEnabled"] = settings.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = settings.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = settings.alertVolumeFadeVolume;
    doc["speedMuteEnabled"] = settings.speedMuteEnabled;
    doc["speedMuteThresholdMph"] = settings.speedMuteThresholdMph;
    doc["speedMuteHysteresisMph"] = settings.speedMuteHysteresisMph;
    doc["speedMuteVolume"] = settings.speedMuteVolume;

    doc["autoPushEnabled"] = settings.autoPushEnabled;
    doc["activeSlot"] = settings.activeSlot;
    doc["slot0Name"] = settings.slot0Name;
    doc["slot0Color"] = settings.slot0Color;
    doc["slot0Volume"] = settings.slot0Volume;
    doc["slot0MuteVolume"] = settings.slot0MuteVolume;
    doc["slot0DarkMode"] = settings.slot0DarkMode;
    doc["slot0MuteToZero"] = settings.slot0MuteToZero;
    doc["slot0AlertPersist"] = settings.slot0AlertPersist;
    doc["slot0PriorityArrow"] = settings.slot0PriorityArrow;
    doc["slot0ProfileName"] = settings.slot0_default.profileName;
    doc["slot0Mode"] = settings.slot0_default.mode;

    doc["slot1Name"] = settings.slot1Name;
    doc["slot1Color"] = settings.slot1Color;
    doc["slot1Volume"] = settings.slot1Volume;
    doc["slot1MuteVolume"] = settings.slot1MuteVolume;
    doc["slot1DarkMode"] = settings.slot1DarkMode;
    doc["slot1MuteToZero"] = settings.slot1MuteToZero;
    doc["slot1AlertPersist"] = settings.slot1AlertPersist;
    doc["slot1PriorityArrow"] = settings.slot1PriorityArrow;
    doc["slot1ProfileName"] = settings.slot1_highway.profileName;
    doc["slot1Mode"] = settings.slot1_highway.mode;

    doc["slot2Name"] = settings.slot2Name;
    doc["slot2Color"] = settings.slot2Color;
    doc["slot2Volume"] = settings.slot2Volume;
    doc["slot2MuteVolume"] = settings.slot2MuteVolume;
    doc["slot2DarkMode"] = settings.slot2DarkMode;
    doc["slot2MuteToZero"] = settings.slot2MuteToZero;
    doc["slot2AlertPersist"] = settings.slot2AlertPersist;
    doc["slot2PriorityArrow"] = settings.slot2PriorityArrow;
    doc["slot2ProfileName"] = settings.slot2_comfort.profileName;
    doc["slot2Mode"] = settings.slot2_comfort.mode;

    JsonArray profilesArr = doc["profiles"].to<JsonArray>();
    BuildResult result;
    if (profileManager.isReady()) {
        std::vector<String> profileNames = profileManager.listProfiles();
        for (const String& name : profileNames) {
            V1Profile profile;
            if (!profileManager.loadProfile(name, profile)) {
                continue;
            }
            appendProfile(profilesArr, profile);
            result.profilesBackedUp++;
        }
    }

    return result;
}

}  // namespace BackupPayloadBuilder

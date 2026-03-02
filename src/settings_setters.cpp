/**
 * Settings property setters, slot getters/setters, and reset.
 * Extracted from settings.cpp to reduce file size.
 */

#include "settings_internals.h"

// --- Simple property setters ---

void SettingsManager::setWiFiEnabled(bool enabled) {
    settings.enableWifi = enabled;
    save();
}

void SettingsManager::setAPCredentials(const String& ssid, const String& password) {
    settings.apSSID = sanitizeApSsidValue(ssid);
    settings.apPassword = sanitizeApPasswordValue(password);
    save();
}

void SettingsManager::setProxyBLE(bool enabled) {
    settings.proxyBLE = enabled;
    save();
}

void SettingsManager::setProxyName(const String& name) {
    settings.proxyName = sanitizeProxyNameValue(name);
    save();
}

void SettingsManager::setObdEnabled(bool enabled) {
    if (settings.obdEnabled == enabled) {
        return;
    }
    settings.obdEnabled = enabled;
    save();
}

void SettingsManager::setObdVwDataEnabled(bool enabled) {
    if (settings.obdVwDataEnabled == enabled) {
        return;
    }
    settings.obdVwDataEnabled = enabled;
    save();
}

void SettingsManager::setGpsEnabled(bool enabled) {
    if (settings.gpsEnabled == enabled) {
        return;
    }
    settings.gpsEnabled = enabled;
    save();
}

void SettingsManager::setAutoPowerOffMinutes(uint8_t minutes) {
    settings.autoPowerOffMinutes = clampU8(minutes, 0, 60);
    save();
}

void SettingsManager::setApTimeoutMinutes(uint8_t minutes) {
    settings.apTimeoutMinutes = clampApTimeoutValue(minutes);
    save();
}

void SettingsManager::setBrightness(uint8_t brightness) {
    settings.brightness = brightness;
    save();
}

void SettingsManager::setDisplayOff(bool off) {
    settings.turnOffDisplay = off;
    save();
}

void SettingsManager::setAutoPushEnabled(bool enabled) {
    settings.autoPushEnabled = enabled;
    save();
}

void SettingsManager::setActiveSlot(int slot) {
    settings.activeSlot = std::max(0, std::min(2, slot));
    save();
}

void SettingsManager::setSlot(int slotNum, const String& profileName, V1Mode mode) {
    const String safeProfileName = sanitizeProfileNameValue(profileName);
    const V1Mode safeMode = normalizeV1ModeValue(static_cast<int>(mode));
    switch (slotNum) {
        case 0:
            settings.slot0_default.profileName = safeProfileName;
            settings.slot0_default.mode = safeMode;
            break;
        case 1:
            settings.slot1_highway.profileName = safeProfileName;
            settings.slot1_highway.mode = safeMode;
            break;
        case 2:
            settings.slot2_comfort.profileName = safeProfileName;
            settings.slot2_comfort.mode = safeMode;
            break;
    }
    save();
}

void SettingsManager::setSlotName(int slotNum, const String& name) {
    String upperName = sanitizeSlotNameValue(name);
    
    switch (slotNum) {
        case 0: settings.slot0Name = upperName; break;
        case 1: settings.slot1Name = upperName; break;
        case 2: settings.slot2Name = upperName; break;
    }
    save();
}

void SettingsManager::setSlotColor(int slotNum, uint16_t color) {
    switch (slotNum) {
        case 0: settings.slot0Color = color; break;
        case 1: settings.slot1Color = color; break;
        case 2: settings.slot2Color = color; break;
    }
    save();
}

void SettingsManager::setSlotVolumes(int slotNum, uint8_t volume, uint8_t muteVolume) {
    uint8_t safeVolume = clampSlotVolumeValue(volume);
    uint8_t safeMuteVolume = clampSlotVolumeValue(muteVolume);
    switch (slotNum) {
        case 0: settings.slot0Volume = safeVolume; settings.slot0MuteVolume = safeMuteVolume; break;
        case 1: settings.slot1Volume = safeVolume; settings.slot1MuteVolume = safeMuteVolume; break;
        case 2: settings.slot2Volume = safeVolume; settings.slot2MuteVolume = safeMuteVolume; break;
    }
    save();
}

void SettingsManager::setDisplayColors(uint16_t bogey, uint16_t freq, uint16_t arrowFront, uint16_t arrowSide, uint16_t arrowRear,
                                        uint16_t bandL, uint16_t bandKa, uint16_t bandK, uint16_t bandX, bool deferSave) {
    settings.colorBogey = bogey;
    settings.colorFrequency = freq;
    settings.colorArrowFront = arrowFront;
    settings.colorArrowSide = arrowSide;
    settings.colorArrowRear = arrowRear;
    settings.colorBandL = bandL;
    settings.colorBandKa = bandKa;
    settings.colorBandK = bandK;
    settings.colorBandX = bandX;
    if (!deferSave) save();
}

void SettingsManager::setWiFiIconColors(uint16_t icon, uint16_t connected) {
    settings.colorWiFiIcon = icon;
    settings.colorWiFiConnected = connected;
    save();
}

void SettingsManager::setBleIconColors(uint16_t connected, uint16_t disconnected) {
    settings.colorBleConnected = connected;
    settings.colorBleDisconnected = disconnected;
    save();
}

void SettingsManager::setSignalBarColors(uint16_t bar1, uint16_t bar2, uint16_t bar3, uint16_t bar4, uint16_t bar5, uint16_t bar6) {
    settings.colorBar1 = bar1;
    settings.colorBar2 = bar2;
    settings.colorBar3 = bar3;
    settings.colorBar4 = bar4;
    settings.colorBar5 = bar5;
    settings.colorBar6 = bar6;
    save();
}

void SettingsManager::setMutedColor(uint16_t color) {
    settings.colorMuted = color;
    save();
}

void SettingsManager::setBandPhotoColor(uint16_t color) {
    settings.colorBandPhoto = color;
    save();
}

void SettingsManager::setPersistedColor(uint16_t color) {
    settings.colorPersisted = color;
    save();
}

void SettingsManager::setVolumeMainColor(uint16_t color) {
    settings.colorVolumeMain = color;
    save();
}

void SettingsManager::setVolumeMuteColor(uint16_t color) {
    settings.colorVolumeMute = color;
    save();
}

void SettingsManager::setRssiV1Color(uint16_t color) {
    settings.colorRssiV1 = color;
    save();
}

void SettingsManager::setRssiProxyColor(uint16_t color) {
    settings.colorRssiProxy = color;
    save();
}

void SettingsManager::setFreqUseBandColor(bool use) {
    settings.freqUseBandColor = use;
    save();
}

void SettingsManager::setHideWifiIcon(bool hide) {
    settings.hideWifiIcon = hide;
    save();
}

void SettingsManager::setHideProfileIndicator(bool hide) {
    settings.hideProfileIndicator = hide;
    save();
}

void SettingsManager::setHideBatteryIcon(bool hide) {
    settings.hideBatteryIcon = hide;
    save();
}

void SettingsManager::setShowBatteryPercent(bool show) {
    settings.showBatteryPercent = show;
    save();
}

void SettingsManager::setHideBleIcon(bool hide) {
    settings.hideBleIcon = hide;
    save();
}

void SettingsManager::setHideVolumeIndicator(bool hide) {
    settings.hideVolumeIndicator = hide;
    save();
}

void SettingsManager::setHideRssiIndicator(bool hide) {
    settings.hideRssiIndicator = hide;
    save();
}

void SettingsManager::setShowRestTelemetryCards(bool show) {
    settings.showRestTelemetryCards = show;
    save();
}

void SettingsManager::setEnableWifiAtBoot(bool enable, bool deferSave) {
    settings.enableWifiAtBoot = enable;
    if (!deferSave) save();
}

void SettingsManager::setVoiceAlertMode(VoiceAlertMode mode) {
    settings.voiceAlertMode = clampVoiceAlertModeValue(static_cast<int>(mode));
    save();
}

void SettingsManager::setVoiceDirectionEnabled(bool enabled) {
    settings.voiceDirectionEnabled = enabled;
    save();
}

void SettingsManager::setAnnounceBogeyCount(bool enabled) {
    settings.announceBogeyCount = enabled;
    save();
}

void SettingsManager::setMuteVoiceIfVolZero(bool mute) {
    settings.muteVoiceIfVolZero = mute;
    save();
}

void SettingsManager::setAnnounceSecondaryAlerts(bool enabled) {
    settings.announceSecondaryAlerts = enabled;
    save();
}

void SettingsManager::setSecondaryLaser(bool enabled) {
    settings.secondaryLaser = enabled;
    save();
}

void SettingsManager::setSecondaryKa(bool enabled) {
    settings.secondaryKa = enabled;
    save();
}

void SettingsManager::setSecondaryK(bool enabled) {
    settings.secondaryK = enabled;
    save();
}

void SettingsManager::setSecondaryX(bool enabled) {
    settings.secondaryX = enabled;
    save();
}

void SettingsManager::setAlertVolumeFade(bool enabled, uint8_t delaySec, uint8_t volume) {
    settings.alertVolumeFadeEnabled = enabled;
    settings.alertVolumeFadeDelaySec = clampU8(delaySec, 1, 10);
    settings.alertVolumeFadeVolume = clampU8(volume, 0, 9);
    save();
}


const AutoPushSlot& SettingsManager::getActiveSlot() const {
    switch (settings.activeSlot) {
        case 1: return settings.slot1_highway;
        case 2: return settings.slot2_comfort;
        default: return settings.slot0_default;
    }
}

const AutoPushSlot& SettingsManager::getSlot(int slotNum) const {
    switch (slotNum) {
        case 1: return settings.slot1_highway;
        case 2: return settings.slot2_comfort;
        default: return settings.slot0_default;
    }
}

uint8_t SettingsManager::getSlotVolume(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0Volume;
        case 1: return settings.slot1Volume;
        case 2: return settings.slot2Volume;
        default: return 0xFF;
    }
}

uint8_t SettingsManager::getSlotMuteVolume(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0MuteVolume;
        case 1: return settings.slot1MuteVolume;
        case 2: return settings.slot2MuteVolume;
        default: return 0xFF;
    }
}

bool SettingsManager::getSlotDarkMode(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0DarkMode;
        case 1: return settings.slot1DarkMode;
        case 2: return settings.slot2DarkMode;
        default: return false;
    }
}

bool SettingsManager::getSlotMuteToZero(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0MuteToZero;
        case 1: return settings.slot1MuteToZero;
        case 2: return settings.slot2MuteToZero;
        default: return false;
    }
}

uint8_t SettingsManager::getSlotAlertPersistSec(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0AlertPersist;
        case 1: return settings.slot1AlertPersist;
        case 2: return settings.slot2AlertPersist;
        default: return 0;
    }
}

void SettingsManager::setSlotDarkMode(int slotNum, bool darkMode) {
    switch (slotNum) {
        case 0: settings.slot0DarkMode = darkMode; break;
        case 1: settings.slot1DarkMode = darkMode; break;
        case 2: settings.slot2DarkMode = darkMode; break;
    }
    save();
}

void SettingsManager::setSlotMuteToZero(int slotNum, bool mz) {
    switch (slotNum) {
        case 0: settings.slot0MuteToZero = mz; break;
        case 1: settings.slot1MuteToZero = mz; break;
        case 2: settings.slot2MuteToZero = mz; break;
    }
    save();
}

void SettingsManager::setSlotAlertPersistSec(int slotNum, uint8_t seconds) {
    uint8_t clamped = std::min<uint8_t>(5, seconds);
    switch (slotNum) {
        case 0: settings.slot0AlertPersist = clamped; break;
        case 1: settings.slot1AlertPersist = clamped; break;
        case 2: settings.slot2AlertPersist = clamped; break;
        default: return;
    }
    save();
}

bool SettingsManager::getSlotPriorityArrowOnly(int slotNum) const {
    switch (slotNum) {
        case 0: return settings.slot0PriorityArrow;
        case 1: return settings.slot1PriorityArrow;
        case 2: return settings.slot2PriorityArrow;
        default: return false;
    }
}

void SettingsManager::setSlotPriorityArrowOnly(int slotNum, bool prioArrow) {
    switch (slotNum) {
        case 0: settings.slot0PriorityArrow = prioArrow; break;
        case 1: settings.slot1PriorityArrow = prioArrow; break;
        case 2: settings.slot2PriorityArrow = prioArrow; break;
    }
    save();
}

void SettingsManager::setLastV1Address(const String& addr) {
    String safeAddr = sanitizeLastV1AddressValue(addr);
    if (safeAddr != settings.lastV1Address) {
        settings.lastV1Address = safeAddr;
        save();
        Serial.printf("Saved new V1 address: %s\n", safeAddr.c_str());
    }
}

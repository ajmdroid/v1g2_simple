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
    AutoPushSlot& slot = settings.autoPushSlotView(slotNum).config;
    slot.profileName = safeProfileName;
    slot.mode = safeMode;
    save();
}

void SettingsManager::setSlotName(int slotNum, const String& name) {
    String upperName = sanitizeSlotNameValue(name);

    settings.autoPushSlotView(slotNum).name = upperName;
    save();
}

void SettingsManager::setSlotColor(int slotNum, uint16_t color) {
    settings.autoPushSlotView(slotNum).color = color;
    save();
}

void SettingsManager::setSlotVolumes(int slotNum, uint8_t volume, uint8_t muteVolume) {
    uint8_t safeVolume = clampSlotVolumeValue(volume);
    uint8_t safeMuteVolume = clampSlotVolumeValue(muteVolume);
    V1Settings::AutoPushSlotView slot = settings.autoPushSlotView(slotNum);
    slot.volume = safeVolume;
    slot.muteVolume = safeMuteVolume;
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
    return settings.autoPushSlotView(settings.activeSlot).config;
}

const AutoPushSlot& SettingsManager::getSlot(int slotNum) const {
    return settings.autoPushSlotView(slotNum).config;
}

uint8_t SettingsManager::getSlotVolume(int slotNum) const {
    return settings.autoPushSlotView(slotNum).volume;
}

uint8_t SettingsManager::getSlotMuteVolume(int slotNum) const {
    return settings.autoPushSlotView(slotNum).muteVolume;
}

bool SettingsManager::getSlotDarkMode(int slotNum) const {
    return settings.autoPushSlotView(slotNum).darkMode;
}

bool SettingsManager::getSlotMuteToZero(int slotNum) const {
    return settings.autoPushSlotView(slotNum).muteToZero;
}

uint8_t SettingsManager::getSlotAlertPersistSec(int slotNum) const {
    return settings.autoPushSlotView(slotNum).alertPersist;
}

void SettingsManager::setSlotDarkMode(int slotNum, bool darkMode) {
    settings.autoPushSlotView(slotNum).darkMode = darkMode;
    save();
}

void SettingsManager::setSlotMuteToZero(int slotNum, bool mz) {
    settings.autoPushSlotView(slotNum).muteToZero = mz;
    save();
}

void SettingsManager::setSlotAlertPersistSec(int slotNum, uint8_t seconds) {
    uint8_t clamped = std::min<uint8_t>(5, seconds);
    settings.autoPushSlotView(slotNum).alertPersist = clamped;
    save();
}

bool SettingsManager::getSlotPriorityArrowOnly(int slotNum) const {
    return settings.autoPushSlotView(slotNum).priorityArrow;
}

void SettingsManager::setSlotPriorityArrowOnly(int slotNum, bool prioArrow) {
    settings.autoPushSlotView(slotNum).priorityArrow = prioArrow;
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

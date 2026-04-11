/**
 * Settings property setters, slot getters/setters, and reset.
 * Extracted from settings_.cpp to reduce file size.
 */

#include "settings_internals.h"

namespace {

template <typename T>
bool assignIfChanged(T& target, const T& value) {
    if (target == value) {
        return false;
    }
    target = value;
    return true;
}

void persistSettingsByMode(SettingsManager& manager, SettingsPersistMode persistMode) {
    if (persistMode == SettingsPersistMode::Deferred) {
        manager.requestDeferredPersist();
        return;
    }
    manager.save();
}

}  // namespace

// --- Simple property setters ---

void SettingsManager::setWiFiEnabled(bool enabled) {
    settings_.enableWifi = enabled;
    save();
}

void SettingsManager::setAPCredentials(const String& ssid, const String& password) {
    settings_.apSSID = sanitizeApSsidValue(ssid);
    settings_.apPassword = sanitizeApPasswordValue(password);
    save();
}

void SettingsManager::setProxyBLE(bool enabled) {
    settings_.proxyBLE = enabled;
    save();
}

void SettingsManager::setProxyName(const String& name) {
    settings_.proxyName = sanitizeProxyNameValue(name);
    save();
}

void SettingsManager::setAutoPowerOffMinutes(uint8_t minutes) {
    settings_.autoPowerOffMinutes = clampU8(minutes, 0, 60);
    save();
}

void SettingsManager::setApTimeoutMinutes(uint8_t minutes) {
    settings_.apTimeoutMinutes = clampApTimeoutValue(minutes);
    save();
}

void SettingsManager::setBrightness(uint8_t brightness) {
    settings_.brightness = brightness;
    save();
}

void SettingsManager::setDisplayOff(bool off) {
    settings_.turnOffDisplay = off;
    save();
}

void SettingsManager::setAutoPushEnabled(bool enabled) {
    settings_.autoPushEnabled = enabled;
    save();
}

void SettingsManager::setActiveSlot(int slot) {
    settings_.activeSlot = std::max(0, std::min(2, slot));
    save();
}

void SettingsManager::setSlot(int slotNum, const String& profileName, V1Mode mode) {
    const String safeProfileName = sanitizeProfileNameValue(profileName);
    const V1Mode safeMode = normalizeV1ModeValue(static_cast<int>(mode));
    AutoPushSlot& slot = settings_.autoPushSlotView(slotNum).config;
    slot.profileName = safeProfileName;
    slot.mode = safeMode;
    save();
}

void SettingsManager::setSlotName(int slotNum, const String& name) {
    String upperName = sanitizeSlotNameValue(name);

    settings_.autoPushSlotView(slotNum).name = upperName;
    save();
}

void SettingsManager::setSlotColor(int slotNum, uint16_t color) {
    settings_.autoPushSlotView(slotNum).color = color;
    save();
}

void SettingsManager::setSlotVolumes(int slotNum, uint8_t volume, uint8_t muteVolume) {
    uint8_t safeVolume = clampSlotVolumeValue(volume);
    uint8_t safeMuteVolume = clampSlotVolumeValue(muteVolume);
    V1Settings::AutoPushSlotView slot = settings_.autoPushSlotView(slotNum);
    slot.volume = safeVolume;
    slot.muteVolume = safeMuteVolume;
    save();
}

void SettingsManager::setDisplayColors(uint16_t bogey, uint16_t freq, uint16_t arrowFront, uint16_t arrowSide, uint16_t arrowRear,
                                        uint16_t bandL, uint16_t bandKa, uint16_t bandK, uint16_t bandX, bool deferSave) {
    settings_.colorBogey = bogey;
    settings_.colorFrequency = freq;
    settings_.colorArrowFront = arrowFront;
    settings_.colorArrowSide = arrowSide;
    settings_.colorArrowRear = arrowRear;
    settings_.colorBandL = bandL;
    settings_.colorBandKa = bandKa;
    settings_.colorBandK = bandK;
    settings_.colorBandX = bandX;
    if (!deferSave) save();
}

void SettingsManager::setWiFiIconColors(uint16_t icon, uint16_t connected) {
    settings_.colorWiFiIcon = icon;
    settings_.colorWiFiConnected = connected;
    save();
}

void SettingsManager::setBleIconColors(uint16_t connected, uint16_t disconnected) {
    settings_.colorBleConnected = connected;
    settings_.colorBleDisconnected = disconnected;
    save();
}

void SettingsManager::setSignalBarColors(uint16_t bar1, uint16_t bar2, uint16_t bar3, uint16_t bar4, uint16_t bar5, uint16_t bar6) {
    settings_.colorBar1 = bar1;
    settings_.colorBar2 = bar2;
    settings_.colorBar3 = bar3;
    settings_.colorBar4 = bar4;
    settings_.colorBar5 = bar5;
    settings_.colorBar6 = bar6;
    save();
}

void SettingsManager::setMutedColor(uint16_t color) {
    settings_.colorMuted = color;
    save();
}

void SettingsManager::setBandPhotoColor(uint16_t color) {
    settings_.colorBandPhoto = color;
    save();
}

void SettingsManager::setPersistedColor(uint16_t color) {
    settings_.colorPersisted = color;
    save();
}

void SettingsManager::setVolumeMainColor(uint16_t color) {
    settings_.colorVolumeMain = color;
    save();
}

void SettingsManager::setVolumeMuteColor(uint16_t color) {
    settings_.colorVolumeMute = color;
    save();
}

void SettingsManager::setRssiV1Color(uint16_t color) {
    settings_.colorRssiV1 = color;
    save();
}

void SettingsManager::setRssiProxyColor(uint16_t color) {
    settings_.colorRssiProxy = color;
    save();
}

void SettingsManager::setFreqUseBandColor(bool use) {
    settings_.freqUseBandColor = use;
    save();
}

void SettingsManager::setHideWifiIcon(bool hide) {
    settings_.hideWifiIcon = hide;
    save();
}

void SettingsManager::setHideProfileIndicator(bool hide) {
    settings_.hideProfileIndicator = hide;
    save();
}

void SettingsManager::setHideBatteryIcon(bool hide) {
    settings_.hideBatteryIcon = hide;
    save();
}

void SettingsManager::setShowBatteryPercent(bool show) {
    settings_.showBatteryPercent = show;
    save();
}

void SettingsManager::setHideBleIcon(bool hide) {
    settings_.hideBleIcon = hide;
    save();
}

void SettingsManager::setHideVolumeIndicator(bool hide) {
    settings_.hideVolumeIndicator = hide;
    save();
}

void SettingsManager::setHideRssiIndicator(bool hide) {
    settings_.hideRssiIndicator = hide;
    save();
}

void SettingsManager::setEnableWifiAtBoot(bool enable, bool deferSave) {
    settings_.enableWifiAtBoot = enable;
    if (!deferSave) save();
}

void SettingsManager::setVoiceAlertMode(VoiceAlertMode mode) {
    settings_.voiceAlertMode = clampVoiceAlertModeValue(static_cast<int>(mode));
    save();
}

void SettingsManager::setVoiceDirectionEnabled(bool enabled) {
    settings_.voiceDirectionEnabled = enabled;
    save();
}

void SettingsManager::setAnnounceBogeyCount(bool enabled) {
    settings_.announceBogeyCount = enabled;
    save();
}

void SettingsManager::setMuteVoiceIfVolZero(bool mute) {
    settings_.muteVoiceIfVolZero = mute;
    save();
}

void SettingsManager::setAnnounceSecondaryAlerts(bool enabled) {
    settings_.announceSecondaryAlerts = enabled;
    save();
}

void SettingsManager::setSecondaryLaser(bool enabled) {
    settings_.secondaryLaser = enabled;
    save();
}

void SettingsManager::setSecondaryKa(bool enabled) {
    settings_.secondaryKa = enabled;
    save();
}

void SettingsManager::setSecondaryK(bool enabled) {
    settings_.secondaryK = enabled;
    save();
}

void SettingsManager::setSecondaryX(bool enabled) {
    settings_.secondaryX = enabled;
    save();
}

void SettingsManager::setAlertVolumeFade(bool enabled, uint8_t delaySec, uint8_t volume) {
    settings_.alertVolumeFadeEnabled = enabled;
    settings_.alertVolumeFadeDelaySec = clampU8(delaySec, 1, 10);
    settings_.alertVolumeFadeVolume = clampU8(volume, 0, 9);
    save();
}

void SettingsManager::setSpeedMute(bool enabled, uint8_t thresholdMph,
                                   uint8_t hysteresisMph) {
    settings_.speedMuteEnabled = enabled;
    settings_.speedMuteThresholdMph = clampU8(thresholdMph, 5, 60);
    settings_.speedMuteHysteresisMph = clampU8(hysteresisMph, 1, 10);
    save();
}


const AutoPushSlot& SettingsManager::getActiveSlot() const {
    return settings_.autoPushSlotView(settings_.activeSlot).config;
}

const AutoPushSlot& SettingsManager::getSlot(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).config;
}

uint8_t SettingsManager::getSlotVolume(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).volume;
}

uint8_t SettingsManager::getSlotMuteVolume(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).muteVolume;
}

bool SettingsManager::getSlotDarkMode(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).darkMode;
}

bool SettingsManager::getSlotMuteToZero(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).muteToZero;
}

uint8_t SettingsManager::getSlotAlertPersistSec(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).alertPersist;
}

void SettingsManager::setSlotDarkMode(int slotNum, bool darkMode) {
    settings_.autoPushSlotView(slotNum).darkMode = darkMode;
    save();
}

void SettingsManager::setSlotMuteToZero(int slotNum, bool mz) {
    settings_.autoPushSlotView(slotNum).muteToZero = mz;
    save();
}

void SettingsManager::setSlotAlertPersistSec(int slotNum, uint8_t seconds) {
    uint8_t clamped = std::min<uint8_t>(5, seconds);
    settings_.autoPushSlotView(slotNum).alertPersist = clamped;
    save();
}

bool SettingsManager::getSlotPriorityArrowOnly(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).priorityArrow;
}

void SettingsManager::setSlotPriorityArrowOnly(int slotNum, bool prioArrow) {
    settings_.autoPushSlotView(slotNum).priorityArrow = prioArrow;
    save();
}

bool SettingsManager::applyAutoPushSlotUpdate(const AutoPushSlotUpdate& update,
                                              SettingsPersistMode persistMode) {
    bool changed = false;
    V1Settings::AutoPushSlotView slot = settings_.autoPushSlotView(update.slot);

    if (update.hasName) {
        changed |= assignIfChanged(slot.name, sanitizeSlotNameValue(update.name));
    }
    if (update.hasColor) {
        changed |= assignIfChanged(slot.color, update.color);
    }
    if (update.hasVolume) {
        changed |= assignIfChanged(slot.volume, clampSlotVolumeValue(update.volume));
    }
    if (update.hasMuteVolume) {
        changed |= assignIfChanged(slot.muteVolume, clampSlotVolumeValue(update.muteVolume));
    }
    if (update.hasDarkMode) {
        changed |= assignIfChanged(slot.darkMode, update.darkMode);
    }
    if (update.hasMuteToZero) {
        changed |= assignIfChanged(slot.muteToZero, update.muteToZero);
    }
    if (update.hasAlertPersist) {
        changed |= assignIfChanged(slot.alertPersist, std::min<uint8_t>(5, update.alertPersist));
    }
    if (update.hasPriorityArrowOnly) {
        changed |= assignIfChanged(slot.priorityArrow, update.priorityArrowOnly);
    }
    if (update.hasProfileName) {
        changed |= assignIfChanged(slot.config.profileName,
                                   sanitizeProfileNameValue(update.profileName));
    }
    if (update.hasMode) {
        changed |= assignIfChanged(slot.config.mode,
                                   normalizeV1ModeValue(static_cast<int>(update.mode)));
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }

    return changed;
}

bool SettingsManager::applyAutoPushStateUpdate(const AutoPushStateUpdate& update,
                                               SettingsPersistMode persistMode) {
    bool changed = false;

    if (update.hasActiveSlot) {
        changed |= assignIfChanged(settings_.activeSlot,
                                   static_cast<int>(
                                       V1Settings::normalizeAutoPushSlotIndex(update.activeSlot)));
    }
    if (update.hasEnabled) {
        changed |= assignIfChanged(settings_.autoPushEnabled, update.enabled);
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }

    return changed;
}

void SettingsManager::setLastV1Address(const String& addr) {
    String safeAddr = sanitizeLastV1AddressValue(addr);
    if (safeAddr != settings_.lastV1Address) {
        settings_.lastV1Address = safeAddr;
        requestDeferredPersist();
        Serial.printf("Deferred persist for new V1 address: %s\n", safeAddr.c_str());
    }
}

void SettingsManager::applyDeviceSettingsUpdate(const DeviceSettingsUpdate& update,
                                                SettingsPersistMode persistMode) {
    bool changed = false;

    if (update.hasApCredentials) {
        changed |= assignIfChanged(settings_.apSSID, sanitizeApSsidValue(update.apSSID));
        changed |= assignIfChanged(settings_.apPassword, sanitizeApPasswordValue(update.apPassword));
    }
    if (update.hasProxyBLE) {
        changed |= assignIfChanged(settings_.proxyBLE, update.proxyBLE);
    }
    if (update.hasProxyName) {
        changed |= assignIfChanged(settings_.proxyName, sanitizeProxyNameValue(update.proxyName));
    }
    if (update.hasAutoPowerOffMinutes) {
        changed |= assignIfChanged(settings_.autoPowerOffMinutes,
                                   clampU8(update.autoPowerOffMinutes, 0, 60));
    }
    if (update.hasApTimeoutMinutes) {
        changed |= assignIfChanged(settings_.apTimeoutMinutes,
                                   clampApTimeoutValue(update.apTimeoutMinutes));
    }
    if (update.hasEnableWifiAtBoot) {
        changed |= assignIfChanged(settings_.enableWifiAtBoot, update.enableWifiAtBoot);
    }
    if (update.hasAlpEnabled) {
        changed |= assignIfChanged(settings_.alpEnabled, update.alpEnabled);
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }
}

void SettingsManager::applyAudioSettingsUpdate(const AudioSettingsUpdate& update,
                                               SettingsPersistMode persistMode) {
    bool changed = false;

    if (update.hasVoiceAlertMode) {
        changed |= assignIfChanged(settings_.voiceAlertMode,
                                   clampVoiceAlertModeValue(static_cast<int>(update.voiceAlertMode)));
    }
    if (update.hasVoiceDirectionEnabled) {
        changed |= assignIfChanged(settings_.voiceDirectionEnabled, update.voiceDirectionEnabled);
    }
    if (update.hasAnnounceBogeyCount) {
        changed |= assignIfChanged(settings_.announceBogeyCount, update.announceBogeyCount);
    }
    if (update.hasMuteVoiceIfVolZero) {
        changed |= assignIfChanged(settings_.muteVoiceIfVolZero, update.muteVoiceIfVolZero);
    }
    if (update.hasVoiceVolume) {
        changed |= assignIfChanged(settings_.voiceVolume, clampU8(update.voiceVolume, 0, 100));
    }
    if (update.hasAnnounceSecondaryAlerts) {
        changed |= assignIfChanged(settings_.announceSecondaryAlerts, update.announceSecondaryAlerts);
    }
    if (update.hasSecondaryLaser) {
        changed |= assignIfChanged(settings_.secondaryLaser, update.secondaryLaser);
    }
    if (update.hasSecondaryKa) {
        changed |= assignIfChanged(settings_.secondaryKa, update.secondaryKa);
    }
    if (update.hasSecondaryK) {
        changed |= assignIfChanged(settings_.secondaryK, update.secondaryK);
    }
    if (update.hasSecondaryX) {
        changed |= assignIfChanged(settings_.secondaryX, update.secondaryX);
    }
    if (update.hasAlertVolumeFadeEnabled) {
        changed |= assignIfChanged(settings_.alertVolumeFadeEnabled, update.alertVolumeFadeEnabled);
    }
    if (update.hasAlertVolumeFadeDelaySec) {
        changed |= assignIfChanged(settings_.alertVolumeFadeDelaySec,
                                   clampU8(update.alertVolumeFadeDelaySec, 1, 10));
    }
    if (update.hasAlertVolumeFadeVolume) {
        changed |= assignIfChanged(settings_.alertVolumeFadeVolume,
                                   clampU8(update.alertVolumeFadeVolume, 0, 9));
    }
    if (update.hasSpeedMuteEnabled) {
        changed |= assignIfChanged(settings_.speedMuteEnabled, update.speedMuteEnabled);
    }
    if (update.hasSpeedMuteThresholdMph) {
        changed |= assignIfChanged(settings_.speedMuteThresholdMph,
                                   clampU8(update.speedMuteThresholdMph, 5, 60));
    }
    if (update.hasSpeedMuteHysteresisMph) {
        changed |= assignIfChanged(settings_.speedMuteHysteresisMph,
                                   clampU8(update.speedMuteHysteresisMph, 1, 10));
    }
    if (update.hasSpeedMuteVolume) {
        const uint8_t val = (update.speedMuteVolume <= 9) ? update.speedMuteVolume : 0xFF;
        changed |= assignIfChanged(settings_.speedMuteVolume, val);
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }
}

void SettingsManager::applyDisplaySettingsUpdate(const DisplaySettingsUpdate& update,
                                                 SettingsPersistMode persistMode) {
    bool changed = false;

    if (update.hasColorBogey) changed |= assignIfChanged(settings_.colorBogey, update.colorBogey);
    if (update.hasColorFrequency) changed |= assignIfChanged(settings_.colorFrequency, update.colorFrequency);
    if (update.hasColorArrowFront) changed |= assignIfChanged(settings_.colorArrowFront, update.colorArrowFront);
    if (update.hasColorArrowSide) changed |= assignIfChanged(settings_.colorArrowSide, update.colorArrowSide);
    if (update.hasColorArrowRear) changed |= assignIfChanged(settings_.colorArrowRear, update.colorArrowRear);
    if (update.hasColorBandL) changed |= assignIfChanged(settings_.colorBandL, update.colorBandL);
    if (update.hasColorBandKa) changed |= assignIfChanged(settings_.colorBandKa, update.colorBandKa);
    if (update.hasColorBandK) changed |= assignIfChanged(settings_.colorBandK, update.colorBandK);
    if (update.hasColorBandX) changed |= assignIfChanged(settings_.colorBandX, update.colorBandX);
    if (update.hasColorBandPhoto) changed |= assignIfChanged(settings_.colorBandPhoto, update.colorBandPhoto);
    if (update.hasColorWiFiIcon) changed |= assignIfChanged(settings_.colorWiFiIcon, update.colorWiFiIcon);
    if (update.hasColorWiFiConnected) {
        changed |= assignIfChanged(settings_.colorWiFiConnected, update.colorWiFiConnected);
    }
    if (update.hasColorBleConnected) changed |= assignIfChanged(settings_.colorBleConnected, update.colorBleConnected);
    if (update.hasColorBleDisconnected) {
        changed |= assignIfChanged(settings_.colorBleDisconnected, update.colorBleDisconnected);
    }
    if (update.hasColorBar1) changed |= assignIfChanged(settings_.colorBar1, update.colorBar1);
    if (update.hasColorBar2) changed |= assignIfChanged(settings_.colorBar2, update.colorBar2);
    if (update.hasColorBar3) changed |= assignIfChanged(settings_.colorBar3, update.colorBar3);
    if (update.hasColorBar4) changed |= assignIfChanged(settings_.colorBar4, update.colorBar4);
    if (update.hasColorBar5) changed |= assignIfChanged(settings_.colorBar5, update.colorBar5);
    if (update.hasColorBar6) changed |= assignIfChanged(settings_.colorBar6, update.colorBar6);
    if (update.hasColorMuted) changed |= assignIfChanged(settings_.colorMuted, update.colorMuted);
    if (update.hasColorPersisted) changed |= assignIfChanged(settings_.colorPersisted, update.colorPersisted);
    if (update.hasColorVolumeMain) changed |= assignIfChanged(settings_.colorVolumeMain, update.colorVolumeMain);
    if (update.hasColorVolumeMute) changed |= assignIfChanged(settings_.colorVolumeMute, update.colorVolumeMute);
    if (update.hasColorRssiV1) changed |= assignIfChanged(settings_.colorRssiV1, update.colorRssiV1);
    if (update.hasColorRssiProxy) changed |= assignIfChanged(settings_.colorRssiProxy, update.colorRssiProxy);
    if (update.hasColorObd) changed |= assignIfChanged(settings_.colorObd, update.colorObd);
    if (update.hasColorAlpConnected) changed |= assignIfChanged(settings_.colorAlpConnected, update.colorAlpConnected);
    if (update.hasColorAlpScan) changed |= assignIfChanged(settings_.colorAlpScan, update.colorAlpScan);
    if (update.hasColorAlpArmed) changed |= assignIfChanged(settings_.colorAlpArmed, update.colorAlpArmed);
    if (update.hasFreqUseBandColor) changed |= assignIfChanged(settings_.freqUseBandColor, update.freqUseBandColor);
    if (update.hasHideWifiIcon) changed |= assignIfChanged(settings_.hideWifiIcon, update.hideWifiIcon);
    if (update.hasHideProfileIndicator) {
        changed |= assignIfChanged(settings_.hideProfileIndicator, update.hideProfileIndicator);
    }
    if (update.hasHideBatteryIcon) changed |= assignIfChanged(settings_.hideBatteryIcon, update.hideBatteryIcon);
    if (update.hasShowBatteryPercent) changed |= assignIfChanged(settings_.showBatteryPercent, update.showBatteryPercent);
    if (update.hasHideBleIcon) changed |= assignIfChanged(settings_.hideBleIcon, update.hideBleIcon);
    if (update.hasHideVolumeIndicator) {
        changed |= assignIfChanged(settings_.hideVolumeIndicator, update.hideVolumeIndicator);
    }
    if (update.hasHideRssiIndicator) changed |= assignIfChanged(settings_.hideRssiIndicator, update.hideRssiIndicator);
    if (update.hasBrightness) changed |= assignIfChanged(settings_.brightness, update.brightness);
    if (update.hasDisplayStyle) {
        changed |= assignIfChanged(settings_.displayStyle,
                                   normalizeDisplayStyle(static_cast<int>(update.displayStyle)));
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }
}

void SettingsManager::resetDisplaySettings(SettingsPersistMode persistMode) {
    settings_.colorBogey = 0xF800;
    settings_.colorFrequency = 0xF800;
    settings_.colorArrowFront = 0xF800;
    settings_.colorArrowSide = 0xF800;
    settings_.colorArrowRear = 0xF800;
    settings_.colorBandL = 0x001F;
    settings_.colorBandKa = 0xF800;
    settings_.colorBandK = 0x001F;
    settings_.colorBandX = 0x07E0;
    settings_.colorBandPhoto = 0x780F;
    settings_.colorWiFiIcon = 0x07FF;
    settings_.colorWiFiConnected = 0x07E0;
    settings_.colorBleConnected = 0x07E0;
    settings_.colorBleDisconnected = 0x001F;
    settings_.colorBar1 = 0x07E0;
    settings_.colorBar2 = 0x07E0;
    settings_.colorBar3 = 0xFFE0;
    settings_.colorBar4 = 0xFFE0;
    settings_.colorBar5 = 0xF800;
    settings_.colorBar6 = 0xF800;
    settings_.colorMuted = 0x3186;
    settings_.colorPersisted = 0x18C3;
    settings_.colorVolumeMain = 0x001F;
    settings_.colorVolumeMute = 0xFFE0;
    settings_.colorRssiV1 = 0x07E0;
    settings_.colorRssiProxy = 0x001F;
    settings_.colorObd = 0x001F;
    settings_.colorAlpConnected = 0x07E0;
    settings_.colorAlpScan = 0xFD20;
    settings_.colorAlpArmed = 0x001F;
    settings_.freqUseBandColor = false;

    persistSettingsByMode(*this, persistMode);
}

bool SettingsManager::applyObdSettingsUpdate(const ObdSettingsUpdate& update,
                                             SettingsPersistMode persistMode) {
    bool changed = false;

    if (update.resetSavedNameOnAddressChange &&
        update.hasSavedAddress &&
        settings_.obdSavedAddress != update.savedAddress &&
        !update.hasSavedName) {
        changed |= assignIfChanged(settings_.obdSavedName, String(""));
    }

    if (update.hasEnabled) {
        changed |= assignIfChanged(settings_.obdEnabled, update.enabled);
    }
    if (update.hasMinRssi) {
        const int clampedRssi = std::max(-100, std::min(static_cast<int>(update.minRssi), -40));
        changed |= assignIfChanged(settings_.obdMinRssi, static_cast<int8_t>(clampedRssi));
    }
    if (update.hasSavedAddress) {
        if (isValidBleAddress(update.savedAddress)) {
            changed |= assignIfChanged(settings_.obdSavedAddress, update.savedAddress);
        } else {
            Serial.printf("[Settings] WARN: Rejecting invalid OBD address update: '%s'\n",
                          update.savedAddress.c_str());
        }
    }
    if (update.hasSavedName) {
        changed |= assignIfChanged(settings_.obdSavedName, sanitizeObdSavedNameValue(update.savedName));
    }
    if (update.hasSavedAddrType) {
        changed |= assignIfChanged(settings_.obdSavedAddrType, update.savedAddrType);
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }

    return changed;
}

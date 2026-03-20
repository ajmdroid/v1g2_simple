/**
 * Settings property setters, slot getters/setters, and reset.
 * Extracted from settings.cpp to reduce file size.
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
        manager.saveDeferredBackup();
        return;
    }
    manager.save();
}

}  // namespace

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

void SettingsManager::applyDeviceSettingsUpdate(const DeviceSettingsUpdate& update,
                                                SettingsPersistMode persistMode) {
    if (update.hasApCredentials) {
        settings.apSSID = sanitizeApSsidValue(update.apSSID);
        settings.apPassword = sanitizeApPasswordValue(update.apPassword);
    }
    if (update.hasProxyBLE) {
        settings.proxyBLE = update.proxyBLE;
    }
    if (update.hasProxyName) {
        settings.proxyName = sanitizeProxyNameValue(update.proxyName);
    }
    if (update.hasAutoPowerOffMinutes) {
        settings.autoPowerOffMinutes = clampU8(update.autoPowerOffMinutes, 0, 60);
    }
    if (update.hasApTimeoutMinutes) {
        settings.apTimeoutMinutes = clampApTimeoutValue(update.apTimeoutMinutes);
    }
    if (update.hasEnableWifiAtBoot) {
        settings.enableWifiAtBoot = update.enableWifiAtBoot;
    }
    if (update.hasEnableSignalTraceLogging) {
        settings.enableSignalTraceLogging = update.enableSignalTraceLogging;
    }

    persistSettingsByMode(*this, persistMode);
}

void SettingsManager::applyAudioSettingsUpdate(const AudioSettingsUpdate& update,
                                               SettingsPersistMode persistMode) {
    if (update.hasVoiceAlertMode) {
        settings.voiceAlertMode = clampVoiceAlertModeValue(static_cast<int>(update.voiceAlertMode));
    }
    if (update.hasVoiceDirectionEnabled) {
        settings.voiceDirectionEnabled = update.voiceDirectionEnabled;
    }
    if (update.hasAnnounceBogeyCount) {
        settings.announceBogeyCount = update.announceBogeyCount;
    }
    if (update.hasMuteVoiceIfVolZero) {
        settings.muteVoiceIfVolZero = update.muteVoiceIfVolZero;
    }
    if (update.hasVoiceVolume) {
        settings.voiceVolume = clampU8(update.voiceVolume, 0, 100);
    }
    if (update.hasAnnounceSecondaryAlerts) {
        settings.announceSecondaryAlerts = update.announceSecondaryAlerts;
    }
    if (update.hasSecondaryLaser) {
        settings.secondaryLaser = update.secondaryLaser;
    }
    if (update.hasSecondaryKa) {
        settings.secondaryKa = update.secondaryKa;
    }
    if (update.hasSecondaryK) {
        settings.secondaryK = update.secondaryK;
    }
    if (update.hasSecondaryX) {
        settings.secondaryX = update.secondaryX;
    }
    if (update.hasAlertVolumeFadeEnabled) {
        settings.alertVolumeFadeEnabled = update.alertVolumeFadeEnabled;
    }
    if (update.hasAlertVolumeFadeDelaySec) {
        settings.alertVolumeFadeDelaySec = clampU8(update.alertVolumeFadeDelaySec, 1, 10);
    }
    if (update.hasAlertVolumeFadeVolume) {
        settings.alertVolumeFadeVolume = clampU8(update.alertVolumeFadeVolume, 0, 9);
    }

    persistSettingsByMode(*this, persistMode);
}

void SettingsManager::applyDisplaySettingsUpdate(const DisplaySettingsUpdate& update,
                                                 SettingsPersistMode persistMode) {
    if (update.hasColorBogey) settings.colorBogey = update.colorBogey;
    if (update.hasColorFrequency) settings.colorFrequency = update.colorFrequency;
    if (update.hasColorArrowFront) settings.colorArrowFront = update.colorArrowFront;
    if (update.hasColorArrowSide) settings.colorArrowSide = update.colorArrowSide;
    if (update.hasColorArrowRear) settings.colorArrowRear = update.colorArrowRear;
    if (update.hasColorBandL) settings.colorBandL = update.colorBandL;
    if (update.hasColorBandKa) settings.colorBandKa = update.colorBandKa;
    if (update.hasColorBandK) settings.colorBandK = update.colorBandK;
    if (update.hasColorBandX) settings.colorBandX = update.colorBandX;
    if (update.hasColorBandPhoto) settings.colorBandPhoto = update.colorBandPhoto;
    if (update.hasColorWiFiIcon) settings.colorWiFiIcon = update.colorWiFiIcon;
    if (update.hasColorWiFiConnected) settings.colorWiFiConnected = update.colorWiFiConnected;
    if (update.hasColorBleConnected) settings.colorBleConnected = update.colorBleConnected;
    if (update.hasColorBleDisconnected) settings.colorBleDisconnected = update.colorBleDisconnected;
    if (update.hasColorBar1) settings.colorBar1 = update.colorBar1;
    if (update.hasColorBar2) settings.colorBar2 = update.colorBar2;
    if (update.hasColorBar3) settings.colorBar3 = update.colorBar3;
    if (update.hasColorBar4) settings.colorBar4 = update.colorBar4;
    if (update.hasColorBar5) settings.colorBar5 = update.colorBar5;
    if (update.hasColorBar6) settings.colorBar6 = update.colorBar6;
    if (update.hasColorMuted) settings.colorMuted = update.colorMuted;
    if (update.hasColorPersisted) settings.colorPersisted = update.colorPersisted;
    if (update.hasColorVolumeMain) settings.colorVolumeMain = update.colorVolumeMain;
    if (update.hasColorVolumeMute) settings.colorVolumeMute = update.colorVolumeMute;
    if (update.hasColorRssiV1) settings.colorRssiV1 = update.colorRssiV1;
    if (update.hasColorRssiProxy) settings.colorRssiProxy = update.colorRssiProxy;
    if (update.hasColorLockout) settings.colorLockout = update.colorLockout;
    if (update.hasColorGps) settings.colorGps = update.colorGps;
    if (update.hasColorObd) settings.colorObd = update.colorObd;
    if (update.hasFreqUseBandColor) settings.freqUseBandColor = update.freqUseBandColor;
    if (update.hasHideWifiIcon) settings.hideWifiIcon = update.hideWifiIcon;
    if (update.hasHideProfileIndicator) settings.hideProfileIndicator = update.hideProfileIndicator;
    if (update.hasHideBatteryIcon) settings.hideBatteryIcon = update.hideBatteryIcon;
    if (update.hasShowBatteryPercent) settings.showBatteryPercent = update.showBatteryPercent;
    if (update.hasHideBleIcon) settings.hideBleIcon = update.hideBleIcon;
    if (update.hasHideVolumeIndicator) settings.hideVolumeIndicator = update.hideVolumeIndicator;
    if (update.hasHideRssiIndicator) settings.hideRssiIndicator = update.hideRssiIndicator;
    if (update.hasBrightness) settings.brightness = update.brightness;
    if (update.hasDisplayStyle) {
        settings.displayStyle = normalizeDisplayStyle(static_cast<int>(update.displayStyle));
    }

    persistSettingsByMode(*this, persistMode);
}

void SettingsManager::resetDisplaySettings(SettingsPersistMode persistMode) {
    settings.colorBogey = 0xF800;
    settings.colorFrequency = 0xF800;
    settings.colorArrowFront = 0xF800;
    settings.colorArrowSide = 0xF800;
    settings.colorArrowRear = 0xF800;
    settings.colorBandL = 0x001F;
    settings.colorBandKa = 0xF800;
    settings.colorBandK = 0x001F;
    settings.colorBandX = 0x07E0;
    settings.colorBandPhoto = 0x780F;
    settings.colorWiFiIcon = 0x07FF;
    settings.colorWiFiConnected = 0x07E0;
    settings.colorBleConnected = 0x07E0;
    settings.colorBleDisconnected = 0x001F;
    settings.colorBar1 = 0x07E0;
    settings.colorBar2 = 0x07E0;
    settings.colorBar3 = 0xFFE0;
    settings.colorBar4 = 0xFFE0;
    settings.colorBar5 = 0xF800;
    settings.colorBar6 = 0xF800;
    settings.colorMuted = 0x3186;
    settings.colorPersisted = 0x18C3;
    settings.colorVolumeMain = 0x001F;
    settings.colorVolumeMute = 0xFFE0;
    settings.colorRssiV1 = 0x07E0;
    settings.colorRssiProxy = 0x001F;
    settings.colorLockout = 0x07E0;
    settings.colorGps = 0x07FF;
    settings.colorObd = 0x001F;
    settings.freqUseBandColor = false;

    persistSettingsByMode(*this, persistMode);
}

GpsSettingsApplyResult SettingsManager::applyGpsSettingsUpdate(const GpsSettingsUpdate& update,
                                                              SettingsPersistMode persistMode) {
    GpsSettingsApplyResult result;

    if (update.hasEnabled && assignIfChanged(settings.gpsEnabled, update.enabled)) {
        result.changed = true;
        result.enabledChanged = true;
    }
    if (update.hasLockoutMode &&
        assignIfChanged(settings.gpsLockoutMode,
                        clampLockoutRuntimeModeValue(static_cast<int>(update.lockoutMode)))) {
        result.changed = true;
    }
    if (update.hasCoreGuardEnabled &&
        assignIfChanged(settings.gpsLockoutCoreGuardEnabled, update.coreGuardEnabled)) {
        result.changed = true;
    }
    if (update.hasMaxQueueDrops && assignIfChanged(settings.gpsLockoutMaxQueueDrops, update.maxQueueDrops)) {
        result.changed = true;
    }
    if (update.hasMaxPerfDrops && assignIfChanged(settings.gpsLockoutMaxPerfDrops, update.maxPerfDrops)) {
        result.changed = true;
    }
    if (update.hasMaxEventBusDrops && assignIfChanged(settings.gpsLockoutMaxEventBusDrops, update.maxEventBusDrops)) {
        result.changed = true;
    }
    if (update.hasLearnerPromotionHits &&
        assignIfChanged(settings.gpsLockoutLearnerPromotionHits,
                        clampLockoutLearnerHitsValue(update.learnerPromotionHits))) {
        result.changed = true;
        result.learnerTuningChanged = true;
    }
    if (update.hasLearnerRadiusE5 &&
        assignIfChanged(settings.gpsLockoutLearnerRadiusE5,
                        clampLockoutLearnerRadiusE5Value(update.learnerRadiusE5))) {
        result.changed = true;
        result.learnerTuningChanged = true;
    }
    if (update.hasLearnerFreqToleranceMHz &&
        assignIfChanged(settings.gpsLockoutLearnerFreqToleranceMHz,
                        clampLockoutLearnerFreqTolValue(update.learnerFreqToleranceMHz))) {
        result.changed = true;
        result.learnerTuningChanged = true;
    }
    if (update.hasLearnerLearnIntervalHours &&
        assignIfChanged(settings.gpsLockoutLearnerLearnIntervalHours,
                        clampLockoutLearnerIntervalHoursValue(update.learnerLearnIntervalHours))) {
        result.changed = true;
        result.learnerTuningChanged = true;
    }
    if (update.hasLearnerUnlearnIntervalHours &&
        assignIfChanged(settings.gpsLockoutLearnerUnlearnIntervalHours,
                        clampLockoutLearnerIntervalHoursValue(update.learnerUnlearnIntervalHours))) {
        result.changed = true;
    }
    if (update.hasLearnerUnlearnCount &&
        assignIfChanged(settings.gpsLockoutLearnerUnlearnCount,
                        clampLockoutLearnerUnlearnCountValue(update.learnerUnlearnCount))) {
        result.changed = true;
    }
    if (update.hasManualDemotionMissCount &&
        assignIfChanged(settings.gpsLockoutManualDemotionMissCount,
                        clampLockoutManualDemotionMissCountValue(update.manualDemotionMissCount))) {
        result.changed = true;
    }
    if (update.hasKaLearningEnabled &&
        assignIfChanged(settings.gpsLockoutKaLearningEnabled, update.kaLearningEnabled)) {
        result.changed = true;
        result.bandLearningPolicyChanged = true;
    }
    if (update.hasKLearningEnabled &&
        assignIfChanged(settings.gpsLockoutKLearningEnabled, update.kLearningEnabled)) {
        result.changed = true;
        result.bandLearningPolicyChanged = true;
    }
    if (update.hasXLearningEnabled &&
        assignIfChanged(settings.gpsLockoutXLearningEnabled, update.xLearningEnabled)) {
        result.changed = true;
        result.bandLearningPolicyChanged = true;
    }
    if (update.hasPreQuiet && assignIfChanged(settings.gpsLockoutPreQuiet, update.preQuiet)) {
        result.changed = true;
    }
    if (update.hasPreQuietBufferE5 &&
        assignIfChanged(settings.gpsLockoutPreQuietBufferE5,
                        clampLockoutPreQuietBufferE5Value(update.preQuietBufferE5))) {
        result.changed = true;
    }
    if (update.hasMaxHdopX10 &&
        assignIfChanged(settings.gpsLockoutMaxHdopX10,
                        clampLockoutGpsMaxHdopX10Value(update.maxHdopX10))) {
        result.changed = true;
        result.learnerTuningChanged = true;
    }
    if (update.hasMinLearnerSpeedMph &&
        assignIfChanged(settings.gpsLockoutMinLearnerSpeedMph,
                        clampLockoutGpsMinLearnerSpeedMphValue(update.minLearnerSpeedMph))) {
        result.changed = true;
        result.learnerTuningChanged = true;
    }

    if (result.changed) {
        persistSettingsByMode(*this, persistMode);
    }

    return result;
}

bool SettingsManager::applyObdSettingsUpdate(const ObdSettingsUpdate& update,
                                             SettingsPersistMode persistMode) {
    bool changed = false;

    if (update.resetSavedNameOnAddressChange &&
        update.hasSavedAddress &&
        settings.obdSavedAddress != update.savedAddress &&
        !update.hasSavedName) {
        changed |= assignIfChanged(settings.obdSavedName, String(""));
    }

    if (update.hasEnabled) {
        changed |= assignIfChanged(settings.obdEnabled, update.enabled);
    }
    if (update.hasMinRssi) {
        const int clampedRssi = std::max(-90, std::min(static_cast<int>(update.minRssi), -40));
        changed |= assignIfChanged(settings.obdMinRssi, static_cast<int8_t>(clampedRssi));
    }
    if (update.hasSavedAddress) {
        changed |= assignIfChanged(settings.obdSavedAddress, update.savedAddress);
    }
    if (update.hasSavedName) {
        changed |= assignIfChanged(settings.obdSavedName, sanitizeObdSavedNameValue(update.savedName));
    }
    if (update.hasSavedAddrType) {
        changed |= assignIfChanged(settings.obdSavedAddrType, update.savedAddrType);
    }
    if (update.hasCachedVinPrefix11) {
        changed |= assignIfChanged(settings.obdCachedVinPrefix11, update.cachedVinPrefix11);
    }
    if (update.hasCachedEotProfileId) {
        changed |= assignIfChanged(settings.obdCachedEotProfileId, update.cachedEotProfileId);
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }

    return changed;
}

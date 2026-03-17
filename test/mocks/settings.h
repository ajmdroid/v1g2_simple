#pragma once
#ifndef SETTINGS_H
#define SETTINGS_H

#ifdef ARDUINO
#include <Arduino.h>
#else
#include "Arduino.h"
#endif
#include <cstdint>

// Voice alert modes (subset of real settings.h)
enum VoiceAlertMode : uint8_t {
    VOICE_MODE_DISABLED = 0,
    VOICE_MODE_BAND_ONLY = 1,
    VOICE_MODE_FREQ_ONLY = 2,
    VOICE_MODE_BAND_FREQ = 3
};

enum LockoutRuntimeMode : uint8_t {
    LOCKOUT_RUNTIME_OFF = 0,
    LOCKOUT_RUNTIME_SHADOW = 1,
    LOCKOUT_RUNTIME_ADVISORY = 2,
    LOCKOUT_RUNTIME_ENFORCE = 3
};

inline LockoutRuntimeMode clampLockoutRuntimeModeValue(int rawMode) {
    if (rawMode < static_cast<int>(LOCKOUT_RUNTIME_OFF)) {
        return LOCKOUT_RUNTIME_OFF;
    }
    if (rawMode > static_cast<int>(LOCKOUT_RUNTIME_ENFORCE)) {
        return LOCKOUT_RUNTIME_ENFORCE;
    }
    return static_cast<LockoutRuntimeMode>(rawMode);
}

inline const char* lockoutRuntimeModeName(LockoutRuntimeMode mode) {
    switch (mode) {
        case LOCKOUT_RUNTIME_OFF:      return "OFF";
        case LOCKOUT_RUNTIME_SHADOW:   return "SHADOW";
        case LOCKOUT_RUNTIME_ADVISORY: return "ADVISORY";
        case LOCKOUT_RUNTIME_ENFORCE:  return "ENFORCE";
        default:                       return "UNKNOWN";
    }
}

// GPS quality gate constants (must match real settings.h)
static constexpr uint8_t LOCKOUT_LEARNER_HITS_DEFAULT = 3;
static constexpr uint8_t LOCKOUT_LEARNER_HITS_MIN = 2;
static constexpr uint8_t LOCKOUT_LEARNER_HITS_MAX = 6;
static constexpr uint16_t LOCKOUT_LEARNER_RADIUS_E5_DEFAULT = 135;
static constexpr uint16_t LOCKOUT_LEARNER_RADIUS_E5_MIN = 45;
static constexpr uint16_t LOCKOUT_LEARNER_RADIUS_E5_MAX = 360;
static constexpr uint16_t LOCKOUT_LEARNER_FREQ_TOL_DEFAULT = 10;
static constexpr uint16_t LOCKOUT_LEARNER_FREQ_TOL_MIN = 2;
static constexpr uint16_t LOCKOUT_LEARNER_FREQ_TOL_MAX = 20;
static constexpr uint8_t LOCKOUT_LEARNER_LEARN_INTERVAL_HOURS_DEFAULT = 0;
static constexpr uint8_t LOCKOUT_LEARNER_UNLEARN_INTERVAL_HOURS_DEFAULT = 0;
static constexpr uint8_t LOCKOUT_LEARNER_UNLEARN_COUNT_DEFAULT = 0;
static constexpr uint8_t LOCKOUT_LEARNER_UNLEARN_COUNT_MIN = 0;
static constexpr uint8_t LOCKOUT_LEARNER_UNLEARN_COUNT_MAX = 10;
static constexpr uint8_t LOCKOUT_MANUAL_DEMOTION_MISS_COUNT_DEFAULT = 0;
static constexpr uint16_t LOCKOUT_PRE_QUIET_BUFFER_E5_DEFAULT = 0;
static constexpr uint16_t LOCKOUT_PRE_QUIET_BUFFER_E5_MAX = 135;
static constexpr uint8_t  LOCKOUT_GPS_MIN_SATELLITES = 4;
static constexpr uint16_t LOCKOUT_GPS_MAX_HDOP_X10_DEFAULT = 50;
static constexpr uint16_t LOCKOUT_GPS_MAX_HDOP_X10_MIN = 10;
static constexpr uint16_t LOCKOUT_GPS_MAX_HDOP_X10_MAX = 100;
static constexpr uint8_t LOCKOUT_GPS_MIN_LEARNER_SPEED_MPH_DEFAULT = 5;
static constexpr uint8_t LOCKOUT_GPS_MIN_LEARNER_SPEED_MPH_MIN = 0;
static constexpr uint8_t LOCKOUT_GPS_MIN_LEARNER_SPEED_MPH_MAX = 20;
static constexpr uint32_t LOCKOUT_GPS_COURSE_MAX_AGE_MS = 5000;

inline uint16_t clampLockoutPreQuietBufferE5Value(int rawBuffer) {
    return static_cast<uint16_t>(std::max(0,
                                          std::min(rawBuffer, static_cast<int>(LOCKOUT_PRE_QUIET_BUFFER_E5_MAX))));
}

inline uint16_t clampLockoutGpsMaxHdopX10Value(int rawHdopX10) {
    return static_cast<uint16_t>(std::max(static_cast<int>(LOCKOUT_GPS_MAX_HDOP_X10_MIN),
                                          std::min(rawHdopX10, static_cast<int>(LOCKOUT_GPS_MAX_HDOP_X10_MAX))));
}

inline uint8_t clampLockoutGpsMinLearnerSpeedMphValue(int rawSpeed) {
    return static_cast<uint8_t>(std::max(static_cast<int>(LOCKOUT_GPS_MIN_LEARNER_SPEED_MPH_MIN),
                                         std::min(rawSpeed, static_cast<int>(LOCKOUT_GPS_MIN_LEARNER_SPEED_MPH_MAX))));
}

inline uint8_t clampLockoutLearnerHitsValue(int rawHits) {
    return static_cast<uint8_t>(std::max(static_cast<int>(LOCKOUT_LEARNER_HITS_MIN),
                                         std::min(rawHits, static_cast<int>(LOCKOUT_LEARNER_HITS_MAX))));
}

inline uint16_t clampLockoutLearnerRadiusE5Value(int rawRadiusE5) {
    return static_cast<uint16_t>(std::max(static_cast<int>(LOCKOUT_LEARNER_RADIUS_E5_MIN),
                                          std::min(rawRadiusE5, static_cast<int>(LOCKOUT_LEARNER_RADIUS_E5_MAX))));
}

inline uint16_t clampLockoutLearnerFreqTolValue(int rawFreqTol) {
    return static_cast<uint16_t>(std::max(static_cast<int>(LOCKOUT_LEARNER_FREQ_TOL_MIN),
                                          std::min(rawFreqTol, static_cast<int>(LOCKOUT_LEARNER_FREQ_TOL_MAX))));
}

inline uint8_t clampLockoutLearnerIntervalHoursValue(int rawHours) {
    if (rawHours <= 0) return 0;
    if (rawHours <= 1) return 1;
    if (rawHours <= 4) return 4;
    if (rawHours <= 12) return 12;
    return 24;
}

inline uint8_t clampLockoutLearnerUnlearnCountValue(int rawCount) {
    return static_cast<uint8_t>(std::max(static_cast<int>(LOCKOUT_LEARNER_UNLEARN_COUNT_MIN),
                                         std::min(rawCount, static_cast<int>(LOCKOUT_LEARNER_UNLEARN_COUNT_MAX))));
}

inline uint8_t clampLockoutManualDemotionMissCountValue(int rawCount) {
    if (rawCount <= 0) return 0;
    if (rawCount <= 10) return 10;
    if (rawCount <= 25) return 25;
    return 50;
}
// Minimal display font enum (for compatibility with older tests)
enum FontStyle : uint8_t {
    FONT_STYLE_CLASSIC = 0,
    FONT_STYLE_MODERN = 1,
    FONT_STYLE_HEMI = 2,
    FONT_STYLE_SERPENTINE = 3
};

enum V1Mode : uint8_t {
    V1_MODE_UNKNOWN = 0x00,
    V1_MODE_ALL_BOGEYS = 0x01,
    V1_MODE_LOGIC = 0x02,
    V1_MODE_ADVANCED_LOGIC = 0x03
};

struct AutoPushSlot {
    String profileName;
    V1Mode mode = V1_MODE_UNKNOWN;
};

// Mocked settings structure (superset of fields used in modules/tests)
struct V1Settings {
    // Display
    uint8_t brightness = 128;
    bool displayOn = true;
    FontStyle fontStyle = FONT_STYLE_CLASSIC;
    // Colors (kept for older display tests)
    uint16_t colorX = 0x07E0;
    uint16_t colorK = 0x07FF;
    uint16_t colorKa = 0xF800;
    uint16_t colorLaser = 0xFFFF;
    uint16_t colorPhoto = 0xF81F;
    uint16_t colorMuted = 0x8410;
    uint16_t colorBogey = 0xFFE0;
    
    // Audio / voice
    uint8_t volume = 5;
    VoiceAlertMode voiceAlertMode = VOICE_MODE_BAND_FREQ;
    bool voiceDirectionEnabled = true;
    bool announceBogeyCount = true;
    bool muteVoiceIfVolZero = true;
    
    // Secondary alerts
    bool announceSecondaryAlerts = true;
    bool secondaryLaser = true;
    bool secondaryKa = true;
    bool secondaryK = true;
    bool secondaryX = true;
    
    // Volume fade
    bool alertVolumeFadeEnabled = false;
    uint8_t alertVolumeFadeDelaySec = 5;
    uint8_t alertVolumeFadeVolume = 3;
    
    // Misc flags retained for compatibility
    bool gpsEnabled = true;
    bool obdEnabled = false;
    uint8_t autoPowerOffMinutes = 10;
    String obdSavedAddress = "";
    uint8_t obdSavedAddrType = 0;
    int8_t obdMinRssi = -80;
    String obdCachedVinPrefix11 = "";
    uint8_t obdCachedEotProfileId = 0;
    LockoutRuntimeMode gpsLockoutMode = LOCKOUT_RUNTIME_OFF;
    bool gpsLockoutPreQuiet = false;
    uint16_t gpsLockoutPreQuietBufferE5 = 0;
    bool gpsLockoutCoreGuardEnabled = true;
    uint16_t gpsLockoutMaxQueueDrops = 0;
    uint16_t gpsLockoutMaxPerfDrops = 0;
    uint16_t gpsLockoutMaxEventBusDrops = 0;
    uint8_t gpsLockoutLearnerPromotionHits = LOCKOUT_LEARNER_HITS_DEFAULT;
    uint16_t gpsLockoutLearnerRadiusE5 = LOCKOUT_LEARNER_RADIUS_E5_DEFAULT;
    uint16_t gpsLockoutLearnerFreqToleranceMHz = LOCKOUT_LEARNER_FREQ_TOL_DEFAULT;
    uint8_t gpsLockoutLearnerLearnIntervalHours = 0;
    uint8_t gpsLockoutLearnerUnlearnIntervalHours = 0;
    uint8_t gpsLockoutLearnerUnlearnCount = 0;
    uint8_t gpsLockoutManualDemotionMissCount = 0;
    bool gpsLockoutKaLearningEnabled = false;
    bool gpsLockoutKLearningEnabled = true;
    bool gpsLockoutXLearningEnabled = true;
    uint16_t gpsLockoutMaxHdopX10 = 50;          // 5.0 HDOP × 10
    uint8_t gpsLockoutMinLearnerSpeedMph = 5;
    bool bleProxyEnabled = true;
    uint8_t activeSlot = 0;
    bool autoPushEnabled = false;

};

// Backwards compatibility alias used by some legacy tests
using Settings = V1Settings;

// Settings manager stub
class SettingsManager {
public:
    V1Settings settings;
    int saveCalls = 0;
    int setGpsEnabledCalls = 0;
    int saveDeferredBackupCalls = 0;
    int backupToSDCalls = 0;
    int requestDeferredBackupCalls = 0;
    uint8_t slotAlertPersistSec[3] = {0, 0, 0};
    AutoPushSlot slotConfigs[3];
    uint8_t slotVolumes[3] = {0xFF, 0xFF, 0xFF};
    uint8_t slotMuteVolumes[3] = {0xFF, 0xFF, 0xFF};
    bool slotDarkModes[3] = {false, false, false};
    bool slotMuteToZero[3] = {false, false, false};
    bool backupToSDResult = true;
    
    void load() {}
    void save() { ++saveCalls; }
    void setGpsEnabled(bool enabled) {
        settings.gpsEnabled = enabled;
        ++setGpsEnabledCalls;
    }
    void saveDeferredBackup() { ++saveDeferredBackupCalls; }
    void setDefaults() {}
    bool backupToSD() {
        ++backupToSDCalls;
        return backupToSDResult;
    }
    void requestDeferredBackupFromCurrentState() { ++requestDeferredBackupCalls; }
    bool deferredBackupPending() const { return false; }
    bool deferredBackupRetryScheduled() const { return false; }
    uint32_t deferredBackupNextAttemptAtMs() const { return 0; }
    void setLastV1Address(const char*) {}
    void setActiveSlot(int slot) { settings.activeSlot = static_cast<uint8_t>(slot); }
    void setAutoPushEnabled(bool enabled) { settings.autoPushEnabled = enabled; }
    void setSlot(int slotNum, const String& profile, V1Mode mode) {
        if (slotNum < 0 || slotNum > 2) return;
        slotConfigs[slotNum].profileName = profile;
        slotConfigs[slotNum].mode = mode;
    }
    const AutoPushSlot& getSlot(int slotNum) const {
        static AutoPushSlot fallback;
        if (slotNum < 0 || slotNum > 2) return fallback;
        return slotConfigs[slotNum];
    }
    uint8_t getSlotVolume(int slotNum) const {
        return (slotNum >= 0 && slotNum < 3) ? slotVolumes[slotNum] : 0xFF;
    }
    uint8_t getSlotMuteVolume(int slotNum) const {
        return (slotNum >= 0 && slotNum < 3) ? slotMuteVolumes[slotNum] : 0xFF;
    }
    bool getSlotDarkMode(int slotNum) const {
        return (slotNum >= 0 && slotNum < 3) ? slotDarkModes[slotNum] : false;
    }
    bool getSlotMuteToZero(int slotNum) const {
        return (slotNum >= 0 && slotNum < 3) ? slotMuteToZero[slotNum] : false;
    }
    void setSlotVolumes(int slotNum, uint8_t volume, uint8_t muteVolume) {
        if (slotNum < 0 || slotNum > 2) return;
        slotVolumes[slotNum] = volume;
        slotMuteVolumes[slotNum] = muteVolume;
    }
    void setSlotDarkMode(int slotNum, bool darkMode) {
        if (slotNum < 0 || slotNum > 2) return;
        slotDarkModes[slotNum] = darkMode;
    }
    void setSlotMuteToZero(int slotNum, bool enabled) {
        if (slotNum < 0 || slotNum > 2) return;
        slotMuteToZero[slotNum] = enabled;
    }
    
    const V1Settings& get() const { return settings; }
    V1Settings& getMutable() { return settings; }
    V1Settings& mutableSettings() { return settings; }
    uint8_t getSlotAlertPersistSec(uint8_t slot) const {
        return (slot < 3) ? slotAlertPersistSec[slot] : 0;
    }
    
    // Convenience helpers used by some tests
    uint8_t getBrightness() const { return settings.brightness; }
    void setBrightness(uint8_t b) { settings.brightness = b; }

    bool isDisplayOn() const { return settings.displayOn; }
    void setDisplayOn(bool on) { settings.displayOn = on; }
};

// Global settings instance
extern SettingsManager settingsManager;

#endif // SETTINGS_H

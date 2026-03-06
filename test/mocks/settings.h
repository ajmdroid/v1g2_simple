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
static constexpr uint8_t  LOCKOUT_GPS_MIN_SATELLITES = 4;
static constexpr uint32_t LOCKOUT_GPS_COURSE_MAX_AGE_MS = 5000;
static constexpr uint32_t CAMERA_ALERT_RANGE_CM_MIN = 16093;
static constexpr uint32_t CAMERA_ALERT_RANGE_CM_MAX = 160934;
static constexpr uint32_t CAMERA_ALERT_RANGE_CM_DEFAULT = 128748;

inline uint32_t clampCameraAlertRangeCmValue(int rawRangeCm) {
    if (rawRangeCm < static_cast<int>(CAMERA_ALERT_RANGE_CM_MIN)) {
        return CAMERA_ALERT_RANGE_CM_MIN;
    }
    if (rawRangeCm > static_cast<int>(CAMERA_ALERT_RANGE_CM_MAX)) {
        return CAMERA_ALERT_RANGE_CM_MAX;
    }
    return static_cast<uint32_t>(rawRangeCm);
}

// Minimal display font enum (for compatibility with older tests)
enum FontStyle : uint8_t {
    FONT_STYLE_CLASSIC = 0,
    FONT_STYLE_MODERN = 1,
    FONT_STYLE_HEMI = 2,
    FONT_STYLE_SERPENTINE = 3
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
    LockoutRuntimeMode gpsLockoutMode = LOCKOUT_RUNTIME_OFF;
    bool gpsLockoutPreQuiet = false;
    uint16_t gpsLockoutPreQuietBufferE5 = 0;
    bool gpsLockoutCoreGuardEnabled = true;
    uint16_t gpsLockoutMaxQueueDrops = 0;
    uint16_t gpsLockoutMaxPerfDrops = 0;
    uint16_t gpsLockoutMaxEventBusDrops = 0;
    uint8_t gpsLockoutLearnerLearnIntervalHours = 0;
    uint8_t gpsLockoutLearnerUnlearnIntervalHours = 0;
    uint8_t gpsLockoutLearnerUnlearnCount = 0;
    uint8_t gpsLockoutManualDemotionMissCount = 0;
    bool gpsLockoutKaLearningEnabled = false;
    uint16_t gpsLockoutMaxHdopX10 = 50;          // 5.0 HDOP × 10
    uint8_t gpsLockoutMinLearnerSpeedMph = 5;
    bool bleProxyEnabled = true;

    // Camera alerts
    bool cameraAlertsEnabled = true;
    uint32_t cameraAlertRangeCm = CAMERA_ALERT_RANGE_CM_DEFAULT;
    bool cameraTypeAlpr = true;
    bool cameraTypeRedLight = true;
    bool cameraTypeSpeed = true;
    bool cameraTypeBusLane = false;
    uint16_t colorCameraArrow = 0x780F;
    uint16_t colorCameraText = 0x780F;
    bool cameraVoiceFarEnabled = true;
    bool cameraVoiceNearEnabled = true;
};

// Backwards compatibility alias used by some legacy tests
using Settings = V1Settings;

// Settings manager stub
class SettingsManager {
public:
    V1Settings settings;
    
    void load() {}
    void save() {}
    void setDefaults() {}
    
    const V1Settings& get() const { return settings; }
    V1Settings& getMutable() { return settings; }
    
    // Convenience helpers used by some tests
    uint8_t getBrightness() const { return settings.brightness; }
    void setBrightness(uint8_t b) { settings.brightness = b; }

    bool isDisplayOn() const { return settings.displayOn; }
    void setDisplayOn(bool on) { settings.displayOn = on; }
};

// Global settings instance
extern SettingsManager settingsManager;

#endif // SETTINGS_H

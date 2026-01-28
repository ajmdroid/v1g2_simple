#pragma once

#include <Arduino.h>
#include <vector>

#include "display.h"
#include "display_mode.h"
#include "packet_parser.h"
#include "settings.h"
#include "gps_handler.h"
#include "lockout_manager.h"
#include "auto_lockout_manager.h"
#include "ble_client.h"
#include "modules/camera/camera_alert_module.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/voice/voice_module.h"
#include "modules/speed_volume/speed_volume_module.h"
#include "debug_logger.h"

class DisplayPipelineModule {
public:
    void begin(DisplayMode* displayModePtr,
               V1Display* displayPtr,
               PacketParser* parserPtr,
               SettingsManager* settingsMgr,
               GPSHandler* gpsHandler,
               LockoutManager* lockouts,
               AutoLockoutManager* autoLockouts,
               V1BLEClient* bleClient,
               CameraAlertModule* cameraAlertModule,
               AlertPersistenceModule* alertPersistenceModule,
               VolumeFadeModule* volumeFadeModule,
               VoiceModule* voiceModule,
               SpeedVolumeModule* speedVolumeModule,
               DebugLogger* debugLogger);

    // Process after a successful parser.parse(); expects parser state already updated.
    void handleParsed(unsigned long nowMs);

private:
    DisplayMode* displayMode = nullptr;
    V1Display* display = nullptr;
    PacketParser* parser = nullptr;
    SettingsManager* settings = nullptr;
    GPSHandler* gps = nullptr;
    LockoutManager* lockoutMgr = nullptr;
    AutoLockoutManager* autoLockoutMgr = nullptr;
    V1BLEClient* ble = nullptr;
    CameraAlertModule* cameraAlert = nullptr;
    AlertPersistenceModule* alertPersistence = nullptr;
    VolumeFadeModule* volumeFade = nullptr;
    VoiceModule* voice = nullptr;
    SpeedVolumeModule* speedVolume = nullptr;
    DebugLogger* debug = nullptr;

    // Mute debounce
    bool debouncedMuteState = false;
    unsigned long lastMuteChangeMs = 0;
    static constexpr unsigned long MUTE_DEBOUNCE_MS = 150;

    // Display throttling
    unsigned long lastDisplayDraw = 0;
    static constexpr unsigned long DISPLAY_DRAW_MIN_MS = 50;

    // Alert gap recovery
    unsigned long lastAlertGapRecoverMs = 0;

    // Lockout mute tracking
    bool lockoutMuteSent = false;
    uint32_t lastLockoutAlertId = 0xFFFFFFFF;

    // Instrumentation
    unsigned long displayLatencySum = 0;
    unsigned long displayLatencyCount = 0;
    unsigned long displayLatencyMax = 0;
    unsigned long displayLatencyLastLog = 0;
    static constexpr unsigned long DISPLAY_LOG_INTERVAL_MS = 10000;
    static constexpr unsigned long DISPLAY_SLOW_THRESHOLD_US = 16000;
    static constexpr bool PERF_TIMING_LOGS = false;
    unsigned long perfTimingAccum = 0;
    unsigned long perfTimingCount = 0;
    unsigned long perfTimingMax = 0;
    unsigned long perfLastReport = 0;

    void recordDisplayTiming(const char* label, unsigned long startUs, unsigned long endUs);
    void recordPerfTiming(const char* label, unsigned long startUs, unsigned long endUs);
};

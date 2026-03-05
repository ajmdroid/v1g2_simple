#pragma once

#include <Arduino.h>
#include <vector>

#include "display.h"
#include "display_mode.h"
#include "packet_parser.h"
#include "settings.h"
#include "ble_client.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/voice/voice_module.h"

class DisplayPipelineModule {
public:
    void begin(DisplayMode* displayModePtr,
               V1Display* displayPtr,
               PacketParser* parserPtr,
               SettingsManager* settingsMgr,
               V1BLEClient* bleClient,
               AlertPersistenceModule* alertPersistenceModule,
               VolumeFadeModule* volumeFadeModule,
               VoiceModule* voiceModule);

    // Process after a successful parser.parse(); expects parser state already updated.
    // prioritySuppressed is a per-frame software suppression flag (e.g. lockout ENFORCE match).
    void handleParsed(unsigned long nowMs, bool prioritySuppressed);

private:
    DisplayMode* displayMode = nullptr;
    V1Display* display = nullptr;
    PacketParser* parser = nullptr;
    SettingsManager* settings = nullptr;
    V1BLEClient* ble = nullptr;
    AlertPersistenceModule* alertPersistence = nullptr;
    VolumeFadeModule* volumeFade = nullptr;
    VoiceModule* voice = nullptr;

    // Mute debounce
    bool debouncedMuteState = false;
    unsigned long lastMuteChangeMs = 0;
    static constexpr unsigned long MUTE_DEBOUNCE_MS = 150;

    // Display throttling
    unsigned long lastDisplayDraw = 0;
    static constexpr unsigned long DISPLAY_DRAW_MIN_MS = 25;

    // Alert gap recovery
    unsigned long displayLatencySum = 0;
    unsigned long displayLatencyCount = 0;
    unsigned long displayLatencyMax = 0;
    unsigned long displayLatencyLastLog = 0;
    static constexpr unsigned long DISPLAY_LOG_INTERVAL_MS = 10000;
    static constexpr bool PERF_TIMING_LOGS = false;
    unsigned long perfTimingAccum = 0;
    unsigned long perfTimingCount = 0;
    unsigned long perfTimingMax = 0;
    unsigned long perfLastReport = 0;

    void recordDisplayTiming(const char* label, unsigned long startUs, unsigned long endUs);
    void recordPerfTiming(const char* label, unsigned long startUs, unsigned long endUs);
};

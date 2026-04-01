#pragma once

#include <stdint.h>

// Forward declarations — full headers included in display_pipeline_module.cpp
class V1Display;
struct DisplayState;
class PacketParser;
enum class DisplayMode;

class SettingsManager;
class V1BLEClient;
class AlertPersistenceModule;
class VoiceModule;
class SpeedMuteModule;
class QuietCoordinatorModule;

class DisplayPipelineModule {
public:
    void begin(DisplayMode* displayModePtr,
               V1Display* displayPtr,
               PacketParser* parserPtr,
               SettingsManager* settingsMgr,
               V1BLEClient* bleClient,
               AlertPersistenceModule* alertPersistenceModule,
               VoiceModule* voiceModule,
               QuietCoordinatorModule* quietCoordinator);

    void setSpeedMuteModule(SpeedMuteModule* module) { speedMute_ = module; }

    // Process after a successful parser.parse(); expects parser state already updated.
    void handleParsed(uint32_t nowMs);
    void restoreCurrentOwner(uint32_t nowMs);
    bool allowsObdPairGesture(uint32_t nowMs) const;

private:
    enum class RenderOwner : uint8_t {
        Unknown = 0,
        Scanning,
        Live,
        Persisted,
        Resting
    };

    DisplayMode* displayMode_ = nullptr;
    V1Display* display_ = nullptr;
    PacketParser* parser_ = nullptr;
    SettingsManager* settings_ = nullptr;
    V1BLEClient* ble_ = nullptr;
    AlertPersistenceModule* alertPersistence_ = nullptr;
    VoiceModule* voice_ = nullptr;
    SpeedMuteModule* speedMute_ = nullptr;
    QuietCoordinatorModule* quiet_ = nullptr;

    // Mute debounce
    bool debouncedMuteState_ = false;
    unsigned long lastMuteChangeMs_ = 0;
    static constexpr unsigned long MUTE_DEBOUNCE_MS = 150;

    // Display throttling
    unsigned long lastDisplayDraw_ = 0;
    static constexpr unsigned long DISPLAY_DRAW_MIN_MS = 25;

    // Alert gap recovery
    unsigned long lastAlertGapRecoverMs_ = 0;
    unsigned long displayLatencySum_ = 0;
    unsigned long displayLatencyCount_ = 0;
    unsigned long displayLatencyMax_ = 0;
    unsigned long displayLatencyLastLog_ = 0;
    static constexpr unsigned long DISPLAY_LOG_INTERVAL_MS = 10000;
    static constexpr bool PERF_TIMING_LOGS = false;
    unsigned long perfTimingAccum_ = 0;
    unsigned long perfTimingCount_ = 0;
    unsigned long perfTimingMax_ = 0;
    unsigned long perfLastReport_ = 0;
    int lastPersistenceSlot_ = -1;
    RenderOwner lastRenderedOwner_ = RenderOwner::Unknown;

    void recordDisplayTiming(const char* label, unsigned long startUs, unsigned long endUs);
    void recordPerfTiming(const char* label, unsigned long startUs, unsigned long endUs);
    void renderIdleOwner(uint32_t nowMs,
                         const DisplayState& state,
                         bool forceRedraw,
                         bool restoreContext = false);
};

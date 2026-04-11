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
    DisplayMode* displayMode_ = nullptr;
    V1Display* display_ = nullptr;
    PacketParser* parser_ = nullptr;
    SettingsManager* settings_ = nullptr;
    V1BLEClient* ble_ = nullptr;
    AlertPersistenceModule* alertPersistence_ = nullptr;
    VoiceModule* voice_ = nullptr;
    SpeedMuteModule* speedMute_ = nullptr;
    QuietCoordinatorModule* quiet_ = nullptr;

    int lastPersistenceSlot_ = -1;

    void renderIdleOwner(uint32_t nowMs,
                         const DisplayState& state,
                         bool forceRedraw,
                         bool restoreContext = false);
    void recordPerfTiming(unsigned long startUs, unsigned long endUs);
};

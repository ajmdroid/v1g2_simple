#pragma once

#include <stdint.h>

#include "lockout_runtime_mute_controller.h"
#include "lockout_pre_quiet_controller.h"

class V1BLEClient;
class PacketParser;
class SettingsManager;
class V1Display;
class LockoutEnforcer;
class LockoutIndex;
class SignalCaptureModule;
class SystemEventBus;
struct PerfCounters;
class TimeService;
struct GpsRuntimeStatus;
class QuietCoordinatorModule;
class SpeedSourceSelector;

enum class LockoutVolumeCommandType : uint8_t {
    None = 0,
    PreQuietDrop,
    PreQuietRestore,
};

struct LockoutVolumeCommand {
    LockoutVolumeCommandType type = LockoutVolumeCommandType::None;
    uint8_t volume = 0;
    uint8_t muteVolume = 0;

    bool hasAction() const {
        return type != LockoutVolumeCommandType::None;
    }
};

/// Result emitted per process() call for main-loop consumption.
struct LockoutOrchestrationResult {
    bool prioritySuppressed = false;  // True → suppress voice priority announcements
    LockoutVolumeCommand volumeCommand{};
};

/// Orchestrates the full lockout enforcement pipeline:
///   signal capture → enforce → mute execution → safety override → pre-quiet.
///
/// Extracted from main.cpp loop() to consolidate the static state and
/// decision logic that previously lived inline.
///
/// Thread safety: designed for single-threaded access from loop().
class LockoutOrchestrationModule {
public:
    void begin(V1BLEClient* ble,
               PacketParser* parser,
               SettingsManager* settings,
               V1Display* display,
               LockoutEnforcer* enforcer,
               LockoutIndex* index,
               SignalCaptureModule* sigCapture,
               SystemEventBus* eventBus,
               PerfCounters* perfCounters,
               TimeService* timeSvc,
               QuietCoordinatorModule* quietCoordinator,
               SpeedSourceSelector* speedSelector);

    /// Run the full lockout pipeline for one parsed BLE frame.
    /// @param nowMs                Current millis() timestamp
    /// @param gpsStatus            Current GPS snapshot
    /// @param proxyClientConnected True when a proxy (app) client is connected
    /// @param enableSignalTrace    Settings flag for signal trace logging
    LockoutOrchestrationResult process(uint32_t nowMs,
                                       const GpsRuntimeStatus& gpsStatus,
                                       bool proxyClientConnected,
                                       bool enableSignalTrace);

    /// Reset all internal state (e.g. on proxy connect or mode change).
    void reset();

    /// True when pre-quiet has actively lowered V1 volume (DROPPED phase).
    bool isPreQuietActive() const { return preQuietState_.phase == PreQuietPhase::DROPPED; }

    /// Inject a volume hint from an external owner (e.g. speed volume).
    /// When set, pre-quiet uses this value instead of DisplayState for its
    /// volume capture, preventing stale-capture of a lowered volume.
    void setVolumeHint(uint8_t mainVol, uint8_t muteVol) {
        volumeHintMain_ = mainVol;
        volumeHintMute_ = muteVol;
    }
    void clearVolumeHint() { volumeHintMain_ = 0xFF; }

private:
    PreQuietState preQuietState_{};

    // External volume hint (e.g. from speed volume module).
    // When set (main != 0xFF), pre-quiet captures this instead of DisplayState.
    uint8_t volumeHintMain_ = 0xFF;
    uint8_t volumeHintMute_ = 0;

    // DI pointers (set in begin())
    V1BLEClient* ble_ = nullptr;
    PacketParser* parser_ = nullptr;
    SettingsManager* settings_ = nullptr;
    V1Display* display_ = nullptr;
    LockoutEnforcer* enforcer_ = nullptr;
    LockoutIndex* index_ = nullptr;
    SignalCaptureModule* sigCapture_ = nullptr;
    SystemEventBus* eventBus_ = nullptr;
    PerfCounters* perfCounters_ = nullptr;
    TimeService* timeSvc_ = nullptr;
    QuietCoordinatorModule* quiet_ = nullptr;
    SpeedSourceSelector* speedSelector_ = nullptr;
};

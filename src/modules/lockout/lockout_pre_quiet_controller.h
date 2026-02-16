#pragma once

#include <stdint.h>

/// Persistent state for the GPS lockout pre-quiet feature.
/// Tracks whether volume has been proactively dropped and the
/// original volume settings to restore.
struct PreQuietState {
    bool preQuietActive = false;        // Currently in pre-quieted state
    uint8_t savedMainVolume = 0xFF;     // Volume to restore (0xFF = not captured)
    uint8_t savedMuteVolume = 0;        // Mute volume to restore
    uint32_t enteredZoneMs = 0;         // When we first saw a nearby zone (entry debounce)
    uint32_t leftZoneMs = 0;            // When nearby count dropped to 0 (exit debounce)
};

/// Decision returned by evaluatePreQuiet().  Caller executes via setVolume().
struct PreQuietDecision {
    enum Action : uint8_t {
        NONE = 0,
        DROP_VOLUME,        // Drop to muted volume (entering lockout zone)
        RESTORE_VOLUME      // Restore original volume (leaving zone or real alert)
    };
    Action action = NONE;
    uint8_t volume = 0;        // Target main volume for DROP/RESTORE
    uint8_t muteVolume = 0;    // Target mute volume for DROP/RESTORE
};

/// Pure-function evaluator for lockout zone pre-quiet volume management.
///
/// When GPS enters a lockout zone and no alert is active, proactively drops
/// V1 volume to (muteVolume, muteVolume) so any false-alert beep is quiet.
/// Restores volume immediately if a non-lockout alert fires, or after
/// leaving the zone with debounce.
///
/// Thread safety: designed for single-threaded access from loop().
PreQuietDecision evaluatePreQuiet(
    bool featureEnabled,                // gpsLockoutPreQuiet setting
    bool enforceMode,                   // gpsLockoutMode == LOCKOUT_RUNTIME_ENFORCE
    bool bleConnected,
    bool hasAlert,                      // parser.hasAlerts()
    bool lockoutEvaluated,              // lockRes.evaluated
    bool lockoutShouldMute,             // lockRes.shouldMute (valid when alert + evaluated)
    size_t nearbyZoneCount,             // findNearby() result count
    uint8_t currentMainVolume,          // from DisplayState.mainVolume
    uint8_t currentMuteVolume,          // from DisplayState.muteVolume
    uint32_t nowMs,
    PreQuietState& state);

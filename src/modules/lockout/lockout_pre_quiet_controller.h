#pragma once

#include <stdint.h>

/// Three-state machine for lockout pre-quiet volume management.
///   IDLE     → no volume override active.
///   DROPPED  → volume proactively lowered to mute-volume in a lockout zone.
///   DISARMED → real alert fired, volume restored; stay alert until all zones cleared.
enum class PreQuietPhase : uint8_t {
    IDLE = 0,
    DROPPED,
    DISARMED
};

/// Persistent state for the GPS lockout pre-quiet feature.
struct PreQuietState {
    PreQuietPhase phase = PreQuietPhase::IDLE;
    uint8_t savedMainVolume = 0xFF;     // Volume to restore (0xFF = not captured)
    uint8_t savedMuteVolume = 0;        // Mute volume to restore
    uint32_t enteredZoneMs = 0;         // When we first saw a nearby zone (entry debounce)
    uint32_t leftZoneMs = 0;            // When nearby count dropped to 0 (exit debounce)
    uint32_t droppedAtMs = 0;           // When DROPPED phase began (safety timeout)
    uint32_t gpsLostMs = 0;            // When GPS first lost while DROPPED (GPS-loss debounce)
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
/// "Drop once, restore on real threat, stay alert."
///
///   IDLE → enter lockout zone (200ms debounce) → DROP_VOLUME → DROPPED
///   DROPPED + Ka/Laser active                 → RESTORE_VOLUME → DISARMED
///   DROPPED + lockout-matched alert            → NONE (mute controller handles)
///   DROPPED + real alert (even GPS lost)       → RESTORE_VOLUME → DISARMED
///   DROPPED + leave all zones (500ms debounce) → RESTORE_VOLUME → IDLE
///   DROPPED + GPS fix lost >5 s                → RESTORE_VOLUME → IDLE
///   DROPPED + held >60 s                       → RESTORE_VOLUME → IDLE (safety timeout)
///   DISARMED + leave all zones (GPS valid)     → IDLE (no BLE command needed)
///   Any phase + GPS fix lost                   → hold state (like BLE disconnect)
///   Any phase + feature/mode/BLE disabled      → restore if DROPPED, reset
///
/// Max 2 BLE commands per zone visit.  No re-drop after real alert.
///
/// Thread safety: designed for single-threaded access from loop().
PreQuietDecision evaluatePreQuiet(
    bool featureEnabled,                // gpsLockoutPreQuiet setting
    bool enforceMode,                   // gpsLockoutMode == LOCKOUT_RUNTIME_ENFORCE
    bool bleConnected,
    bool gpsValid,                      // gpsStatus.locationValid — false = hold state
    bool hasAlert,                      // parser.hasAlerts()
    bool lockoutEvaluated,              // lockRes.evaluated
    bool lockoutShouldMute,             // lockRes.shouldMute (valid when alert + evaluated)
    size_t nearbyZoneCount,             // findNearby() result count
    uint8_t currentMainVolume,          // from DisplayState.mainVolume
    uint8_t currentMuteVolume,          // from DisplayState.muteVolume
    uint32_t nowMs,
    PreQuietState& state,
    bool hasKaOrLaser = false);         // Ka/Laser band active — always restores volume

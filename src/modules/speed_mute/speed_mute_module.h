// Speed-Aware Muting Module
//
// Suppresses audio alerts (voice + V1 mute command) below a configurable speed
// threshold.  Inspired by JBV1 / Highway Radar / Spectre Nav low-speed muting.
//
// Design rules:
//   - Fail-open: if speed source is lost, NEVER mute (safety first)
//   - Hysteresis: unmute threshold = threshold + hysteresis to prevent cycling
//   - Band overrides: Laser (and optionally Ka) always bypass low-speed mute
//   - Best-effort (Priority tier 4): never blocks BLE/display/connectivity
//   - Pure decision function: caller owns BLE commands & voice suppression

#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Settings snapshot (read from V1Settings each loop)
// ---------------------------------------------------------------------------

struct SpeedMuteSettings {
    bool enabled = false;
    uint8_t thresholdMph = 25;       // Mute below this speed (5–60 mph)
    uint8_t hysteresisMph = 3;       // Unmute at threshold + hysteresis
    bool overrideLaser = true;       // Always alert on Laser regardless of speed
    bool overrideKa = false;         // Always alert on Ka regardless of speed
};

// ---------------------------------------------------------------------------
// Input context — populated by caller each loop iteration
// ---------------------------------------------------------------------------

struct SpeedMuteContext {
    float speedMph = 0.0f;          // Current arbitrated speed (SpeedSourceSelector)
    bool speedValid = false;         // Speed source is fresh & trusted
    uint32_t nowMs = 0;
};

// ---------------------------------------------------------------------------
// Decision output
// ---------------------------------------------------------------------------

struct SpeedMuteDecision {
    bool shouldMute = false;         // True → suppress audio (voice + V1 mute cmd)
};

// ---------------------------------------------------------------------------
// Persistent state (owned by module instance, mutated by evaluate())
// ---------------------------------------------------------------------------

struct SpeedMuteState {
    bool muteActive = false;         // Current muted state (with hysteresis applied)
    uint32_t lastTransitionMs = 0;   // Timestamp of last mute/unmute transition
};

// ---------------------------------------------------------------------------
// Pure decision function (testable, no side effects beyond state mutation)
// ---------------------------------------------------------------------------

SpeedMuteDecision evaluateSpeedMute(
    const SpeedMuteSettings& settings,
    const SpeedMuteContext& ctx,
    SpeedMuteState& state);

// ---------------------------------------------------------------------------
// Module wrapper — convenience for main-loop wiring
// ---------------------------------------------------------------------------

class SpeedMuteModule {
public:
    void begin(bool enabled, uint8_t thresholdMph, uint8_t hysteresisMph,
               bool overrideLaser, bool overrideKa);

    /// Update settings at runtime (from web UI / settings sync).
    void syncSettings(bool enabled, uint8_t thresholdMph, uint8_t hysteresisMph,
                      bool overrideLaser, bool overrideKa);

    /// Evaluate muting decision.  Call once per loop iteration.
    SpeedMuteDecision update(float speedMph, bool speedValid, uint32_t nowMs);

    /// Query whether a specific band is exempt from speed-mute.
    bool isBandOverridden(uint8_t band) const;

    const SpeedMuteSettings& getSettings() const { return settings_; }
    const SpeedMuteState& getState() const { return state_; }

private:
    SpeedMuteSettings settings_;
    SpeedMuteState state_;
};

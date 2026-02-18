#include "lockout_pre_quiet_controller.h"

/// Entry debounce: require GPS to report inside a lockout zone for this
/// many ms before dropping volume.  Prevents single-frame noise on zone edges.
static constexpr uint32_t ENTRY_DEBOUNCE_MS = 100;

/// Exit debounce: after leaving all lockout zones, wait this long before
/// restoring volume.  Prevents flip-flopping at zone boundaries.
static constexpr uint32_t EXIT_DEBOUNCE_MS = 500;

// Helper: build a RESTORE decision from saved state.
static PreQuietDecision restoreFrom(const PreQuietState& state) {
    PreQuietDecision d;
    d.action   = PreQuietDecision::RESTORE_VOLUME;
    d.volume   = state.savedMainVolume;
    d.muteVolume = state.savedMuteVolume;
    return d;
}

PreQuietDecision evaluatePreQuiet(
    bool featureEnabled,
    bool enforceMode,
    bool bleConnected,
    bool gpsValid,
    bool hasAlert,
    bool lockoutEvaluated,
    bool lockoutShouldMute,
    size_t nearbyZoneCount,
    uint8_t currentMainVolume,
    uint8_t currentMuteVolume,
    uint32_t nowMs,
    PreQuietState& state) {

    PreQuietDecision decision;

    // --- Gate: feature must be on and enforce mode ---
    if (!featureEnabled || !enforceMode) {
        if (state.phase == PreQuietPhase::DROPPED && bleConnected) {
            decision = restoreFrom(state);
        }
        state = PreQuietState{};
        return decision;
    }

    // BLE down — can't send commands.  Keep state so we resume correctly
    // when BLE reconnects.  The V1 retains whatever volume it has.
    if (!bleConnected) {
        return decision;  // NONE
    }

    // GPS validity affects zone-presence logic (entry/exit debounce) but must
    // NOT block the real-alert restore path.  A real alert while DROPPED must
    // always restore volume regardless of GPS state — safety trumps position.
    //
    // When GPS is invalid, nearbyZoneCount is 0 (findNearby not called).
    // Treat that as "position unknown" rather than "left the zone."
    const bool inZone = gpsValid ? (nearbyZoneCount > 0) : false;
    const bool positionKnown = gpsValid;

    switch (state.phase) {

    // ── IDLE: not active, watch for zone entry ──────────────────────
    case PreQuietPhase::IDLE:
        if (!positionKnown) {
            state.enteredZoneMs = 0;  // Can't trust zone data, reset debounce
            return decision;  // NONE
        }
        if (inZone && !hasAlert) {
            // Start or continue entry debounce.
            if (state.enteredZoneMs == 0) {
                state.enteredZoneMs = nowMs;
                return decision;  // NONE — wait for debounce
            }
            if ((nowMs - state.enteredZoneMs) < ENTRY_DEBOUNCE_MS) {
                return decision;  // NONE — still debouncing
            }
            // Debounce elapsed — capture volumes and drop.
            state.savedMainVolume = currentMainVolume;
            state.savedMuteVolume = currentMuteVolume;
            state.phase = PreQuietPhase::DROPPED;
            state.enteredZoneMs = 0;
            decision.action = PreQuietDecision::DROP_VOLUME;
            decision.volume = currentMuteVolume;
            decision.muteVolume = currentMuteVolume;
            return decision;
        }
        // Not in zone or alert already present — reset entry timer.
        state.enteredZoneMs = 0;
        return decision;

    // ── DROPPED: volume lowered, waiting for zone exit or real alert ─
    case PreQuietPhase::DROPPED:
        // Real alert (non-lockout match) → restore immediately, go DISARMED.
        // This fires regardless of GPS state — safety over position.
        if (hasAlert && lockoutEvaluated && !lockoutShouldMute) {
            decision = restoreFrom(state);
            state.phase = PreQuietPhase::DISARMED;
            state.leftZoneMs = 0;
            return decision;
        }
        // Still in zone with lockout-matched alert → stay quiet.
        if (hasAlert && lockoutEvaluated && lockoutShouldMute) {
            state.leftZoneMs = 0;
            return decision;  // NONE — mute controller handles
        }
        // GPS lost — hold DROPPED, cancel any exit debounce.
        if (!positionKnown) {
            state.leftZoneMs = 0;
            return decision;  // NONE — can't evaluate zone presence
        }
        // Still in zone, no alert → hold.
        if (inZone) {
            state.leftZoneMs = 0;
            return decision;  // NONE
        }
        // Left zone, no alert → exit debounce then restore.
        if (state.leftZoneMs == 0) {
            state.leftZoneMs = nowMs;
            return decision;  // NONE — start exit debounce
        }
        if ((nowMs - state.leftZoneMs) < EXIT_DEBOUNCE_MS) {
            return decision;  // NONE — still debouncing
        }
        // Debounce elapsed — restore and go IDLE.
        decision = restoreFrom(state);
        state = PreQuietState{};
        return decision;

    // ── DISARMED: real alert fired, volume restored, stay alert ──────
    case PreQuietPhase::DISARMED:
        // Stay in DISARMED until GPS is valid and all lockout zones are cleared.
        // Do not re-drop — a real signal means stay at full volume.
        if (positionKnown && !inZone) {
            state = PreQuietState{};  // → IDLE
        }
        return decision;  // NONE — never send BLE commands in DISARMED
    }

    return decision;  // unreachable, keeps compiler happy
}

#include "lockout_pre_quiet_controller.h"

/// Entry debounce: require GPS to report inside a lockout zone for this
/// many ms before dropping volume.  Prevents single-frame noise on zone edges.
static constexpr uint32_t ENTRY_DEBOUNCE_MS = 200;

/// Exit debounce: after leaving all lockout zones, wait this long before
/// restoring volume.  Prevents flip-flopping at zone boundaries.
static constexpr uint32_t EXIT_DEBOUNCE_MS = 500;

PreQuietDecision evaluatePreQuiet(
    bool featureEnabled,
    bool enforceMode,
    bool bleConnected,
    bool hasAlert,
    bool lockoutEvaluated,
    bool lockoutShouldMute,
    size_t nearbyZoneCount,
    uint8_t currentMainVolume,
    uint8_t currentMuteVolume,
    uint32_t nowMs,
    PreQuietState& state) {

    PreQuietDecision decision;

    // --- Gate: feature must be on, enforce mode, BLE up ---
    if (!featureEnabled || !enforceMode || !bleConnected) {
        if (state.preQuietActive) {
            // Restore volume before disabling.
            decision.action = PreQuietDecision::RESTORE_VOLUME;
            decision.volume = state.savedMainVolume;
            decision.muteVolume = state.savedMuteVolume;
            state = PreQuietState{};
        }
        return decision;
    }

    const bool inZone = nearbyZoneCount > 0;

    // --- Case 1: In zone, no alert → drop volume (with entry debounce) ---
    if (inZone && !hasAlert) {
        // Reset exit timer when we're still in a zone.
        state.leftZoneMs = 0;

        if (!state.preQuietActive) {
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
            state.preQuietActive = true;
            decision.action = PreQuietDecision::DROP_VOLUME;
            decision.volume = currentMuteVolume;
            decision.muteVolume = currentMuteVolume;
            return decision;
        }
        // Already pre-quieted and still in zone with no alert → no action.
        return decision;
    }

    // --- Case 2: Has alert and lockout match → stay quiet, let mute controller handle ---
    if (hasAlert && lockoutEvaluated && lockoutShouldMute) {
        // Reset exit timer — we're actively in a lockout-matched alert.
        state.leftZoneMs = 0;
        return decision;  // NONE
    }

    // --- Case 3: Has alert but NOT a lockout match → real threat, restore immediately ---
    if (hasAlert && lockoutEvaluated && !lockoutShouldMute && state.preQuietActive) {
        decision.action = PreQuietDecision::RESTORE_VOLUME;
        decision.volume = state.savedMainVolume;
        decision.muteVolume = state.savedMuteVolume;
        state = PreQuietState{};
        return decision;
    }

    // --- Case 4: Left zone, no alert → exit debounce then restore ---
    if (!inZone && !hasAlert && state.preQuietActive) {
        state.enteredZoneMs = 0;  // Reset entry timer.
        if (state.leftZoneMs == 0) {
            state.leftZoneMs = nowMs;
            return decision;  // NONE — start exit debounce
        }
        if ((nowMs - state.leftZoneMs) < EXIT_DEBOUNCE_MS) {
            return decision;  // NONE — still debouncing
        }
        // Debounce elapsed — restore.
        decision.action = PreQuietDecision::RESTORE_VOLUME;
        decision.volume = state.savedMainVolume;
        decision.muteVolume = state.savedMuteVolume;
        state = PreQuietState{};
        return decision;
    }

    // --- Case 5: Not in zone and not active → reset entry timer ---
    if (!inZone) {
        state.enteredZoneMs = 0;
        state.leftZoneMs = 0;
    }

    return decision;
}

#include "lockout_enforcer.h"
#include "lockout_store.h"

#include "../../packet_parser.h"
#include "../../settings.h"

#ifndef UNIT_TEST
#include "../gps/gps_runtime_module.h"
#include <Arduino.h>
#else
#include "../../../test/mocks/Arduino.h"
#endif

#include <cmath>

namespace {

int32_t degreesToE5(float degrees) {
    return static_cast<int32_t>(lroundf(degrees * 100000.0f));
}

}  // namespace

LockoutEnforcer lockoutEnforcer;

void LockoutEnforcer::begin(const SettingsManager* settings,
                            LockoutIndex* index,
                            LockoutStore* store) {
    settings_ = settings;
    index_    = index;
    store_    = store;
    lastResult_ = LockoutEnforcerResult{};
    stats_ = Stats{};
    lastLogMs_ = 0;
    lastCleanPassEpochMs_ = 0;
}

LockoutEnforcerResult LockoutEnforcer::process(uint32_t nowMs,
                                               int64_t epochMs,
                                               const PacketParser& parser,
                                               const GpsRuntimeStatus& gpsStatus) {
    lastResult_ = LockoutEnforcerResult{};

    // --- Gate 1: mode check ---
    if (!settings_ || !index_) {
        return lastResult_;
    }
    const LockoutRuntimeMode mode = settings_->get().gpsLockoutMode;
    lastResult_.mode = static_cast<uint8_t>(mode);

    if (mode == LOCKOUT_RUNTIME_OFF) {
        ++stats_.skippedOff;
        return lastResult_;
    }

    // --- Gate 2: GPS validity ---
    if (!gpsStatus.hasFix) {
        ++stats_.skippedNoFix;
        return lastResult_;
    }
    if (!gpsStatus.locationValid ||
        !std::isfinite(gpsStatus.latitudeDeg) ||
        !std::isfinite(gpsStatus.longitudeDeg)) {
        ++stats_.skippedNoGps;
        return lastResult_;
    }

    const int32_t latE5 = degreesToE5(gpsStatus.latitudeDeg);
    const int32_t lonE5 = degreesToE5(gpsStatus.longitudeDeg);

    // --- Gate 3: alert present ---
    if (!parser.hasAlerts()) {
        lastResult_.evaluated = true;
        ++stats_.evaluations;

        // No alert → clean-pass opportunity for nearby lockout zones.
        if (mode == LOCKOUT_RUNTIME_ENFORCE) {
            recordCleanPasses(latE5, lonE5, -1, epochMs);
        }
        return lastResult_;
    }
    const AlertData priority = parser.getPriorityAlert();
    if (!priority.isValid || priority.band == BAND_NONE) {
        lastResult_.evaluated = true;
        ++stats_.evaluations;
        return lastResult_;
    }

    // --- Evaluate ---
    const uint8_t band  = static_cast<uint8_t>(priority.band);
    const uint16_t freqMHz = static_cast<uint16_t>(
        (priority.frequency <= UINT16_MAX) ? priority.frequency : 0);

    const LockoutDecision decision = index_->evaluate(latE5, lonE5, band, freqMHz);

    lastResult_.evaluated  = true;
    lastResult_.shouldMute = decision.shouldMute;
    lastResult_.matchIndex = decision.matchIndex;
    lastResult_.confidence = decision.confidence;
    ++stats_.evaluations;

    if (decision.shouldMute) {
        ++stats_.matches;

        // Only ENFORCE mode mutates index state (confidence + lastSeenMs).
        // SHADOW and ADVISORY are read-only so toggling them has no side-effects.
        if (mode == LOCKOUT_RUNTIME_ENFORCE) {
            index_->recordHit(static_cast<size_t>(decision.matchIndex), epochMs);
            if (store_) store_->markDirty();
        }

        // Rate-limited log for SHADOW / ADVISORY / ENFORCE.
        if (nowMs - lastLogMs_ >= LOG_INTERVAL_MS) {
            lastLogMs_ = nowMs;
            Serial.printf("[Lockout] MATCH mode=%s slot=%d conf=%u band=%u freq=%u lat=%ld lon=%ld\n",
                          lockoutRuntimeModeName(mode),
                          decision.matchIndex,
                          decision.confidence,
                          band,
                          freqMHz,
                          static_cast<long>(latE5),
                          static_cast<long>(lonE5));
        }
    }

    return lastResult_;
}
// ---------------------------------------------------------------------------
// Clean-pass recording for demotion (ENFORCE only).
//
// When no alert is present and we're within range of lockout entries,
// those entries receive a clean-pass decrement.  Rate-limited to once
// per CLEAN_PASS_INTERVAL_MS (~30s) so driving through a 150m zone at
// 60 mph (5.6s transit) records at most one clean pass per transit.
// ---------------------------------------------------------------------------

void LockoutEnforcer::recordCleanPasses(int32_t latE5, int32_t lonE5,
                                        int16_t matchedSlot, int64_t epochMs) {
    if (!index_ || epochMs <= 0) return;

    // Rate-limit using epoch delta (ms precision, monotonic once synced).
    if (lastCleanPassEpochMs_ != 0 &&
        (epochMs - lastCleanPassEpochMs_) < CLEAN_PASS_INTERVAL_MS) {
        return;
    }
    lastCleanPassEpochMs_ = epochMs;

    // Find all entries whose zone covers our current position.
    int16_t nearby[16];
    const size_t count = index_->findNearby(latE5, lonE5, nearby, 16);

    bool anyMutated = false;
    for (size_t i = 0; i < count; ++i) {
        if (nearby[i] == matchedSlot) continue;  // Skip matched entry.

        const uint8_t conf = index_->recordCleanPass(static_cast<size_t>(nearby[i]), epochMs);
        ++stats_.cleanPasses;
        anyMutated = true;

        if (conf == 0) {
            ++stats_.demotions;
            Serial.printf("[Lockout] DEMOTED slot=%d (clean-pass decay)\n", nearby[i]);
        }
    }

    if (anyMutated && store_) {
        store_->markDirty();
    }
}
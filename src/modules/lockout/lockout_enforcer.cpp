#include "lockout_enforcer.h"

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
                            LockoutIndex* index) {
    settings_ = settings;
    index_    = index;
    lastResult_ = LockoutEnforcerResult{};
    stats_ = Stats{};
    lastLogMs_ = 0;
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

    // --- Gate 3: alert present ---
    if (!parser.hasAlerts()) {
        // No alert to evaluate — still counts as an evaluation cycle.
        lastResult_.evaluated = true;
        ++stats_.evaluations;
        return lastResult_;
    }
    const AlertData priority = parser.getPriorityAlert();
    if (!priority.isValid || priority.band == BAND_NONE) {
        lastResult_.evaluated = true;
        ++stats_.evaluations;
        return lastResult_;
    }

    // --- Evaluate ---
    const int32_t latE5 = degreesToE5(gpsStatus.latitudeDeg);
    const int32_t lonE5 = degreesToE5(gpsStatus.longitudeDeg);
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

        // Record the hit on the matching entry (updates lastSeenMs + confidence).
        index_->recordHit(static_cast<size_t>(decision.matchIndex), epochMs);

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

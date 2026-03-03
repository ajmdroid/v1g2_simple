#include "lockout_index.h"
#include "lockout_band_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>   // abs
#include <cstring>   // memset

LockoutIndex lockoutIndex;

void LockoutIndex::clear() {
    for (size_t i = 0; i < kCapacity; ++i) {
        entries_[i].clear();
    }
}

LockoutDecision LockoutIndex::evaluate(int32_t latE5,
                                       int32_t lonE5,
                                       uint8_t band,
                                       uint16_t freqMHz,
                                       bool courseValid,
                                       float courseDeg) const {
    LockoutDecision decision;
    const uint8_t alertBandMask = lockoutSanitizeBandMask(band);
    if (alertBandMask == 0) {
        return decision;
    }
    for (size_t i = 0; i < kCapacity; ++i) {
        const LockoutEntry& e = entries_[i];
        if (!e.isActive()) {
            continue;
        }
        const uint8_t entryBandMask = lockoutSanitizeBandMask(e.bandMask);
        if (entryBandMask == 0) {
            continue;
        }
        // Band filter: entry's bandMask must include the alert's band bit.
        if ((entryBandMask & alertBandMask) == 0) {
            continue;
        }
        if (!freqMatches(freqMHz, e)) {
            continue;
        }
        if (!courseMatches(courseValid, courseDeg, e)) {
            continue;
        }
        if (!withinRadius(latE5, lonE5, e)) {
            continue;
        }
        // Match found.
        decision.shouldMute = true;
        decision.matchIndex = static_cast<int16_t>(i);
        decision.confidence = e.confidence;
        return decision;
    }
    return decision;
}

int LockoutIndex::add(const LockoutEntry& entry) {
    LockoutEntry sanitized = entry;
    sanitized.bandMask = lockoutSanitizeBandMask(sanitized.bandMask);
    if (sanitized.bandMask == 0) {
        return -1;
    }
    for (size_t i = 0; i < kCapacity; ++i) {
        if (!entries_[i].isActive()) {
            entries_[i] = sanitized;
            entries_[i].setActive(true);
            return static_cast<int>(i);
        }
    }
    // No free slot — try eviction.
    int victim = findEvictionCandidate();
    if (victim >= 0) {
        entries_[victim] = sanitized;
        entries_[victim].setActive(true);
        return victim;
    }
    return -1;  // All slots are un-evictable (manual).
}

int LockoutIndex::addOrUpdate(const LockoutEntry& entry) {
    LockoutEntry sanitized = entry;
    sanitized.bandMask = lockoutSanitizeBandMask(sanitized.bandMask);
    if (sanitized.bandMask == 0) {
        return -1;
    }
    // Check for an existing entry that covers the same zone+band+freq.
    const int existing = findMatch(sanitized.latE5, sanitized.lonE5,
                                   sanitized.bandMask, sanitized.freqMHz);
    if (existing >= 0) {
        LockoutEntry& e = entries_[existing];
        e.bandMask = lockoutSanitizeBandMask(static_cast<uint8_t>(e.bandMask | sanitized.bandMask));
        // Merge: keep the higher confidence.
        if (sanitized.confidence > e.confidence) {
            e.confidence = sanitized.confidence;
        }
        // Keep the earliest firstSeenMs.
        if (sanitized.firstSeenMs > 0 &&
            (e.firstSeenMs == 0 || sanitized.firstSeenMs < e.firstSeenMs)) {
            e.firstSeenMs = sanitized.firstSeenMs;
        }
        // Keep the latest lastSeenMs.
        if (sanitized.lastSeenMs > e.lastSeenMs) {
            e.lastSeenMs = sanitized.lastSeenMs;
        }
        // Existing-zone refresh should clear miss streak.
        e.missCount = 0;
        e.lastCountedMissMs = 0;
        // Merge flags (preserve both manual + learned if applicable).
        e.flags |= sanitized.flags;
        return existing;
    }
    // No existing match — insert at the first free slot.
    return add(sanitized);
}

bool LockoutIndex::remove(size_t index) {
    if (index >= kCapacity) {
        return false;
    }
    entries_[index].clear();
    return true;
}

const LockoutEntry* LockoutIndex::at(size_t index) const {
    if (index >= kCapacity) {
        return nullptr;
    }
    return &entries_[index];
}

LockoutEntry* LockoutIndex::mutableAt(size_t index) {
    if (index >= kCapacity) {
        return nullptr;
    }
    return &entries_[index];
}

size_t LockoutIndex::activeCount() const {
    size_t count = 0;
    for (size_t i = 0; i < kCapacity; ++i) {
        if (entries_[i].isActive()) {
            ++count;
        }
    }
    return count;
}

uint8_t LockoutIndex::recordCleanPass(size_t index, int64_t epochMs) {
    if (index >= kCapacity) {
        return 0;
    }
    LockoutEntry& e = entries_[index];
    if (!e.isActive()) {
        return 0;
    }
    if (epochMs > 0) {
        e.lastPassMs = epochMs;
    }
    // Decay confidence.  Manual entries floor at 0 but stay active.
    if (e.confidence > 0) {
        --e.confidence;
    }
    if (e.confidence == 0 && !e.isManual()) {
        e.clear();  // Auto-remove any non-manual entry at zero confidence.
    }
    return e.confidence;
}

LockoutCleanPassResult LockoutIndex::recordCleanPassWithPolicy(size_t index,
                                                               int64_t epochMs,
                                                               uint32_t missIntervalMs,
                                                               uint8_t missThreshold) {
    LockoutCleanPassResult result;
    if (index >= kCapacity) {
        return result;
    }

    LockoutEntry& e = entries_[index];
    if (!e.isActive()) {
        return result;
    }

    if (missThreshold == 0) {
        const bool wasActive = e.isActive();
        result.confidence = recordCleanPass(index, epochMs);
        result.counted = true;
        result.demoted = wasActive && !entries_[index].isActive();
        return result;
    }

    if (epochMs <= 0) {
        result.confidence = e.confidence;
        return result;
    }

    if (missIntervalMs > 0 && e.lastCountedMissMs > 0 &&
        (epochMs - e.lastCountedMissMs) < static_cast<int64_t>(missIntervalMs)) {
        result.confidence = e.confidence;
        return result;
    }

    e.lastPassMs = epochMs;
    if (e.missCount < 255) {
        ++e.missCount;
    }
    e.lastCountedMissMs = epochMs;
    result.counted = true;

    if (e.missCount >= missThreshold) {
        e.clear();
        result.confidence = 0;
        result.demoted = true;
        return result;
    }

    result.confidence = e.confidence;
    return result;
}

uint8_t LockoutIndex::recordHit(size_t index, int64_t epochMs) {
    if (index >= kCapacity) {
        return 0;
    }
    LockoutEntry& e = entries_[index];
    if (!e.isActive()) {
        return 0;
    }
    if (epochMs > 0) {
        e.lastSeenMs = epochMs;
    }
    if (e.confidence < 255) {
        ++e.confidence;
    }
    e.missCount = 0;
    e.lastCountedMissMs = 0;
    return e.confidence;
}

int LockoutIndex::findMatch(int32_t latE5,
                            int32_t lonE5,
                            uint8_t band,
                            uint16_t freqMHz,
                            bool courseValid,
                            float courseDeg) const {
    const uint8_t alertBandMask = lockoutSanitizeBandMask(band);
    if (alertBandMask == 0) {
        return -1;
    }
    for (size_t i = 0; i < kCapacity; ++i) {
        const LockoutEntry& e = entries_[i];
        if (!e.isActive()) {
            continue;
        }
        const uint8_t entryBandMask = lockoutSanitizeBandMask(e.bandMask);
        if (entryBandMask == 0) {
            continue;
        }
        if ((entryBandMask & alertBandMask) == 0) {
            continue;
        }
        if (!freqMatches(freqMHz, e)) {
            continue;
        }
        if (!courseMatches(courseValid, courseDeg, e)) {
            continue;
        }
        if (!withinRadius(latE5, lonE5, e)) {
            continue;
        }
        return static_cast<int>(i);
    }
    return -1;
}

size_t LockoutIndex::findNearby(int32_t latE5,
                                int32_t lonE5,
                                int16_t* out,
                                size_t outCap) const {
    size_t found = 0;
    if (!out || outCap == 0) return 0;
    for (size_t i = 0; i < kCapacity && found < outCap; ++i) {
        const LockoutEntry& e = entries_[i];
        if (!e.isActive()) continue;
        if (!withinRadius(latE5, lonE5, e)) continue;
        out[found++] = static_cast<int16_t>(i);
    }
    return found;
}

size_t LockoutIndex::findNearbyInflated(int32_t latE5,
                                        int32_t lonE5,
                                        uint16_t bufferE5,
                                        int16_t* out,
                                        size_t outCap) const {
    if (bufferE5 == 0) return findNearby(latE5, lonE5, out, outCap);
    size_t found = 0;
    if (!out || outCap == 0) return 0;
    for (size_t i = 0; i < kCapacity && found < outCap; ++i) {
        const LockoutEntry& e = entries_[i];
        if (!e.isActive()) continue;
        if (!withinInflatedRadius(latE5, lonE5, e, bufferE5)) continue;
        out[found++] = static_cast<int16_t>(i);
    }
    return found;
}

size_t LockoutIndex::findNearbyDirectional(int32_t latE5,
                                           int32_t lonE5,
                                           bool courseValid,
                                           float courseDeg,
                                           uint16_t bufferE5,
                                           int16_t* out,
                                           size_t outCap) const {
    // Fast path: no buffer → identical to findNearby().
    if (bufferE5 == 0) return findNearby(latE5, lonE5, out, outCap);

    // No valid course → fall back to symmetric inflation.
    if (!courseValid || !std::isfinite(courseDeg)) {
        return findNearbyInflated(latE5, lonE5, bufferE5, out, outCap);
    }

    size_t found = 0;
    if (!out || outCap == 0) return 0;

    for (size_t i = 0; i < kCapacity && found < outCap; ++i) {
        const LockoutEntry& e = entries_[i];
        if (!e.isActive()) continue;

        bool hit = false;

        if (e.directionMode == LockoutEntry::DIRECTION_ALL ||
            e.headingDeg == LockoutEntry::HEADING_INVALID ||
            e.headingDeg >= 360) {
            // Omni-directional or no heading data → symmetric inflation.
            hit = withinInflatedRadius(latE5, lonE5, e, bufferE5);
        } else {
            // Zone has a heading.  Check if our course matches the zone
            // direction (within tolerance).  If not, we won't get muted
            // for this zone, so don't inflate.
            if (!courseMatches(courseValid, courseDeg, e)) {
                hit = withinRadius(latE5, lonE5, e);
            } else {
                // Course matches.  Determine approach vs departure using
                // the dot product of (device − zone) onto the zone heading.
                //   dot < 0 → device is upstream (approaching)  → inflate
                //   dot ≥ 0 → device is downstream (past zone)  → no inflate
                const float headRad =
                    static_cast<float>(e.headingDeg) * (M_PI / 180.0f);
                const float cosH = cosf(headRad);  // north component
                const float sinH = sinf(headRad);  // east  component
                const float dLat = static_cast<float>(latE5 - e.latE5);
                const float dLon = static_cast<float>(lonE5 - e.lonE5);
                const float dot  = dLat * cosH + dLon * sinH;

                if (dot < 0.0f) {
                    // Approaching — inflate to trigger pre-quiet early.
                    hit = withinInflatedRadius(latE5, lonE5, e, bufferE5);
                } else {
                    // Past the zone center — base radius only.
                    hit = withinRadius(latE5, lonE5, e);
                }
            }
        }

        if (hit) {
            out[found++] = static_cast<int16_t>(i);
        }
    }
    return found;
}

// --- Eviction ---

int LockoutIndex::findEvictionCandidate() const {
    int best = -1;
    uint8_t bestConf = 255;
    int64_t bestLastSeen = INT64_MAX;

    for (size_t i = 0; i < kCapacity; ++i) {
        const LockoutEntry& e = entries_[i];
        if (!e.isActive()) continue;
        if (e.isManual()) continue;            // Never evict manual zones.
        if (!e.isLearned()) continue;          // Only evict auto-learned zones.

        if (e.confidence < bestConf ||
            (e.confidence == bestConf && e.lastSeenMs < bestLastSeen)) {
            best = static_cast<int>(i);
            bestConf = e.confidence;
            bestLastSeen = e.lastSeenMs;
        }
    }
    return best;
}

// --- Private helpers ---

bool LockoutIndex::withinRadius(int32_t latE5,
                                int32_t lonE5,
                                const LockoutEntry& entry) {
    // Fast bounding-box pre-filter using integer abs.
    const int32_t dLat = latE5 - entry.latE5;
    const int32_t dLon = lonE5 - entry.lonE5;
    const int32_t radius = static_cast<int32_t>(entry.radiusE5);

    if (dLat > radius || dLat < -radius) {
        return false;
    }
    if (dLon > radius || dLon < -radius) {
        return false;
    }

    // Squared-distance check (avoids sqrt).
    // At mid-latitudes 1 E5 unit ≈ 1.11 m latitude, ~0.85 m longitude (varies).
    // We treat E5 units as isotropic for simplicity — the radius is already
    // tuned conservatively (~150 m ≈ 135 E5) so the error is acceptable.
    const int64_t dLat64 = static_cast<int64_t>(dLat);
    const int64_t dLon64 = static_cast<int64_t>(dLon);
    const int64_t r64    = static_cast<int64_t>(radius);
    return (dLat64 * dLat64 + dLon64 * dLon64) <= (r64 * r64);
}

bool LockoutIndex::withinInflatedRadius(int32_t latE5,
                                        int32_t lonE5,
                                        const LockoutEntry& entry,
                                        uint16_t bufferE5) {
    const int32_t dLat = latE5 - entry.latE5;
    const int32_t dLon = lonE5 - entry.lonE5;
    const int32_t radius = static_cast<int32_t>(entry.radiusE5) + static_cast<int32_t>(bufferE5);

    if (dLat > radius || dLat < -radius) return false;
    if (dLon > radius || dLon < -radius) return false;

    const int64_t dLat64 = static_cast<int64_t>(dLat);
    const int64_t dLon64 = static_cast<int64_t>(dLon);
    const int64_t r64    = static_cast<int64_t>(radius);
    return (dLat64 * dLat64 + dLon64 * dLon64) <= (r64 * r64);
}

bool LockoutIndex::freqMatches(uint16_t alertFreqMHz, const LockoutEntry& entry) {
    if (entry.freqTolMHz == 0 && entry.freqMHz == 0) {
        // No frequency filter — band-only lockout.
        return true;
    }
    const int diff = abs(static_cast<int>(alertFreqMHz) - static_cast<int>(entry.freqMHz));
    return diff <= static_cast<int>(entry.freqTolMHz);
}

namespace {

float normalizeHeadingDeg(float heading) {
    if (!std::isfinite(heading)) {
        return NAN;
    }
    float wrapped = std::fmod(heading, 360.0f);
    if (wrapped < 0.0f) {
        wrapped += 360.0f;
    }
    return wrapped;
}

float headingDeltaDeg(float a, float b) {
    const float da = normalizeHeadingDeg(a);
    const float db = normalizeHeadingDeg(b);
    if (!std::isfinite(da) || !std::isfinite(db)) {
        return NAN;
    }
    float delta = std::fabs(da - db);
    if (delta > 180.0f) {
        delta = 360.0f - delta;
    }
    return delta;
}

}  // namespace

bool LockoutIndex::courseMatches(bool courseValid, float courseDeg, const LockoutEntry& entry) {
    if (entry.directionMode == LockoutEntry::DIRECTION_ALL) {
        return true;
    }

    if (!courseValid || !std::isfinite(courseDeg)) {
        return false;
    }

    if (entry.headingDeg == LockoutEntry::HEADING_INVALID || entry.headingDeg >= 360) {
        return false;
    }

    const float tolerance = static_cast<float>(std::min<uint8_t>(entry.headingTolDeg, 90));
    if (entry.directionMode == LockoutEntry::DIRECTION_FORWARD) {
        const float delta = headingDeltaDeg(courseDeg, static_cast<float>(entry.headingDeg));
        return std::isfinite(delta) && delta <= tolerance;
    }
    if (entry.directionMode == LockoutEntry::DIRECTION_REVERSE) {
        const float reverseHeading =
            normalizeHeadingDeg(static_cast<float>(entry.headingDeg) + 180.0f);
        const float delta = headingDeltaDeg(courseDeg, reverseHeading);
        return std::isfinite(delta) && delta <= tolerance;
    }

    return true;
}

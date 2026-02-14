#include "lockout_index.h"
#include "lockout_band_policy.h"

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
                                       uint16_t freqMHz) const {
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
    return -1;  // Full.
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
    return e.confidence;
}

int LockoutIndex::findMatch(int32_t latE5,
                            int32_t lonE5,
                            uint8_t band,
                            uint16_t freqMHz) const {
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
    // tuned conservatively (~150 m ≈ 1350 E5) so the error is acceptable.
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

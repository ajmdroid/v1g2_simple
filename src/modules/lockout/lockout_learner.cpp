#include "lockout_learner.h"
#include "lockout_entry.h"
#include "lockout_index.h"
#include "lockout_store.h"
#include "signal_observation_log.h"

#ifndef UNIT_TEST
#include <Arduino.h>
#else
#include "../../../test/mocks/Arduino.h"
#endif

#include <cstdlib>

LockoutLearner lockoutLearner;

void LockoutLearner::begin(LockoutIndex* index, SignalObservationLog* log) {
    index_ = index;
    log_   = log;
    lastProcessedPublished_ = log ? log->stats().published : 0;
    lastPollMs_   = 0;
    lastPruneMs_  = 0;
    lastLogMs_    = 0;
    stats_ = Stats{};
    for (auto& c : candidates_) {
        c = LearnerCandidate{};
    }
}

void LockoutLearner::process(uint32_t nowMs, int64_t epochMs) {
    if (!index_ || !log_) return;

    // Rate-limit polling (priority 7 — never block higher tiers)
    if (nowMs - lastPollMs_ < kPollIntervalMs) return;
    lastPollMs_ = nowMs;

    // Check for new observations
    const uint32_t published = log_->stats().published;

    if (published == lastProcessedPublished_) {
        // No new observations — still run periodic pruning
        if (epochMs > 0 && nowMs - lastPruneMs_ >= kPruneIntervalMs) {
            lastPruneMs_ = nowMs;
            pruneStale(epochMs);
        }
        return;
    }

    // Compute how many new observations since last poll
    uint32_t newCount = published - lastProcessedPublished_;
    if (newCount > SignalObservationLog::kCapacity) {
        newCount = SignalObservationLog::kCapacity; // Lost some; process what's available
    }
    SignalObservation batch[kBatchSize];
    size_t remainingNew = static_cast<size_t>(newCount);
    while (remainingNew > 0) {
        const size_t fetchCount = (remainingNew < kBatchSize) ? remainingNew : kBatchSize;
        const size_t skipNewest = remainingNew - fetchCount;
        const size_t copied = log_->copyRecentSkip(batch, fetchCount, skipNewest);

        // Process each chunk oldest→newest to preserve deterministic promotion behavior.
        for (size_t i = copied; i > 0; --i) {
            const SignalObservation& obs = batch[i - 1];

            // Gate: valid GPS location required
            if (!obs.locationValid) {
                ++stats_.skippedNoLocation;
                continue;
            }

            ++stats_.observed;

            const int32_t  lat  = obs.latitudeE5;
            const int32_t  lon  = obs.longitudeE5;
            const uint8_t  band = obs.bandRaw;
            const uint16_t freq = obs.frequencyMHz;

            // Gate: already covered by an existing lockout in the index
            if (index_->findMatch(lat, lon, band, freq) >= 0) {
                ++stats_.skippedInIndex;
                continue;
            }

            // Try to match an existing candidate
            int idx = findCandidate(lat, lon, band, freq);
            if (idx >= 0) {
                LearnerCandidate& c = candidates_[idx];
                c.hitCount = (c.hitCount < 255) ? static_cast<uint8_t>(c.hitCount + 1) : 255;
                if (epochMs > 0) c.lastSeenMs = epochMs;

                // Promote when threshold reached
                if (c.hitCount >= kPromotionHits) {
                    promoteCandidate(static_cast<size_t>(idx), epochMs);
                }
            } else {
                // Create new candidate
                int slot = allocCandidate();
                if (slot >= 0) {
                    LearnerCandidate& c = candidates_[slot];
                    c.latE5      = lat;
                    c.lonE5      = lon;
                    c.band       = band;
                    c.freqMHz    = freq;
                    c.hitCount   = 1;
                    c.firstSeenMs = (epochMs > 0) ? epochMs : 0;
                    c.lastSeenMs  = (epochMs > 0) ? epochMs : 0;
                    c.active     = true;
                    ++stats_.candidatesCreated;
                }
                // allocCandidate() returns -1 when table is full — silently skip.
                // Stale pruning will free slots eventually.
            }
        }
        if (copied == 0) {
            break;
        }
        remainingNew = skipNewest;
    }

    lastProcessedPublished_ = published;

    // Periodic stale pruning
    if (epochMs > 0 && nowMs - lastPruneMs_ >= kPruneIntervalMs) {
        lastPruneMs_ = nowMs;
        pruneStale(epochMs);
    }

    // Rate-limited summary log
    if (nowMs - lastLogMs_ >= LOG_INTERVAL_MS && stats_.observed > 0) {
        lastLogMs_ = nowMs;
        Serial.printf("[Learner] obs=%lu cand=%lu prom=%lu pruned=%lu active=%u\n",
                      static_cast<unsigned long>(stats_.observed),
                      static_cast<unsigned long>(stats_.candidatesCreated),
                      static_cast<unsigned long>(stats_.promotions),
                      static_cast<unsigned long>(stats_.pruned),
                      static_cast<unsigned>(activeCandidateCount()));
    }
}

// --- Candidate management ---

int LockoutLearner::findCandidate(int32_t latE5, int32_t lonE5,
                                  uint8_t band, uint16_t freqMHz) const {
    for (size_t i = 0; i < kCandidateCapacity; ++i) {
        const LearnerCandidate& c = candidates_[i];
        if (!c.active) continue;
        if (c.band != band) continue;
        if (!freqClose(freqMHz, c.freqMHz, kFreqToleranceMHz)) continue;
        if (!withinRadius(latE5, lonE5, c.latE5, c.lonE5, kRadiusE5)) continue;
        return static_cast<int>(i);
    }
    return -1;
}

int LockoutLearner::allocCandidate() {
    for (size_t i = 0; i < kCandidateCapacity; ++i) {
        if (!candidates_[i].active) return static_cast<int>(i);
    }
    return -1;
}

void LockoutLearner::promoteCandidate(size_t idx, int64_t epochMs) {
    if (idx >= kCandidateCapacity) return;
    const LearnerCandidate& c = candidates_[idx];

    LockoutEntry entry;
    entry.latE5      = c.latE5;
    entry.lonE5      = c.lonE5;
    entry.radiusE5   = kRadiusE5;
    entry.bandMask   = c.band;  // band is already a bitmask (Band enum values)
    entry.freqMHz    = c.freqMHz;
    entry.freqTolMHz = kFreqToleranceMHz;
    entry.confidence = c.hitCount;
    entry.flags      = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.firstSeenMs = c.firstSeenMs;
    entry.lastSeenMs  = (epochMs > 0) ? epochMs : c.lastSeenMs;
    entry.lastPassMs  = 0;

    const int slot = index_->addOrUpdate(entry);
    if (slot >= 0) {
        ++stats_.promotions;
        lockoutStore.markDirty();
        Serial.printf("[Learner] PROMOTED slot=%d band=%u freq=%u lat=%ld lon=%ld hits=%u\n",
                      slot, c.band, c.freqMHz,
                      static_cast<long>(c.latE5), static_cast<long>(c.lonE5),
                      c.hitCount);
        candidates_[idx] = LearnerCandidate{};
    } else {
        ++stats_.promotionsFailed;
        // Index full — candidate stays until slot opens or it ages out.
    }
}

void LockoutLearner::pruneStale(int64_t epochMs) {
    if (epochMs <= 0) return;
    for (size_t i = 0; i < kCandidateCapacity; ++i) {
        LearnerCandidate& c = candidates_[i];
        if (!c.active) continue;
        if (c.lastSeenMs <= 0) continue; // No valid timestamp
        if ((epochMs - c.lastSeenMs) > kStaleDurationMs) {
            c = LearnerCandidate{};
            ++stats_.pruned;
        }
    }
}

// --- Read-only access ---

const LearnerCandidate* LockoutLearner::candidateAt(size_t index) const {
    if (index >= kCandidateCapacity) return nullptr;
    return &candidates_[index];
}

size_t LockoutLearner::activeCandidateCount() const {
    size_t count = 0;
    for (const auto& c : candidates_) {
        if (c.active) ++count;
    }
    return count;
}

// --- Geometry helpers (integer-only, same algorithm as LockoutIndex) ---

bool LockoutLearner::withinRadius(int32_t latE5, int32_t lonE5,
                                  int32_t centerLatE5, int32_t centerLonE5,
                                  uint16_t radiusE5) {
    const int32_t dLat = latE5 - centerLatE5;
    const int32_t dLon = lonE5 - centerLonE5;
    const int32_t r = static_cast<int32_t>(radiusE5);
    // Bounding box pre-check
    if (dLat > r || dLat < -r) return false;
    if (dLon > r || dLon < -r) return false;
    // Squared distance (no sqrt needed)
    const int64_t d2 = static_cast<int64_t>(dLat) * dLat + static_cast<int64_t>(dLon) * dLon;
    const int64_t r2 = static_cast<int64_t>(r) * r;
    return d2 <= r2;
}

bool LockoutLearner::freqClose(uint16_t freqA, uint16_t freqB, uint16_t tolerance) {
    const int diff = static_cast<int>(freqA) - static_cast<int>(freqB);
    return (diff >= 0 ? diff : -diff) <= static_cast<int>(tolerance);
}

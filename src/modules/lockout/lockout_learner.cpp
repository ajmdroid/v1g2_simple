#include "lockout_learner.h"
#include "lockout_band_policy.h"
#include "lockout_entry.h"
#include "lockout_index.h"
#include "lockout_store.h"
#include "signal_observation_log.h"
#include "../../settings.h"

#ifndef UNIT_TEST
#include <Arduino.h>
#else
#include "../../../test/mocks/Arduino.h"
#endif

#include <cstdlib>
#include <cstring>
#include <cmath>

namespace {

uint8_t clampLearnerIntervalHours(uint8_t hours) {
    if (hours == 0) return 0;
    if (hours <= 1) return 1;
    if (hours <= 4) return 4;
    if (hours <= 12) return 12;
    return 24;
}

int64_t intervalHoursToMs(uint8_t hours) {
    return static_cast<int64_t>(hours) * 3600LL * 1000LL;
}

}  // namespace

LockoutLearner lockoutLearner;

void LockoutLearner::begin(LockoutIndex* index, SignalObservationLog* log) {
    index_ = index;
    log_   = log;
    lastProcessedPublished_ = log ? log->stats().published : 0;
    lastPollMs_   = 0;
    lastPruneMs_  = 0;
    lastLogMs_    = 0;
    stats_ = Stats{};
    setTuning(kDefaultPromotionHits,
              kDefaultRadiusE5,
              kDefaultFreqToleranceMHz,
              kDefaultLearnIntervalHours);
    for (auto& c : candidates_) {
        c = LearnerCandidate{};
    }
    dirty_ = false;
}

void LockoutLearner::clearCandidates() {
    for (auto& c : candidates_) {
        c = LearnerCandidate{};
    }
    dirty_ = true;
}

void LockoutLearner::setTuning(uint8_t promotionHits,
                               uint16_t radiusE5,
                               uint16_t freqToleranceMHz,
                               uint8_t learnIntervalHours,
                               uint16_t maxHdopX10,
                               uint8_t minLearnerSpeedMph) {
    if (promotionHits < kMinPromotionHits) {
        promotionHits = kMinPromotionHits;
    } else if (promotionHits > kMaxPromotionHits) {
        promotionHits = kMaxPromotionHits;
    }
    if (radiusE5 < kMinRadiusE5) {
        radiusE5 = kMinRadiusE5;
    } else if (radiusE5 > kMaxRadiusE5) {
        radiusE5 = kMaxRadiusE5;
    }
    if (freqToleranceMHz < kMinFreqToleranceMHz) {
        freqToleranceMHz = kMinFreqToleranceMHz;
    } else if (freqToleranceMHz > kMaxFreqToleranceMHz) {
        freqToleranceMHz = kMaxFreqToleranceMHz;
    }
    learnIntervalHours = clampLearnerIntervalHours(learnIntervalHours);
    promotionHits_ = promotionHits;
    radiusE5_ = radiusE5;
    freqToleranceMHz_ = freqToleranceMHz;
    learnIntervalHours_ = learnIntervalHours;
    learnHitIntervalMs_ = intervalHoursToMs(learnIntervalHours);
    maxHdopX10_ = maxHdopX10;
    minLearnerSpeedMph_ = minLearnerSpeedMph;
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
            if (!lockoutBandSupported(obs.bandRaw)) {
                ++stats_.skippedBand;
                continue;
            }

            // Gate: minimum satellite count for learning quality
            if (obs.satellites < LOCKOUT_GPS_MIN_SATELLITES) {
                ++stats_.skippedLowSats;
                continue;
            }
            // Gate: HDOP quality threshold for learning
            if (maxHdopX10_ > 0
                && obs.hdopX10 != SignalObservation::HDOP_X10_INVALID
                && obs.hdopX10 > maxHdopX10_) {
                ++stats_.skippedHighHdop;
                continue;
            }
            // Gate: minimum speed for learning (blocks GPS-drift learning)
            if (minLearnerSpeedMph_ > 0
                && std::isfinite(obs.speedMph)
                && obs.speedMph < static_cast<float>(minLearnerSpeedMph_)) {
                ++stats_.skippedLowSpeed;
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
                if (epochMs > 0 && c.lastSeenMs != epochMs) {
                    c.lastSeenMs = epochMs;
                    dirty_ = true;
                }
                bool countedHit = true;
                if (learnHitIntervalMs_ > 0 && epochMs > 0 && c.lastCountedHitMs > 0 &&
                    (epochMs - c.lastCountedHitMs) < learnHitIntervalMs_) {
                    countedHit = false;
                }
                if (countedHit) {
                    c.hitCount = (c.hitCount < 255) ? static_cast<uint8_t>(c.hitCount + 1) : 255;
                    if (epochMs > 0) {
                        c.lastCountedHitMs = epochMs;
                    }
                    dirty_ = true;
                }
                // Accumulate GPS heading for direction detection.
                // Runs on every observation (not just counted hits) so that
                // approach heading is captured while still at highway speed,
                // before the vehicle decelerates near the emitter.
                if (obs.courseValid && std::isfinite(obs.courseDeg)) {
                    const float rad = obs.courseDeg * 0.017453292f;
                    c.headingSinSum += static_cast<int16_t>(sinf(rad) * 100.0f);
                    c.headingCosSum += static_cast<int16_t>(cosf(rad) * 100.0f);
                    if (c.headingSampleCount < 255) ++c.headingSampleCount;
                    dirty_ = true;
                }

                // Promote when threshold reached
                if (c.hitCount >= promotionHits_) {
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
                    c.lastCountedHitMs = (epochMs > 0) ? epochMs : 0;
                    c.active     = true;
                    // Record initial GPS heading
                    if (obs.courseValid && std::isfinite(obs.courseDeg)) {
                        const float rad = obs.courseDeg * 0.017453292f;
                        c.headingSinSum = static_cast<int16_t>(sinf(rad) * 100.0f);
                        c.headingCosSum = static_cast<int16_t>(cosf(rad) * 100.0f);
                        c.headingSampleCount = 1;
                    }
                    ++stats_.candidatesCreated;
                    dirty_ = true;
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
        if (!freqClose(freqMHz, c.freqMHz, freqToleranceMHz_)) continue;
        if (!withinRadius(latE5, lonE5, c.latE5, c.lonE5, radiusE5_)) continue;
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
    const uint8_t bandMask = lockoutSanitizeBandMask(c.band);
    if (bandMask == 0) {
        candidates_[idx] = LearnerCandidate{};
        dirty_ = true;
        return;
    }

    LockoutEntry entry;
    entry.latE5      = c.latE5;
    entry.lonE5      = c.lonE5;
    entry.radiusE5   = radiusE5_;
    entry.bandMask   = bandMask;
    entry.freqMHz    = c.freqMHz;
    entry.freqTolMHz = freqToleranceMHz_;
    entry.confidence = c.hitCount;
    entry.flags      = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.firstSeenMs = c.firstSeenMs;
    entry.lastSeenMs  = (epochMs > 0) ? epochMs : c.lastSeenMs;
    entry.lastPassMs  = 0;

    // Direction analysis: if ≥2 heading samples with high consistency (R > 0.70),
    // set DIRECTION_FORWARD so the lockout only mutes when driving the same way.
    if (c.headingSampleCount >= 2) {
        const float sinMean = static_cast<float>(c.headingSinSum) / (100.0f * c.headingSampleCount);
        const float cosMean = static_cast<float>(c.headingCosSum) / (100.0f * c.headingSampleCount);
        const float R = sqrtf(sinMean * sinMean + cosMean * cosMean);
        if (R > 0.70f) {
            float meanDeg = atan2f(static_cast<float>(c.headingSinSum),
                                   static_cast<float>(c.headingCosSum)) * (180.0f / 3.14159265f);
            if (meanDeg < 0.0f) meanDeg += 360.0f;
            entry.headingDeg = static_cast<uint16_t>(meanDeg) % 360;
            entry.directionMode = LockoutEntry::DIRECTION_FORWARD;
            entry.headingTolDeg = 45;
        }
    }

    const int slot = index_->addOrUpdate(entry);
    if (slot >= 0) {
        ++stats_.promotions;
        lockoutStore.markDirty();
        Serial.printf("[Learner] PROMOTED slot=%d band=%u freq=%u lat=%ld lon=%ld hits=%u dir=%s hdg=%d\n",
                      slot, c.band, c.freqMHz,
                      static_cast<long>(c.latE5), static_cast<long>(c.lonE5),
                      c.hitCount,
                      entry.directionMode == LockoutEntry::DIRECTION_FORWARD ? "fwd" : "all",
                      static_cast<int>(entry.headingDeg));
        candidates_[idx] = LearnerCandidate{};
        dirty_ = true;
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
            dirty_ = true;
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

void LockoutLearner::toJson(JsonDocument& doc) const {
    doc["_type"] = kPersistTypeTag;
    doc["_version"] = kPersistVersion;
    JsonArray candidates = doc["candidates"].to<JsonArray>();
    for (const auto& candidate : candidates_) {
        if (!candidate.active) {
            continue;
        }
        JsonObject out = candidates.add<JsonObject>();
        out["lat"] = candidate.latE5;
        out["lon"] = candidate.lonE5;
        out["band"] = candidate.band;
        out["freq"] = candidate.freqMHz;
        out["hits"] = candidate.hitCount;
        out["first"] = candidate.firstSeenMs;
        out["last"] = candidate.lastSeenMs;
        out["lastHit"] = candidate.lastCountedHitMs;
        if (candidate.headingSampleCount > 0) {
            out["hsin"] = candidate.headingSinSum;
            out["hcos"] = candidate.headingCosSum;
            out["hcnt"] = candidate.headingSampleCount;
        }
    }
}

bool LockoutLearner::fromJson(JsonDocument& doc, int64_t epochMs) {
    const char* type = doc["_type"];
    if (!type || std::strcmp(type, kPersistTypeTag) != 0) {
        return false;
    }
    const uint8_t version = doc["_version"] | static_cast<uint8_t>(0);
    if (version != kPersistVersion) {
        return false;
    }

    JsonArray candidates = doc["candidates"].as<JsonArray>();
    if (candidates.isNull()) {
        return false;
    }

    for (auto& candidate : candidates_) {
        candidate = LearnerCandidate{};
    }

    size_t loaded = 0;
    for (JsonObject in : candidates) {
        if (loaded >= kCandidateCapacity) {
            break;
        }
        if (in["lat"].isNull() || in["lon"].isNull() || in["band"].isNull() ||
            in["freq"].isNull() || in["hits"].isNull()) {
            continue;
        }

        LearnerCandidate candidate;
        candidate.latE5 = in["lat"].as<int32_t>();
        candidate.lonE5 = in["lon"].as<int32_t>();
        candidate.band = in["band"].as<uint8_t>();
        candidate.freqMHz = in["freq"].as<uint16_t>();
        candidate.hitCount = in["hits"].as<uint8_t>();
        candidate.firstSeenMs = in["first"] | static_cast<int64_t>(0);
        candidate.lastSeenMs = in["last"] | static_cast<int64_t>(0);
        candidate.lastCountedHitMs = in["lastHit"] | static_cast<int64_t>(0);
        candidate.headingSinSum = in["hsin"] | static_cast<int16_t>(0);
        candidate.headingCosSum = in["hcos"] | static_cast<int16_t>(0);
        candidate.headingSampleCount = in["hcnt"] | static_cast<uint8_t>(0);

        if (candidate.hitCount == 0 || !lockoutBandSupported(candidate.band)) {
            continue;
        }
        if (epochMs > 0 && candidate.lastSeenMs > 0 &&
            (epochMs - candidate.lastSeenMs) > kStaleDurationMs) {
            continue;
        }
        if (index_ && index_->findMatch(candidate.latE5,
                                        candidate.lonE5,
                                        candidate.band,
                                        candidate.freqMHz) >= 0) {
            continue;
        }

        candidate.active = true;
        candidates_[loaded++] = candidate;
    }

    dirty_ = false;
    return true;
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

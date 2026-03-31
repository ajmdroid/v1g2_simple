#include "lockout_enforcer.h"
#include "lockout_band_policy.h"
#include "lockout_signature_utils.h"
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

uint32_t hoursToMs(uint8_t hours) {
    if (hours == 0) return 0;
    return static_cast<uint32_t>(hours) * 3600UL * 1000UL;
}

float normalizeEnforcerHeadingDeg(float heading) {
    if (!std::isfinite(heading)) {
        return NAN;
    }
    float wrapped = std::fmod(heading, 360.0f);
    if (wrapped < 0.0f) {
        wrapped += 360.0f;
    }
    return wrapped;
}

float enforcerHeadingDeltaDeg(float a, float b) {
    const float da = normalizeEnforcerHeadingDeg(a);
    const float db = normalizeEnforcerHeadingDeg(b);
    if (!std::isfinite(da) || !std::isfinite(db)) {
        return NAN;
    }
    float delta = std::fabs(da - db);
    if (delta > 180.0f) {
        delta = 360.0f - delta;
    }
    return delta;
}

bool courseMatchesEntry(bool courseValid, float courseDeg, const LockoutEntry& entry) {
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
        const float delta =
            enforcerHeadingDeltaDeg(courseDeg, static_cast<float>(entry.headingDeg));
        return std::isfinite(delta) && delta <= tolerance;
    }
    if (entry.directionMode == LockoutEntry::DIRECTION_REVERSE) {
        const float reverseHeading =
            normalizeEnforcerHeadingDeg(static_cast<float>(entry.headingDeg) + 180.0f);
        const float delta = enforcerHeadingDeltaDeg(courseDeg, reverseHeading);
        return std::isfinite(delta) && delta <= tolerance;
    }
    return false;
}

uint16_t windowSpanMHz(const LockoutEntry& entry) {
    const uint16_t low = (entry.freqWindowMinMHz > 0) ? entry.freqWindowMinMHz : entry.freqMHz;
    const uint16_t high = (entry.freqWindowMaxMHz > 0) ? entry.freqWindowMaxMHz : entry.freqMHz;
    return (high >= low) ? static_cast<uint16_t>(high - low) : 0;
}

void sortCandidates(const LockoutIndex* index, int16_t* candidates, size_t count) {
    if (!index) {
        return;
    }
    for (size_t i = 1; i < count; ++i) {
        const int16_t key = candidates[i];
        size_t j = i;
        while (j > 0) {
            const LockoutEntry* lhs = index->at(static_cast<size_t>(key));
            const LockoutEntry* rhs = index->at(static_cast<size_t>(candidates[j - 1]));
            if (!lhs || !rhs) {
                break;
            }
            const bool lhsPreferred =
                windowSpanMHz(*lhs) < windowSpanMHz(*rhs) ||
                (windowSpanMHz(*lhs) == windowSpanMHz(*rhs) &&
                 (lhs->radiusE5 < rhs->radiusE5 ||
                  (lhs->radiusE5 == rhs->radiusE5 &&
                   (lhs->confidence > rhs->confidence ||
                    (lhs->confidence == rhs->confidence && key < candidates[j - 1])))));
            if (!lhsPreferred) {
                break;
            }
            candidates[j] = candidates[j - 1];
            --j;
        }
        candidates[j] = key;
    }
}

bool assignAlert(size_t alertIndex,
                 LockoutEnforcer::LiveAlertMatch* alerts,
                 int16_t* slotToAlert,
                 bool* visitedSlots) {
    LockoutEnforcer::LiveAlertMatch& alert = alerts[alertIndex];
    for (size_t i = 0; i < alert.candidateCount; ++i) {
        const int16_t slot = alert.candidates[i];
        if (slot < 0 || visitedSlots[slot]) {
            continue;
        }
        visitedSlots[slot] = true;
        if (slotToAlert[slot] < 0 ||
            assignAlert(static_cast<size_t>(slotToAlert[slot]), alerts, slotToAlert, visitedSlots)) {
            slotToAlert[slot] = static_cast<int16_t>(alertIndex);
            alert.assignedSlot = slot;
            return true;
        }
    }
    return false;
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
                                               int32_t tzOffsetMinutes,
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

    // --- Gate 2b: GPS quality (satellites + HDOP) ---
    if (gpsStatus.satellites < LOCKOUT_GPS_MIN_SATELLITES) {
        ++stats_.skippedLowSats;
        return lastResult_;
    }
    {
        const uint16_t maxHdopX10 = settings_->get().gpsLockoutMaxHdopX10;
        if (maxHdopX10 > 0 && std::isfinite(gpsStatus.hdop)) {
            const uint16_t hdopX10 = static_cast<uint16_t>(
                lroundf(std::max(0.0f, gpsStatus.hdop) * 10.0f));
            if (hdopX10 > maxHdopX10) {
                ++stats_.skippedHighHdop;
                return lastResult_;
            }
        }
    }

    const int32_t latE5 = degreesToE5(gpsStatus.latitudeDeg);
    const int32_t lonE5 = degreesToE5(gpsStatus.longitudeDeg);
    const uint8_t localHour = lockout_signature::localHourFromEpochMs(epochMs, tzOffsetMinutes);

    // --- Gate 3: alert present ---
    if (!parser.hasAlerts()) {
        lastResult_.evaluated = true;
        ++stats_.evaluations;

        // No alert → clean-pass opportunity for nearby lockout zones.
        if (mode == LOCKOUT_RUNTIME_ENFORCE) {
            recordCleanPasses(latE5, lonE5, -1, epochMs, localHour);
        }
        return lastResult_;
    }
    const bool courseValid = gpsStatus.courseValid &&
                             gpsStatus.courseAgeMs <= LOCKOUT_GPS_COURSE_MAX_AGE_MS;
    const size_t nearbyCount = index_->findNearby(latE5, lonE5, nearbySlots_, kMaxNearbySlots);
    bool encounterOverflow = (nearbyCount >= kMaxNearbySlots);
    for (size_t i = 0; i < kMaxSupportedAlerts; ++i) {
        liveAlerts_[i] = LiveAlertMatch{};
    }
    size_t liveAlertCount = 0;
    size_t matchedSlotCount = 0;

    const auto& alerts = parser.getAllAlerts();
    const size_t alertCount = static_cast<size_t>(parser.getAlertCount());
    for (size_t i = 0; i < alertCount; ++i) {
        const AlertData& alert = alerts[i];
        if (!alert.isValid || alert.band == BAND_NONE) {
            continue;
        }

        const uint8_t band = static_cast<uint8_t>(alert.band);
        if (!lockoutBandSupported(band)) {
            continue;
        }

        const uint16_t freqMHz = static_cast<uint16_t>(
            (alert.frequency <= UINT16_MAX) ? alert.frequency : 0);
        LiveAlertMatch& live = liveAlerts_[liveAlertCount++];
        live.band = band;
        live.freqMHz = freqMHz;

        ++lastResult_.supportedAlertCount;
        for (size_t j = 0; j < nearbyCount; ++j) {
            const int16_t slot = nearbySlots_[j];
            if (slot < 0) {
                continue;
            }
            const LockoutEntry* entry = index_->at(static_cast<size_t>(slot));
            if (!entry || !entry->isActive() || !entry->isLearned() || entry->isManual()) {
                continue;
            }
            const uint8_t entryBandMask = lockoutSanitizeBandMask(entry->bandMask);
            if (entryBandMask == 0 || (entryBandMask & band) == 0) {
                continue;
            }
            if (!lockout_signature::freqMatches(*entry, freqMHz)) {
                continue;
            }
            if (!courseMatchesEntry(courseValid, gpsStatus.courseDeg, *entry)) {
                continue;
            }
            if (live.candidateCount >= kMaxCandidateSlots) {
                encounterOverflow = true;
                break;
            }
            live.candidates[live.candidateCount++] = slot;
        }
        sortCandidates(index_, live.candidates, live.candidateCount);
    }

    bool hasDuplicateSupportedFrequency = false;
    for (size_t i = 0; i < liveAlertCount && !hasDuplicateSupportedFrequency; ++i) {
        for (size_t j = i + 1; j < liveAlertCount; ++j) {
            if (liveAlerts_[i].band == liveAlerts_[j].band &&
                liveAlerts_[i].freqMHz == liveAlerts_[j].freqMHz) {
                hasDuplicateSupportedFrequency = true;
                break;
            }
        }
    }

    for (size_t i = 0; i < LockoutIndex::kCapacity; ++i) {
        slotToAlert_[i] = -1;
    }

    bool allAssigned = !encounterOverflow && !hasDuplicateSupportedFrequency;
    if (allAssigned) {
        for (size_t i = 0; i < liveAlertCount; ++i) {
            if (liveAlerts_[i].candidateCount == 0) {
                allAssigned = false;
                break;
            }
            memset(visitedSlots_, 0, sizeof(visitedSlots_));
            if (!assignAlert(i, liveAlerts_, slotToAlert_, visitedSlots_)) {
                allAssigned = false;
                break;
            }
        }
    }

    if (allAssigned) {
        lastResult_.matchedAlertCount = static_cast<uint8_t>(liveAlertCount);
        for (size_t i = 0; i < liveAlertCount; ++i) {
            const int16_t slot = liveAlerts_[i].assignedSlot;
            if (i == 0 && slot >= 0) {
                const LockoutEntry* first = index_->at(static_cast<size_t>(slot));
                lastResult_.matchIndex = slot;
                lastResult_.confidence = first ? first->confidence : 0;
            }
            bool seenSlot = false;
            for (size_t j = 0; j < matchedSlotCount; ++j) {
                if (matchedSlots_[j] == slot) {
                    seenSlot = true;
                    break;
                }
            }
            if (!seenSlot && matchedSlotCount < kMaxMatchedSlots) {
                matchedSlots_[matchedSlotCount++] = slot;
            }
        }
    }

    lastResult_.evaluated = true;
    lastResult_.shouldMute = allAssigned && lastResult_.supportedAlertCount > 0;
    ++stats_.evaluations;
    stats_.matches += lastResult_.matchedAlertCount;

    if (lastResult_.matchedAlertCount > 0) {
        // Only ENFORCE mode mutates index state (confidence + lastSeenMs).
        // SHADOW and ADVISORY are read-only so toggling them has no side-effects.
        if (mode == LOCKOUT_RUNTIME_ENFORCE) {
            for (size_t i = 0; i < matchedSlotCount; ++i) {
                uint16_t observedFreqMHz = 0;
                for (size_t j = 0; j < liveAlertCount; ++j) {
                    if (liveAlerts_[j].assignedSlot == matchedSlots_[i]) {
                        observedFreqMHz = liveAlerts_[j].freqMHz;
                        break;
                    }
                }
                index_->recordHit(static_cast<size_t>(matchedSlots_[i]),
                                  epochMs,
                                  observedFreqMHz,
                                  localHour);
            }
            if (store_) store_->markDirty();
        }

        // Rate-limited log only when the full active alert set is covered.
        if (lastResult_.shouldMute && (nowMs - lastLogMs_ >= LOG_INTERVAL_MS)) {
            lastLogMs_ = nowMs;
            Serial.printf("[Lockout] MATCH mode=%s alerts=%u/%u slot=%d conf=%u lat=%ld lon=%ld\n",
                          lockoutRuntimeModeName(mode),
                          static_cast<unsigned>(lastResult_.matchedAlertCount),
                          static_cast<unsigned>(lastResult_.supportedAlertCount),
                          lastResult_.matchIndex,
                          lastResult_.confidence,
                          static_cast<long>(latE5),
                          static_cast<long>(lonE5));
        }
    }

    return lastResult_;
}
// --- Clean-pass recording for demotion (ENFORCE only).  When no alert is present and we're within range of lockout entries, those entries receive a clean-pass update.  Rate-limited to once per CLEAN_PASS_INTERVAL_MS (~30s) so driving through a 150m zone at 60 mph (5.6s transit) records at most one clean pass per transit. ---

void LockoutEnforcer::recordCleanPasses(int32_t latE5, int32_t lonE5,
                                        int16_t matchedSlot,
                                        int64_t epochMs,
                                        uint8_t localHour) {
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
    const V1Settings& settings = settings_->get();
    const uint8_t learnedMissThreshold = settings.gpsLockoutLearnerUnlearnCount;
    const uint32_t missIntervalMs = hoursToMs(settings.gpsLockoutLearnerUnlearnIntervalHours);
    const bool missPolicyEnabled = learnedMissThreshold > 0;

    bool anyMutated = false;
    for (size_t i = 0; i < count; ++i) {
        if (nearby[i] == matchedSlot) continue;  // Skip matched entry.
        const size_t slot = static_cast<size_t>(nearby[i]);
        const LockoutEntry* entryBefore = index_->at(slot);
        if (!entryBefore || !entryBefore->isActive()) {
            continue;
        }
        if (!entryBefore->isLearned() || entryBefore->isManual()) {
            continue;
        }
        const uint8_t missThreshold = learnedMissThreshold;

        LockoutCleanPassResult result;
        if (!missPolicyEnabled || missThreshold == 0) {
            const bool wasActive = entryBefore->isActive();
            const uint8_t conf = index_->recordCleanPass(slot, epochMs);
            const LockoutEntry* entryAfter = index_->at(slot);
            result.confidence = conf;
            result.counted = true;
            result.demoted = wasActive && (!entryAfter || !entryAfter->isActive());
        } else {
            result = index_->recordCleanPassWithPolicy(
                slot, epochMs, localHour, missIntervalMs, missThreshold);
        }

        if (result.counted) {
            ++stats_.cleanPasses;
            anyMutated = true;
        }
        if (result.demoted) {
            ++stats_.demotions;
            anyMutated = true;
            Serial.printf("[Lockout] DEMOTED slot=%d (clean-pass policy)\n", nearby[i]);
        }
    }

    if (anyMutated && store_) {
        store_->markDirty();
    }
}

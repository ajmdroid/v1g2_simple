#include <unity.h>
#include <ArduinoJson.h>

// Mock Arduino first — resolved by -I test/mocks for <Arduino.h> includes.
#include "../mocks/Arduino.h"
#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// Real signal observation log (uses UNIT_TEST atomic path).
#include "../../src/modules/lockout/signal_observation_log.h"
#include "../../src/modules/lockout/signal_observation_log.cpp"

// Lockout data structures + index.
#include "../../src/modules/lockout/lockout_entry.h"
#include "../../src/modules/lockout/lockout_index.h"
#include "../../src/modules/lockout/lockout_band_policy.cpp"
#include "../../src/modules/lockout/lockout_index.cpp"

// Store (needed by learner for markDirty on promote).
#include "../../src/modules/lockout/lockout_store.h"
#include "../../src/modules/lockout/lockout_store.cpp"

// Unit under test.
#include "../../src/modules/lockout/lockout_learner.h"
#include "../../src/modules/lockout/lockout_learner.cpp"

static LockoutIndex testIndex;
static SignalObservationLog testLog;
static LockoutLearner learner;

// --- Helpers ---

static SignalObservation makeObs(int32_t latE5, int32_t lonE5,
                                 uint8_t band, uint16_t freqMHz,
                                 uint32_t tsMs = 100) {
    SignalObservation o;
    o.tsMs = tsMs;
    o.bandRaw = band;
    o.strength = 5;
    o.frequencyMHz = freqMHz;
    o.hasFix = true;
    o.fixAgeMs = 100;
    o.locationValid = true;
    o.latitudeE5 = latE5;
    o.longitudeE5 = lonE5;
    o.satellites = 7;
    o.hdopX10 = 13;
    return o;
}

static constexpr int32_t LAT = 1012345;   // ~10.12345°
static constexpr int32_t LON = -2054321;  // ~-20.54321°
static constexpr uint8_t K_BAND = (1 << 2);  // BAND_K bitmask = 0x04
static constexpr uint8_t X_BAND = (1 << 3);  // BAND_X bitmask = 0x08
static constexpr uint8_t KA_BAND = (1 << 1); // BAND_KA bitmask = 0x02
static constexpr uint8_t LASER_BAND = (1 << 0); // BAND_LASER bitmask = 0x01
static constexpr uint16_t K_FREQ = 24148;
static constexpr int64_t EPOCH_BASE = 1700000000000LL;

void setUp() {
    lockoutSetKaLearningEnabled(false);
    testIndex.clear();
    testLog.reset();
    learner.begin(&testIndex, &testLog);
}

void tearDown() {}

// ================================================================
// Basic: single observation creates a candidate
// ================================================================

void test_single_obs_creates_candidate() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(1, learner.stats().candidatesCreated);
    TEST_ASSERT_EQUAL(1, learner.stats().observed);

    const LearnerCandidate* c = learner.candidateAt(0);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(c->active);
    TEST_ASSERT_EQUAL(LAT, c->latE5);
    TEST_ASSERT_EQUAL(LON, c->lonE5);
    TEST_ASSERT_EQUAL(K_BAND, c->band);
    TEST_ASSERT_EQUAL(K_FREQ, c->freqMHz);
    TEST_ASSERT_EQUAL(1, c->hitCount);
    TEST_ASSERT_EQUAL(EPOCH_BASE, c->firstSeenMs);
}

// ================================================================
// Nearby observation increments hit count
// ================================================================

void test_nearby_obs_increments_candidate() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);

    // Same location, slight offset within radius
    testLog.publish(makeObs(LAT + 10, LON - 5, K_BAND, K_FREQ + 3));
    learner.process(4000, EPOCH_BASE + 1000);

    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(1, learner.stats().candidatesCreated);

    const LearnerCandidate* c = learner.candidateAt(0);
    TEST_ASSERT_EQUAL(2, c->hitCount);
    TEST_ASSERT_EQUAL(EPOCH_BASE + 1000, c->lastSeenMs);
}

// ================================================================
// Three hits promotes to LockoutIndex
// ================================================================

void test_three_hits_promotes() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);

    testLog.publish(makeObs(LAT + 5, LON, K_BAND, K_FREQ));
    learner.process(4000, EPOCH_BASE + 1000);

    testLog.publish(makeObs(LAT - 5, LON + 5, K_BAND, K_FREQ));
    learner.process(6000, EPOCH_BASE + 2000);

    // Candidate should be promoted and cleared
    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(1, learner.stats().promotions);

    // Verify the LockoutIndex got the entry
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());
    const LockoutEntry* e = testIndex.at(0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->isActive());
    TEST_ASSERT_TRUE(e->isLearned());
    TEST_ASSERT_FALSE(e->isManual());
    TEST_ASSERT_EQUAL(LAT, e->latE5);   // First observation's location
    TEST_ASSERT_EQUAL(LON, e->lonE5);
    TEST_ASSERT_EQUAL(K_BAND, e->bandMask);
    TEST_ASSERT_EQUAL(K_FREQ, e->freqMHz);
    TEST_ASSERT_EQUAL(3, e->confidence);
    TEST_ASSERT_EQUAL(EPOCH_BASE, e->firstSeenMs);
}

// ================================================================
// Runtime tuning clamps to safe bounds
// ================================================================

void test_set_tuning_clamps_bounds() {
    learner.setTuning(0, 1, 0, 0);
    TEST_ASSERT_EQUAL(LockoutLearner::kMinPromotionHits, learner.promotionHits());
    TEST_ASSERT_EQUAL(LockoutLearner::kMinRadiusE5, learner.radiusE5());
    TEST_ASSERT_EQUAL(LockoutLearner::kMinFreqToleranceMHz, learner.freqToleranceMHz());
    TEST_ASSERT_EQUAL(0, learner.learnIntervalHours());

    learner.setTuning(255, 65535, 255, 255);
    TEST_ASSERT_EQUAL(LockoutLearner::kMaxPromotionHits, learner.promotionHits());
    TEST_ASSERT_EQUAL(LockoutLearner::kMaxRadiusE5, learner.radiusE5());
    TEST_ASSERT_EQUAL(LockoutLearner::kMaxFreqToleranceMHz, learner.freqToleranceMHz());
    TEST_ASSERT_EQUAL(24, learner.learnIntervalHours());
}

// ================================================================
// Runtime tuning: promotion threshold follows configured hits
// ================================================================

void test_custom_promotion_hits_threshold() {
    learner.setTuning(4, LockoutLearner::kDefaultRadiusE5, LockoutLearner::kDefaultFreqToleranceMHz, 0);

    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);
    testLog.publish(makeObs(LAT + 4, LON - 3, K_BAND, K_FREQ + 2));
    learner.process(4000, EPOCH_BASE + 1000);
    testLog.publish(makeObs(LAT - 4, LON + 2, K_BAND, K_FREQ + 1));
    learner.process(6000, EPOCH_BASE + 2000);

    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(0, learner.stats().promotions);
    TEST_ASSERT_EQUAL(3, learner.candidateAt(0)->hitCount);

    testLog.publish(makeObs(LAT + 1, LON + 1, K_BAND, K_FREQ));
    learner.process(8000, EPOCH_BASE + 3000);

    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(1, learner.stats().promotions);
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());
}

// ================================================================
// Runtime tuning: promoted entries inherit radius + freq tolerance
// ================================================================

void test_promoted_entry_uses_runtime_radius_and_freq_tolerance() {
    static constexpr uint16_t tunedRadiusE5 = 300;
    static constexpr uint16_t tunedFreqTolMHz = 7;
    learner.setTuning(2, tunedRadiusE5, tunedFreqTolMHz, 0);

    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);
    // Within tuned radius/frequency tolerance to force a single candidate.
    testLog.publish(makeObs(LAT + 250, LON, K_BAND, K_FREQ + tunedFreqTolMHz));
    learner.process(4000, EPOCH_BASE + 1000);

    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(1, learner.stats().promotions);
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());

    const LockoutEntry* promoted = testIndex.at(0);
    TEST_ASSERT_NOT_NULL(promoted);
    TEST_ASSERT_EQUAL(tunedRadiusE5, promoted->radiusE5);
    TEST_ASSERT_EQUAL(tunedFreqTolMHz, promoted->freqTolMHz);
    TEST_ASSERT_EQUAL(2, promoted->confidence);
}

// ================================================================
// Different band creates separate candidate
// ================================================================

void test_supported_different_band_separate_candidate() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    testLog.publish(makeObs(LAT, LON, X_BAND, 10525));
    learner.process(2000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(2, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(2, learner.stats().candidatesCreated);
}

// ================================================================
// Different frequency creates separate candidate
// ================================================================

void test_different_freq_separate_candidate() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ + 50)); // Way outside ±10 MHz
    learner.process(2000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(2, learner.activeCandidateCount());
}

// ================================================================
// Far away location creates separate candidate
// ================================================================

void test_far_away_separate_candidate() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    testLog.publish(makeObs(LAT + 50000, LON, K_BAND, K_FREQ)); // ~500m away
    learner.process(2000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(2, learner.activeCandidateCount());
}

// ================================================================
// Observation already in LockoutIndex is skipped
// ================================================================

void test_already_in_index_skipped() {
    // Pre-populate the index with an entry at this location
    LockoutEntry existing;
    existing.latE5     = LAT;
    existing.lonE5     = LON;
    existing.radiusE5  = 1350;
    existing.bandMask  = K_BAND;  // Already a bitmask
    existing.freqMHz   = K_FREQ;
    existing.freqTolMHz = 10;
    existing.confidence = 50;
    existing.flags     = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    existing.firstSeenMs = EPOCH_BASE - 100000;
    existing.lastSeenMs  = EPOCH_BASE - 50000;
    testIndex.add(existing);

    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(1, learner.stats().skippedInIndex);
}

// ================================================================
// Observation without valid location is skipped
// ================================================================

void test_no_location_skipped() {
    SignalObservation obs = makeObs(LAT, LON, K_BAND, K_FREQ);
    obs.locationValid = false;
    testLog.publish(obs);
    learner.process(2000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(1, learner.stats().skippedNoLocation);
    TEST_ASSERT_EQUAL(0, learner.stats().observed);
}

// ================================================================
// Unsupported lockout bands are skipped
// ================================================================

void test_unsupported_band_skipped() {
    testLog.publish(makeObs(LAT, LON, KA_BAND, 34700));
    testLog.publish(makeObs(LAT, LON, LASER_BAND, 0));
    learner.process(2000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(0, learner.stats().observed);
    TEST_ASSERT_EQUAL(2, learner.stats().skippedBand);
}

void test_ka_band_allowed_when_policy_enabled() {
    lockoutSetKaLearningEnabled(true);

    testLog.publish(makeObs(LAT, LON, KA_BAND, 34700));
    learner.process(2000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(1, learner.stats().observed);
    TEST_ASSERT_EQUAL(0, learner.stats().skippedBand);
}

// ================================================================
// Rate limiting: process() skips when called too soon
// ================================================================

void test_rate_limited() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));

    // Call at time 0 — should skip (lastPollMs_=0, 0-0=0, 0<2000 → skip)
    // Actually begin() sets lastPollMs_=0, and 0-0=0 is NOT < 2000... wait:
    // kPollIntervalMs=2000, 0 < 2000 is true, so it skips? No: the check is
    // (nowMs - lastPollMs_ < kPollIntervalMs), so 0 - 0 = 0 < 2000 → skip.
    learner.process(0, EPOCH_BASE);
    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());

    // Call at 1000 — still too soon
    learner.process(1000, EPOCH_BASE);
    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());

    // Call at 2000 — now it runs
    learner.process(2000, EPOCH_BASE);
    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
}

// ================================================================
// Backlog larger than one batch is fully consumed
// ================================================================

void test_backlog_over_batch_fully_processed() {
    // Publish 40 unique observations before a single poll.
    for (int i = 0; i < 40; ++i) {
        testLog.publish(makeObs(LAT + (i * 5000), LON - (i * 5000), K_BAND, K_FREQ));
    }

    learner.process(2000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(40, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(40, learner.stats().observed);
    TEST_ASSERT_EQUAL(40, learner.stats().candidatesCreated);
}

// ================================================================
// Stale candidate gets pruned after 7 days
// ================================================================

void test_prune_stale_candidate() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);
    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());

    // Jump 8 days and trigger pruning via a new observation elsewhere
    // (so process() runs, and pruneIntervalMs is satisfied)
    const int64_t eightDays = 8LL * 24 * 3600 * 1000;
    testLog.publish(makeObs(LAT + 100000, LON, K_BAND, K_FREQ)); // Far away — new candidate
    learner.process(2000 + 62000, EPOCH_BASE + eightDays); // >60s for prune interval

    // Original candidate should be pruned, but new one created
    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount()); // Only the new one
    TEST_ASSERT_EQUAL(1, learner.stats().pruned);
}

// ================================================================
// Recent candidate is NOT pruned
// ================================================================

void test_recent_candidate_not_pruned() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);

    // 6 days later — within the 7-day window
    const int64_t sixDays = 6LL * 24 * 3600 * 1000;
    testLog.publish(makeObs(LAT + 100000, LON, K_BAND, K_FREQ)); // Trigger a poll
    learner.process(2000 + 62000, EPOCH_BASE + sixDays);

    // Both candidates should still be active
    TEST_ASSERT_EQUAL(2, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(0, learner.stats().pruned);
}

// ================================================================
// Promotion fails when index is full
// ================================================================

void test_promotion_fails_when_index_full() {
    // Fill the entire LockoutIndex
    for (size_t i = 0; i < LockoutIndex::kCapacity; ++i) {
        LockoutEntry e;
        e.latE5 = static_cast<int32_t>(i * 10000);
        e.lonE5 = 0;
        e.radiusE5 = 1350;
        e.bandMask = 0x04;
        e.freqMHz = 24148;
        e.freqTolMHz = 10;
        e.confidence = 50;
        e.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_MANUAL;
        testIndex.add(e);
    }
    TEST_ASSERT_EQUAL(LockoutIndex::kCapacity, testIndex.activeCount());

    // Three observations at a location NOT in the index
    const int32_t farLat = LAT + 5000000; // Very far from all index entries
    for (int i = 0; i < 3; ++i) {
        testLog.publish(makeObs(farLat, LON, K_BAND, K_FREQ));
        learner.process(2000 + i * 2000, EPOCH_BASE + i * 1000);
    }

    // Promotion should have been attempted but failed
    TEST_ASSERT_EQUAL(1, learner.stats().promotionsFailed);
    TEST_ASSERT_EQUAL(0, learner.stats().promotions);
    // Candidate remains active (waiting for an index slot)
    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
}

// ================================================================
// Stats accumulate correctly
// ================================================================

void test_stats_accumulate() {
    // 2 valid observations, 1 without location
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    testLog.publish(makeObs(LAT + 5, LON, K_BAND, K_FREQ + 2));
    SignalObservation noLoc = makeObs(LAT, LON, K_BAND, K_FREQ);
    noLoc.locationValid = false;
    testLog.publish(noLoc);
    learner.process(2000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(2, learner.stats().observed);
    TEST_ASSERT_EQUAL(1, learner.stats().skippedNoLocation);
    TEST_ASSERT_EQUAL(1, learner.stats().candidatesCreated);
}

// ================================================================
// candidateAt bounds check
// ================================================================

void test_candidateAt_bounds() {
    TEST_ASSERT_NULL(learner.candidateAt(LockoutLearner::kCandidateCapacity));
    TEST_ASSERT_NULL(learner.candidateAt(LockoutLearner::kCandidateCapacity + 1));
    TEST_ASSERT_NOT_NULL(learner.candidateAt(0)); // Always valid pointer, but inactive
    TEST_ASSERT_FALSE(learner.candidateAt(0)->active);
}

// ================================================================
// Frequency within tolerance matches same candidate
// ================================================================

void test_freq_within_tolerance_matches() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);

    // +9 MHz — within ±10 tolerance
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ + 9));
    learner.process(4000, EPOCH_BASE + 1000);

    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(2, learner.candidateAt(0)->hitCount);
}

// ================================================================
// Frequency outside tolerance does NOT match
// ================================================================

void test_freq_outside_tolerance_no_match() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);

    // +11 MHz — outside ±10 tolerance
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ + 11));
    learner.process(4000, EPOCH_BASE + 1000);

    TEST_ASSERT_EQUAL(2, learner.activeCandidateCount());
}

// ================================================================
// Epoch 0 still creates candidates (no timestamp)
// ================================================================

void test_epoch_zero_creates_candidate() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, 0);

    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(0, learner.candidateAt(0)->firstSeenMs);
    TEST_ASSERT_EQUAL(0, learner.candidateAt(0)->lastSeenMs);
    TEST_ASSERT_EQUAL(0, learner.candidateAt(0)->lastCountedHitMs);
}

// ================================================================
// Learn interval gates hitCount increments
// ================================================================

void test_learn_interval_gates_hit_increment() {
    learner.setTuning(3, LockoutLearner::kDefaultRadiusE5, LockoutLearner::kDefaultFreqToleranceMHz, 4);
    const int64_t hourMs = 3600LL * 1000LL;

    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);
    TEST_ASSERT_EQUAL(1, learner.candidateAt(0)->hitCount);
    TEST_ASSERT_EQUAL(EPOCH_BASE, learner.candidateAt(0)->lastCountedHitMs);

    testLog.publish(makeObs(LAT + 2, LON - 1, K_BAND, K_FREQ));
    learner.process(4000, EPOCH_BASE + (2 * hourMs));
    TEST_ASSERT_EQUAL(1, learner.candidateAt(0)->hitCount);  // Not counted (<4h)
    TEST_ASSERT_EQUAL(EPOCH_BASE, learner.candidateAt(0)->lastCountedHitMs);

    testLog.publish(makeObs(LAT + 1, LON + 1, K_BAND, K_FREQ));
    learner.process(6000, EPOCH_BASE + (4 * hourMs));
    TEST_ASSERT_EQUAL(2, learner.candidateAt(0)->hitCount);
    TEST_ASSERT_EQUAL(EPOCH_BASE + (4 * hourMs), learner.candidateAt(0)->lastCountedHitMs);

    testLog.publish(makeObs(LAT + 3, LON + 1, K_BAND, K_FREQ));
    learner.process(8000, EPOCH_BASE + (8 * hourMs));
    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());  // Promoted on 3rd counted hit
    TEST_ASSERT_EQUAL(1, learner.stats().promotions);
}

// ================================================================
// 24h learn interval supports 6-hit promotion before stale expiry
// ================================================================

void test_learn_interval_24h_promotes_before_stale_expiry() {
    learner.setTuning(6, LockoutLearner::kDefaultRadiusE5, LockoutLearner::kDefaultFreqToleranceMHz, 24);
    const int64_t dayMs = 24LL * 3600LL * 1000LL;

    for (int i = 0; i < 6; ++i) {
        testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
        learner.process(static_cast<uint32_t>(2000 + (i * 62000)), EPOCH_BASE + (i * dayMs));
    }

    TEST_ASSERT_EQUAL(1, learner.stats().promotions);
    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());
}

// ================================================================
// No observations — no crash, no candidates
// ================================================================

void test_no_observations_no_crash() {
    learner.process(2000, EPOCH_BASE);
    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(0, learner.stats().observed);
}

// ================================================================
// Null dependencies — no crash
// ================================================================

void test_null_deps_no_crash() {
    LockoutLearner nullLearner;
    nullLearner.begin(nullptr, nullptr);
    nullLearner.process(2000, EPOCH_BASE);
    TEST_ASSERT_EQUAL(0, nullLearner.activeCandidateCount());
}

// ================================================================
// Heading accumulates on all observations, not just counted hits.
// With learn interval = 4h, non-counted obs should still feed heading.
// ================================================================

void test_heading_accumulates_on_non_counted_hits() {
    learner.setTuning(3, LockoutLearner::kDefaultRadiusE5, LockoutLearner::kDefaultFreqToleranceMHz, 4);
    const int64_t hourMs = 3600LL * 1000LL;

    // First observation: creates candidate with course. Counted hit.
    auto o1 = makeObs(LAT, LON, K_BAND, K_FREQ);
    o1.courseValid = true;
    o1.courseDeg = 90.0f;
    o1.speedMph = 55.0f;
    testLog.publish(o1);
    learner.process(2000, EPOCH_BASE);
    TEST_ASSERT_EQUAL(1, learner.candidateAt(0)->hitCount);
    TEST_ASSERT_EQUAL(1, learner.candidateAt(0)->headingSampleCount);

    // Second observation: 2h later (under 4h interval), NOT a counted hit,
    // but should still accumulate heading.
    auto o2 = makeObs(LAT + 1, LON - 1, K_BAND, K_FREQ);
    o2.courseValid = true;
    o2.courseDeg = 92.0f;
    o2.speedMph = 60.0f;
    testLog.publish(o2);
    learner.process(4000, EPOCH_BASE + (2 * hourMs));
    TEST_ASSERT_EQUAL(1, learner.candidateAt(0)->hitCount);  // Not counted
    TEST_ASSERT_EQUAL(2, learner.candidateAt(0)->headingSampleCount);  // But heading accumulated!

    // Third observation: 4h after first. Counted hit. Also gets heading.
    auto o3 = makeObs(LAT + 2, LON + 1, K_BAND, K_FREQ);
    o3.courseValid = true;
    o3.courseDeg = 88.0f;
    o3.speedMph = 50.0f;
    testLog.publish(o3);
    learner.process(6000, EPOCH_BASE + (4 * hourMs));
    TEST_ASSERT_EQUAL(2, learner.candidateAt(0)->hitCount);
    TEST_ASSERT_EQUAL(3, learner.candidateAt(0)->headingSampleCount);
}

// ================================================================
// Promotion produces DIRECTION_FORWARD when heading samples are consistent
// ================================================================

void test_promotion_with_course_sets_direction_forward() {
    // 3-hit promotion with consistent 90° heading on all observations.
    for (int i = 0; i < 3; ++i) {
        auto o = makeObs(LAT + i, LON, K_BAND, K_FREQ);
        o.courseValid = true;
        o.courseDeg = 90.0f + static_cast<float>(i);  // 90, 91, 92 — very consistent
        o.speedMph = 55.0f;
        testLog.publish(o);
        learner.process(static_cast<uint32_t>(2000 + i * 2001), EPOCH_BASE + static_cast<int64_t>(i) * 10000LL);
    }

    TEST_ASSERT_EQUAL(1, learner.stats().promotions);
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());

    // Verify the promoted entry has directional data.
    const auto* entry = testIndex.at(0);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL(LockoutEntry::DIRECTION_FORWARD, entry->directionMode);
    TEST_ASSERT_NOT_EQUAL(LockoutEntry::HEADING_INVALID, entry->headingDeg);
    // Heading should be near 90°.
    TEST_ASSERT_INT_WITHIN(5, 90, static_cast<int>(entry->headingDeg));
}

// ================================================================
// Promotion without course data produces DIRECTION_ALL
// ================================================================

void test_promotion_without_course_stays_direction_all() {
    for (int i = 0; i < 3; ++i) {
        auto o = makeObs(LAT + i, LON, K_BAND, K_FREQ);
        // courseValid stays false (default)
        testLog.publish(o);
        learner.process(static_cast<uint32_t>(2000 + i * 2001), EPOCH_BASE + static_cast<int64_t>(i) * 10000LL);
    }

    TEST_ASSERT_EQUAL(1, learner.stats().promotions);
    const auto* entry = testIndex.at(0);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL(LockoutEntry::DIRECTION_ALL, entry->directionMode);
    TEST_ASSERT_EQUAL(LockoutEntry::HEADING_INVALID, entry->headingDeg);
}

void test_clear_candidates_removes_all() {
    testLog.publish(makeObs(LAT, LON, K_BAND, K_FREQ));
    learner.process(2000, EPOCH_BASE);
    testLog.publish(makeObs(LAT + 5000, LON + 5000, K_BAND, K_FREQ));
    learner.process(4100, EPOCH_BASE + 2100);

    TEST_ASSERT_EQUAL(2, learner.activeCandidateCount());
    learner.clearDirty();  // Reset dirty flag from candidate creation.

    learner.clearCandidates();

    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());
    TEST_ASSERT_TRUE(learner.isDirty());

    // Verify all slots are empty.
    for (size_t i = 0; i < LockoutLearner::kCandidateCapacity; ++i) {
        const LearnerCandidate* c = learner.candidateAt(i);
        TEST_ASSERT_FALSE(c->active);
    }
}

// ================================================================
// Runner
// ================================================================

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();

    RUN_TEST(test_single_obs_creates_candidate);
    RUN_TEST(test_nearby_obs_increments_candidate);
    RUN_TEST(test_three_hits_promotes);
    RUN_TEST(test_set_tuning_clamps_bounds);
    RUN_TEST(test_custom_promotion_hits_threshold);
    RUN_TEST(test_promoted_entry_uses_runtime_radius_and_freq_tolerance);
    RUN_TEST(test_supported_different_band_separate_candidate);
    RUN_TEST(test_different_freq_separate_candidate);
    RUN_TEST(test_far_away_separate_candidate);
    RUN_TEST(test_already_in_index_skipped);
    RUN_TEST(test_no_location_skipped);
    RUN_TEST(test_unsupported_band_skipped);
    RUN_TEST(test_ka_band_allowed_when_policy_enabled);
    RUN_TEST(test_rate_limited);
    RUN_TEST(test_backlog_over_batch_fully_processed);
    RUN_TEST(test_prune_stale_candidate);
    RUN_TEST(test_recent_candidate_not_pruned);
    RUN_TEST(test_promotion_fails_when_index_full);
    RUN_TEST(test_stats_accumulate);
    RUN_TEST(test_candidateAt_bounds);
    RUN_TEST(test_freq_within_tolerance_matches);
    RUN_TEST(test_freq_outside_tolerance_no_match);
    RUN_TEST(test_epoch_zero_creates_candidate);
    RUN_TEST(test_learn_interval_gates_hit_increment);
    RUN_TEST(test_learn_interval_24h_promotes_before_stale_expiry);
    RUN_TEST(test_no_observations_no_crash);
    RUN_TEST(test_null_deps_no_crash);
    RUN_TEST(test_heading_accumulates_on_non_counted_hits);
    RUN_TEST(test_promotion_with_course_sets_direction_forward);
    RUN_TEST(test_promotion_without_course_stays_direction_all);
    RUN_TEST(test_clear_candidates_removes_all);

    return UNITY_END();
}

#include <unity.h>

// Pull implementation for header-only UNIT_TEST build.
#include "../../src/modules/lockout/lockout_entry.h"
#include "../../src/modules/lockout/lockout_index.h"
#include "../../src/modules/lockout/lockout_band_policy.cpp"
#include "../../src/modules/lockout/lockout_index.cpp"

#ifndef ARDUINO
#include "../mocks/Arduino.h"
SerialClass Serial;
#endif

static LockoutIndex idx;

void setUp() {
    lockoutSetKaLearningEnabled(false);
    idx.clear();
}

void tearDown() {}

// --- Helper: make a typical K-band lockout entry ---
static LockoutEntry makeKBandEntry(int32_t latE5, int32_t lonE5,
                                   uint16_t freqMHz = 24148,
                                   uint16_t radiusE5 = 1350) {
    LockoutEntry e;
    e.latE5      = latE5;
    e.lonE5      = lonE5;
    e.radiusE5   = radiusE5;
    e.bandMask   = 0x04;          // BAND_K = 1 << 2
    e.freqMHz    = freqMHz;
    e.freqTolMHz = 10;
    e.confidence = 100;
    e.flags      = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    e.firstSeenMs = 1700000000000LL;
    e.lastSeenMs  = 1700000060000LL;
    return e;
}

// ================================================================
// Capacity & add/remove
// ================================================================

void test_clear_resets_all_slots() {
    idx.add(makeKBandEntry(1012345, -2054321));
    idx.add(makeKBandEntry(3736280, -7923225));
    TEST_ASSERT_EQUAL(2, idx.activeCount());

    idx.clear();
    TEST_ASSERT_EQUAL(0, idx.activeCount());
}

void test_add_returns_slot_index() {
    int slot = idx.add(makeKBandEntry(1012345, -2054321));
    TEST_ASSERT_GREATER_OR_EQUAL(0, slot);
    TEST_ASSERT_EQUAL(1, idx.activeCount());
}

void test_add_rejects_unsupported_band_only() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.bandMask = 0x02;  // Ka only
    int slot = idx.add(e);
    TEST_ASSERT_EQUAL(-1, slot);
    TEST_ASSERT_EQUAL(0, idx.activeCount());
}

void test_add_sanitizes_mixed_band_mask() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.bandMask = 0x06;  // K + Ka
    int slot = idx.add(e);
    TEST_ASSERT_GREATER_OR_EQUAL(0, slot);
    TEST_ASSERT_EQUAL(1, idx.activeCount());
    TEST_ASSERT_EQUAL(0x04, idx.at(slot)->bandMask);  // K only
}

void test_add_fills_first_available_slot() {
    int s0 = idx.add(makeKBandEntry(1000000, -1000000));
    int s1 = idx.add(makeKBandEntry(2000000, -2000000));
    TEST_ASSERT_EQUAL(0, s0);
    TEST_ASSERT_EQUAL(1, s1);

    // Remove slot 0, next add should reuse it.
    idx.remove(0);
    int s2 = idx.add(makeKBandEntry(3000000, -3000000));
    TEST_ASSERT_EQUAL(0, s2);
}

void test_add_returns_neg1_when_full() {
    for (size_t i = 0; i < LockoutIndex::kCapacity; ++i) {
        int slot = idx.add(makeKBandEntry(
            static_cast<int32_t>(1000000 + i * 10000),
            static_cast<int32_t>(-1000000 - i * 10000)));
        TEST_ASSERT_GREATER_OR_EQUAL(0, slot);
    }
    TEST_ASSERT_EQUAL(LockoutIndex::kCapacity, idx.activeCount());

    int overflow = idx.add(makeKBandEntry(9999999, -9999999));
    TEST_ASSERT_EQUAL(-1, overflow);
}

void test_remove_marks_inactive() {
    int slot = idx.add(makeKBandEntry(1012345, -2054321));
    TEST_ASSERT_TRUE(idx.at(slot)->isActive());

    bool ok = idx.remove(slot);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(idx.at(slot)->isActive());
    TEST_ASSERT_EQUAL(0, idx.activeCount());
}

void test_remove_out_of_range_returns_false() {
    TEST_ASSERT_FALSE(idx.remove(LockoutIndex::kCapacity));
    TEST_ASSERT_FALSE(idx.remove(999));
}

// ================================================================
// evaluate: basic match
// ================================================================

void test_evaluate_matches_inside_radius() {
    idx.add(makeKBandEntry(1012345, -2054321, 24148, 1350));

    // Exactly at center.
    LockoutDecision d = idx.evaluate(1012345, -2054321, 0x04, 24148);
    TEST_ASSERT_TRUE(d.shouldMute);
    TEST_ASSERT_EQUAL(0, d.matchIndex);
    TEST_ASSERT_EQUAL(100, d.confidence);
}

void test_evaluate_matches_near_edge_of_radius() {
    idx.add(makeKBandEntry(1012345, -2054321, 24148, 1350));

    // ~1300 E5 units away (within 1350 radius).
    LockoutDecision d = idx.evaluate(1012345 + 900, -2054321 + 900, 0x04, 24148);
    // 900^2 + 900^2 = 1620000, 1350^2 = 1822500 → inside
    TEST_ASSERT_TRUE(d.shouldMute);
}

void test_evaluate_no_match_outside_radius() {
    idx.add(makeKBandEntry(1012345, -2054321, 24148, 1350));

    // ~1400 E5 units away (outside 1350 radius).
    LockoutDecision d = idx.evaluate(1012345 + 1000, -2054321 + 1000, 0x04, 24148);
    // 1000^2 + 1000^2 = 2000000, 1350^2 = 1822500 → outside
    TEST_ASSERT_FALSE(d.shouldMute);
    TEST_ASSERT_EQUAL(-1, d.matchIndex);
}

void test_evaluate_no_match_wrong_band() {
    idx.add(makeKBandEntry(1012345, -2054321));

    // Ka band (0x02) vs K-band entry (0x04).
    LockoutDecision d = idx.evaluate(1012345, -2054321, 0x02, 24148);
    TEST_ASSERT_FALSE(d.shouldMute);
}

void test_evaluate_no_match_wrong_freq() {
    idx.add(makeKBandEntry(1012345, -2054321, 24148, 1350));

    // 24200 MHz is 52 MHz away — well outside ±10 MHz tolerance.
    LockoutDecision d = idx.evaluate(1012345, -2054321, 0x04, 24200);
    TEST_ASSERT_FALSE(d.shouldMute);
}

void test_evaluate_matches_freq_within_tolerance() {
    idx.add(makeKBandEntry(1012345, -2054321, 24148, 1350));

    // 24155 MHz is 7 MHz away — within ±10 MHz tolerance.
    LockoutDecision d = idx.evaluate(1012345, -2054321, 0x04, 24155);
    TEST_ASSERT_TRUE(d.shouldMute);
}

void test_evaluate_band_only_lockout_no_freq_filter() {
    LockoutEntry e;
    e.latE5      = 1012345;
    e.lonE5      = -2054321;
    e.radiusE5   = 1350;
    e.bandMask   = 0x04;  // K
    e.freqMHz    = 0;     // No frequency filter
    e.freqTolMHz = 0;
    e.confidence = 50;
    e.flags      = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_MANUAL;
    idx.add(e);

    // Should match any K-band frequency.
    LockoutDecision d = idx.evaluate(1012345, -2054321, 0x04, 35000);
    TEST_ASSERT_TRUE(d.shouldMute);
}

void test_evaluate_empty_index_no_match() {
    LockoutDecision d = idx.evaluate(1012345, -2054321, 0x04, 24148);
    TEST_ASSERT_FALSE(d.shouldMute);
    TEST_ASSERT_EQUAL(-1, d.matchIndex);
}

// ================================================================
// evaluate: multi-band entry
// ================================================================

void test_evaluate_multi_band_entry() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.bandMask = 0x04 | 0x08;  // K + X
    idx.add(e);

    // K match
    LockoutDecision dk = idx.evaluate(1012345, -2054321, 0x04, 24148);
    TEST_ASSERT_TRUE(dk.shouldMute);

    // X match
    LockoutDecision dx = idx.evaluate(1012345, -2054321, 0x08, 24148);
    TEST_ASSERT_TRUE(dx.shouldMute);

    // Ka no match
    LockoutDecision dka = idx.evaluate(1012345, -2054321, 0x02, 24148);
    TEST_ASSERT_FALSE(dka.shouldMute);
}

void test_evaluate_ka_matches_when_policy_enabled() {
    lockoutSetKaLearningEnabled(true);

    LockoutEntry e = makeKBandEntry(1012345, -2054321, 34700);
    e.bandMask = 0x02;  // Ka
    int slot = idx.add(e);
    TEST_ASSERT_GREATER_OR_EQUAL(0, slot);

    LockoutDecision d = idx.evaluate(1012345, -2054321, 0x02, 34700);
    TEST_ASSERT_TRUE(d.shouldMute);
}

// ================================================================
// findMatch
// ================================================================

void test_findMatch_returns_correct_index() {
    idx.add(makeKBandEntry(1000000, -1000000, 24100));
    idx.add(makeKBandEntry(2000000, -2000000, 24200));
    idx.add(makeKBandEntry(3000000, -3000000, 24300));

    int found = idx.findMatch(2000000, -2000000, 0x04, 24200);
    TEST_ASSERT_EQUAL(1, found);
}

void test_findMatch_returns_neg1_when_no_match() {
    idx.add(makeKBandEntry(1000000, -1000000, 24100));

    int found = idx.findMatch(9000000, -9000000, 0x04, 24100);
    TEST_ASSERT_EQUAL(-1, found);
}

// ================================================================
// Confidence: recordHit / recordCleanPass
// ================================================================

void test_recordHit_increments_confidence() {
    int slot = idx.add(makeKBandEntry(1012345, -2054321));
    TEST_ASSERT_EQUAL(100, idx.at(slot)->confidence);

    uint8_t c = idx.recordHit(slot, 1700000120000LL);
    TEST_ASSERT_EQUAL(101, c);
    TEST_ASSERT_EQUAL(1700000120000LL, idx.at(slot)->lastSeenMs);
}

void test_recordHit_caps_at_255() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.confidence = 254;
    int slot = idx.add(e);

    idx.recordHit(slot, 0);
    TEST_ASSERT_EQUAL(255, idx.at(slot)->confidence);

    idx.recordHit(slot, 0);  // Should not overflow.
    TEST_ASSERT_EQUAL(255, idx.at(slot)->confidence);
}

void test_recordCleanPass_decrements_confidence() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.confidence = 3;
    int slot = idx.add(e);

    uint8_t c = idx.recordCleanPass(slot, 1700000120000LL);
    TEST_ASSERT_EQUAL(2, c);
    TEST_ASSERT_EQUAL(1700000120000LL, idx.at(slot)->lastPassMs);
}

void test_recordCleanPass_auto_removes_learned_at_zero() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.confidence = 1;
    e.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    int slot = idx.add(e);

    uint8_t c = idx.recordCleanPass(slot, 1700000120000LL);
    TEST_ASSERT_EQUAL(0, c);
    TEST_ASSERT_FALSE(idx.at(slot)->isActive());  // Auto-removed.
    TEST_ASSERT_EQUAL(0, idx.activeCount());
}

void test_recordCleanPass_manual_entry_floors_at_zero_but_stays_active() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.confidence = 1;
    e.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_MANUAL;
    int slot = idx.add(e);

    uint8_t c = idx.recordCleanPass(slot, 1700000120000LL);
    TEST_ASSERT_EQUAL(0, c);
    TEST_ASSERT_TRUE(idx.at(slot)->isActive());  // Manual stays.
}

void test_recordCleanPass_removes_plain_active_at_zero() {
    // Entry is active but has neither MANUAL nor LEARNED flag.
    // Should still be removed at confidence 0 (all non-manual are pruned).
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.confidence = 1;
    e.flags = LockoutEntry::FLAG_ACTIVE;  // No MANUAL, no LEARNED
    int slot = idx.add(e);

    uint8_t c = idx.recordCleanPass(slot, 1700000120000LL);
    TEST_ASSERT_EQUAL(0, c);
    TEST_ASSERT_FALSE(idx.at(slot)->isActive());  // Non-manual removed.
    TEST_ASSERT_EQUAL(0, idx.activeCount());
}

void test_recordHit_resets_miss_tracking() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.missCount = 4;
    e.lastCountedMissMs = 1700000000000LL;
    int slot = idx.add(e);

    idx.recordHit(slot, 1700000120000LL);
    TEST_ASSERT_EQUAL(0, idx.at(slot)->missCount);
    TEST_ASSERT_EQUAL(0, idx.at(slot)->lastCountedMissMs);
}

void test_recordCleanPassWithPolicy_threshold_demotes_after_count() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.confidence = 80;
    int slot = idx.add(e);

    LockoutCleanPassResult r1 = idx.recordCleanPassWithPolicy(
        static_cast<size_t>(slot), 1700000100000LL, 0, 3);
    TEST_ASSERT_TRUE(r1.counted);
    TEST_ASSERT_FALSE(r1.demoted);
    TEST_ASSERT_EQUAL(1, idx.at(slot)->missCount);

    LockoutCleanPassResult r2 = idx.recordCleanPassWithPolicy(
        static_cast<size_t>(slot), 1700000135000LL, 0, 3);
    TEST_ASSERT_TRUE(r2.counted);
    TEST_ASSERT_FALSE(r2.demoted);
    TEST_ASSERT_EQUAL(2, idx.at(slot)->missCount);

    LockoutCleanPassResult r3 = idx.recordCleanPassWithPolicy(
        static_cast<size_t>(slot), 1700000170000LL, 0, 3);
    TEST_ASSERT_TRUE(r3.counted);
    TEST_ASSERT_TRUE(r3.demoted);
    TEST_ASSERT_FALSE(idx.at(slot)->isActive());
}

void test_recordCleanPassWithPolicy_interval_gate() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    int slot = idx.add(e);
    const uint32_t oneHourMs = 3600000UL;

    LockoutCleanPassResult r1 = idx.recordCleanPassWithPolicy(
        static_cast<size_t>(slot), 1700000100000LL, oneHourMs, 5);
    TEST_ASSERT_TRUE(r1.counted);
    TEST_ASSERT_EQUAL(1, idx.at(slot)->missCount);
    TEST_ASSERT_EQUAL(1700000100000LL, idx.at(slot)->lastPassMs);
    TEST_ASSERT_EQUAL(1700000100000LL, idx.at(slot)->lastCountedMissMs);

    LockoutCleanPassResult r2 = idx.recordCleanPassWithPolicy(
        static_cast<size_t>(slot), 1700000105000LL, oneHourMs, 5);
    TEST_ASSERT_FALSE(r2.counted);
    TEST_ASSERT_FALSE(r2.demoted);
    TEST_ASSERT_EQUAL(1, idx.at(slot)->missCount);
    TEST_ASSERT_EQUAL(1700000100000LL, idx.at(slot)->lastPassMs);  // unchanged until counted

    LockoutCleanPassResult r3 = idx.recordCleanPassWithPolicy(
        static_cast<size_t>(slot), 1700003700000LL, oneHourMs, 5);
    TEST_ASSERT_TRUE(r3.counted);
    TEST_ASSERT_EQUAL(2, idx.at(slot)->missCount);
    TEST_ASSERT_EQUAL(1700003700000LL, idx.at(slot)->lastPassMs);
}

// ================================================================
// Timestamp fields: real epoch values
// ================================================================

void test_entry_stores_real_epoch_timestamps() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.firstSeenMs = 1739000000000LL;  // ~Feb 2025
    e.lastSeenMs  = 1739000060000LL;
    e.lastPassMs  = 0;

    int slot = idx.add(e);
    TEST_ASSERT_EQUAL(1739000000000LL, idx.at(slot)->firstSeenMs);
    TEST_ASSERT_EQUAL(1739000060000LL, idx.at(slot)->lastSeenMs);
    TEST_ASSERT_EQUAL(0, idx.at(slot)->lastPassMs);

    idx.recordHit(slot, 1739000120000LL);
    TEST_ASSERT_EQUAL(1739000120000LL, idx.at(slot)->lastSeenMs);

    idx.recordCleanPass(slot, 1739000180000LL);
    TEST_ASSERT_EQUAL(1739000180000LL, idx.at(slot)->lastPassMs);
}

// ================================================================
// at() / mutableAt() bounds checking
// ================================================================

void test_at_nullptr_out_of_range() {
    TEST_ASSERT_NULL(idx.at(LockoutIndex::kCapacity));
    TEST_ASSERT_NULL(idx.at(999));
    TEST_ASSERT_NULL(idx.mutableAt(LockoutIndex::kCapacity));
}

void test_mutableAt_allows_modification() {
    int slot = idx.add(makeKBandEntry(1012345, -2054321));
    LockoutEntry* e = idx.mutableAt(slot);
    TEST_ASSERT_NOT_NULL(e);

    e->confidence = 42;
    TEST_ASSERT_EQUAL(42, idx.at(slot)->confidence);
}

// ================================================================
// Entry flag helpers
// ================================================================

void test_flag_setters_and_getters() {
    LockoutEntry e;
    TEST_ASSERT_FALSE(e.isActive());
    TEST_ASSERT_FALSE(e.isManual());
    TEST_ASSERT_FALSE(e.isLearned());

    e.setActive(true);
    TEST_ASSERT_TRUE(e.isActive());
    e.setManual(true);
    TEST_ASSERT_TRUE(e.isManual());
    e.setLearned(true);
    TEST_ASSERT_TRUE(e.isLearned());

    e.setActive(false);
    TEST_ASSERT_FALSE(e.isActive());
    TEST_ASSERT_TRUE(e.isManual());   // Others unchanged.
    TEST_ASSERT_TRUE(e.isLearned());
}

void test_entry_clear_resets_all_fields() {
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.clear();

    TEST_ASSERT_EQUAL(0, e.latE5);
    TEST_ASSERT_EQUAL(0, e.lonE5);
    TEST_ASSERT_EQUAL(0, e.radiusE5);
    TEST_ASSERT_EQUAL(0, e.bandMask);
    TEST_ASSERT_EQUAL(0, e.freqMHz);
    TEST_ASSERT_EQUAL(0, e.confidence);
    TEST_ASSERT_EQUAL(0, e.flags);
    TEST_ASSERT_EQUAL(0, e.missCount);
    TEST_ASSERT_EQUAL(0, e.firstSeenMs);
    TEST_ASSERT_EQUAL(0, e.lastSeenMs);
    TEST_ASSERT_EQUAL(0, e.lastPassMs);
    TEST_ASSERT_EQUAL(0, e.lastCountedMissMs);
}

// ================================================================
// addOrUpdate (dedup)
// ================================================================

void test_addOrUpdate_creates_when_no_match() {
    int slot = idx.addOrUpdate(makeKBandEntry(1012345, -2054321));
    TEST_ASSERT_GREATER_OR_EQUAL(0, slot);
    TEST_ASSERT_EQUAL(1, idx.activeCount());
}

void test_addOrUpdate_merges_when_match() {
    LockoutEntry e1 = makeKBandEntry(1012345, -2054321);
    e1.confidence = 50;
    idx.addOrUpdate(e1);

    // Same location+band+freq → should merge, not create a second entry.
    LockoutEntry e2 = makeKBandEntry(1012345, -2054321);
    e2.confidence = 80;
    int slot = idx.addOrUpdate(e2);

    TEST_ASSERT_GREATER_OR_EQUAL(0, slot);
    TEST_ASSERT_EQUAL(1, idx.activeCount());  // No duplicate.
    TEST_ASSERT_EQUAL(80, idx.at(slot)->confidence);  // Max of 50, 80.
}

void test_addOrUpdate_keeps_earliest_firstSeen() {
    LockoutEntry e1 = makeKBandEntry(1012345, -2054321);
    e1.firstSeenMs = 2000000000000LL;
    e1.lastSeenMs  = 2000000000000LL;
    idx.addOrUpdate(e1);

    LockoutEntry e2 = makeKBandEntry(1012345, -2054321);
    e2.firstSeenMs = 1000000000000LL;  // Earlier.
    e2.lastSeenMs  = 3000000000000LL;
    int slot = idx.addOrUpdate(e2);

    TEST_ASSERT_EQUAL(1000000000000LL, idx.at(slot)->firstSeenMs);  // Earliest kept.
}

void test_addOrUpdate_keeps_latest_lastSeen() {
    LockoutEntry e1 = makeKBandEntry(1012345, -2054321);
    e1.lastSeenMs = 3000000000000LL;
    idx.addOrUpdate(e1);

    LockoutEntry e2 = makeKBandEntry(1012345, -2054321);
    e2.lastSeenMs = 1000000000000LL;  // Older — should NOT overwrite.
    int slot = idx.addOrUpdate(e2);

    TEST_ASSERT_EQUAL(3000000000000LL, idx.at(slot)->lastSeenMs);  // Latest kept.
}

void test_addOrUpdate_merges_flags() {
    LockoutEntry e1 = makeKBandEntry(1012345, -2054321);
    e1.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    idx.addOrUpdate(e1);

    LockoutEntry e2 = makeKBandEntry(1012345, -2054321);
    e2.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_MANUAL;
    int slot = idx.addOrUpdate(e2);

    uint8_t expected = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED | LockoutEntry::FLAG_MANUAL;
    TEST_ASSERT_EQUAL(expected, idx.at(slot)->flags);
}

void test_addOrUpdate_no_duplicate() {
    // Add 3 entries at the same location — only 1 should survive.
    LockoutEntry e = makeKBandEntry(1012345, -2054321);
    e.confidence = 10;
    idx.addOrUpdate(e);
    e.confidence = 20;
    idx.addOrUpdate(e);
    e.confidence = 30;
    idx.addOrUpdate(e);

    TEST_ASSERT_EQUAL(1, idx.activeCount());
    // Confidence should be max seen: 30.
    const LockoutEntry* p = idx.at(0);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(30, p->confidence);
}

void test_addOrUpdate_resets_miss_tracking_on_merge() {
    LockoutEntry e1 = makeKBandEntry(1012345, -2054321);
    e1.missCount = 5;
    e1.lastCountedMissMs = 1700000000000LL;
    idx.addOrUpdate(e1);

    LockoutEntry e2 = makeKBandEntry(1012345, -2054321);
    e2.confidence = 150;
    idx.addOrUpdate(e2);

    const LockoutEntry* p = idx.at(0);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(0, p->missCount);
    TEST_ASSERT_EQUAL(0, p->lastCountedMissMs);
}

// ================================================================
// findNearby (position-only scan)
// ================================================================

void test_findNearby_returns_entries_within_radius() {
    LockoutEntry eK = makeKBandEntry(1012345, -2054321);
    eK.bandMask = 0x04;
    eK.freqMHz = 24148;
    // Add an X entry at the SAME location, different band.
    LockoutEntry eX = makeKBandEntry(1012345, -2054321);
    eX.bandMask = 0x08;
    eX.freqMHz = 10525;

    idx.add(eK);
    idx.add(eX);

    int16_t nearby[8];
    size_t count = idx.findNearby(1012345, -2054321, nearby, 8);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL(0, nearby[0]);
    TEST_ASSERT_EQUAL(1, nearby[1]);
}

void test_findNearby_excludes_distant_entries() {
    idx.add(makeKBandEntry(1012345, -2054321));
    // Add another entry far away.
    idx.add(makeKBandEntry(4000000, -8000000));

    int16_t nearby[8];
    size_t count = idx.findNearby(1012345, -2054321, nearby, 8);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(0, nearby[0]);
}

void test_findNearby_respects_outCap() {
    // Add 5 entries at the same location.
    for (int i = 0; i < 5; ++i) {
        idx.add(makeKBandEntry(1012345, -2054321));
    }

    int16_t nearby[3];
    size_t count = idx.findNearby(1012345, -2054321, nearby, 3);
    TEST_ASSERT_EQUAL(3, count);
}

void test_findNearby_empty_returns_zero() {
    int16_t nearby[8];
    size_t count = idx.findNearby(1012345, -2054321, nearby, 8);
    TEST_ASSERT_EQUAL(0, count);
}

// ================================================================
// Runner
// ================================================================

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();

    // Capacity & add/remove
    RUN_TEST(test_clear_resets_all_slots);
    RUN_TEST(test_add_returns_slot_index);
    RUN_TEST(test_add_rejects_unsupported_band_only);
    RUN_TEST(test_add_sanitizes_mixed_band_mask);
    RUN_TEST(test_add_fills_first_available_slot);
    RUN_TEST(test_add_returns_neg1_when_full);
    RUN_TEST(test_remove_marks_inactive);
    RUN_TEST(test_remove_out_of_range_returns_false);

    // evaluate
    RUN_TEST(test_evaluate_matches_inside_radius);
    RUN_TEST(test_evaluate_matches_near_edge_of_radius);
    RUN_TEST(test_evaluate_no_match_outside_radius);
    RUN_TEST(test_evaluate_no_match_wrong_band);
    RUN_TEST(test_evaluate_no_match_wrong_freq);
    RUN_TEST(test_evaluate_matches_freq_within_tolerance);
    RUN_TEST(test_evaluate_band_only_lockout_no_freq_filter);
    RUN_TEST(test_evaluate_empty_index_no_match);
    RUN_TEST(test_evaluate_multi_band_entry);
    RUN_TEST(test_evaluate_ka_matches_when_policy_enabled);

    // findMatch
    RUN_TEST(test_findMatch_returns_correct_index);
    RUN_TEST(test_findMatch_returns_neg1_when_no_match);

    // Confidence
    RUN_TEST(test_recordHit_increments_confidence);
    RUN_TEST(test_recordHit_caps_at_255);
    RUN_TEST(test_recordCleanPass_decrements_confidence);
    RUN_TEST(test_recordCleanPass_auto_removes_learned_at_zero);
    RUN_TEST(test_recordCleanPass_manual_entry_floors_at_zero_but_stays_active);
    RUN_TEST(test_recordCleanPass_removes_plain_active_at_zero);
    RUN_TEST(test_recordHit_resets_miss_tracking);
    RUN_TEST(test_recordCleanPassWithPolicy_threshold_demotes_after_count);
    RUN_TEST(test_recordCleanPassWithPolicy_interval_gate);

    // Timestamps
    RUN_TEST(test_entry_stores_real_epoch_timestamps);

    // Bounds / access
    RUN_TEST(test_at_nullptr_out_of_range);
    RUN_TEST(test_mutableAt_allows_modification);

    // Flags
    RUN_TEST(test_flag_setters_and_getters);
    RUN_TEST(test_entry_clear_resets_all_fields);

    // addOrUpdate (dedup)
    RUN_TEST(test_addOrUpdate_creates_when_no_match);
    RUN_TEST(test_addOrUpdate_merges_when_match);
    RUN_TEST(test_addOrUpdate_keeps_earliest_firstSeen);
    RUN_TEST(test_addOrUpdate_keeps_latest_lastSeen);
    RUN_TEST(test_addOrUpdate_merges_flags);
    RUN_TEST(test_addOrUpdate_no_duplicate);
    RUN_TEST(test_addOrUpdate_resets_miss_tracking_on_merge);

    // findNearby (position-only)
    RUN_TEST(test_findNearby_returns_entries_within_radius);
    RUN_TEST(test_findNearby_excludes_distant_entries);
    RUN_TEST(test_findNearby_respects_outCap);
    RUN_TEST(test_findNearby_empty_returns_zero);

    return UNITY_END();
}

#include <unity.h>
#include <ArduinoJson.h>

// Mock Arduino first — resolved by -I test/mocks for <Arduino.h> includes.
#include "../mocks/Arduino.h"
#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// Lockout data structures + index.
#include "../../src/modules/lockout/lockout_entry.h"
#include "../../src/modules/lockout/lockout_index.h"
#include "../../src/modules/lockout/lockout_band_policy.cpp"
#include "../../src/modules/lockout/lockout_index.cpp"

// Unit under test.
#include "../../src/modules/lockout/lockout_store.h"
#include "../../src/modules/lockout/lockout_store.cpp"

static LockoutIndex testIndex;
static LockoutStore store;

void setUp() {
    lockoutSetKaLearningEnabled(false);
    testIndex.clear();
    store.begin(&testIndex);
}

void tearDown() {}

// --- Helper: make a typical K-band lockout entry ---
static LockoutEntry makeEntry(int32_t latE5      = 3736277,
                              int32_t lonE5      = -7923221,
                              uint16_t freqMHz   = 24148,
                              uint8_t  bandMask  = 0x04,      // BAND_K
                              uint8_t  confidence = 100,
                              uint8_t  flags     = LockoutEntry::FLAG_ACTIVE
                                                 | LockoutEntry::FLAG_LEARNED) {
    LockoutEntry e;
    e.latE5       = latE5;
    e.lonE5       = lonE5;
    e.radiusE5    = 1350;
    e.bandMask    = bandMask;
    e.freqMHz     = freqMHz;
    e.freqTolMHz  = 10;
    e.confidence  = confidence;
    e.flags       = flags;
    e.missCount   = 0;
    e.firstSeenMs = 1700000000000LL;
    e.lastSeenMs  = 1700000060000LL;
    e.lastPassMs  = 1700000090000LL;
    e.lastCountedMissMs = 0;
    return e;
}

// ================================================================
// toJson tests
// ================================================================

void test_toJson_empty_index() {
    JsonDocument doc;
    store.toJson(doc);

    TEST_ASSERT_EQUAL_STRING("v1simple_lockout_zones", doc["_type"].as<const char*>());
    TEST_ASSERT_EQUAL(1, doc["_version"].as<int>());

    JsonArray zones = doc["zones"];
    TEST_ASSERT_FALSE(zones.isNull());
    TEST_ASSERT_EQUAL(0, zones.size());
    TEST_ASSERT_EQUAL(0, store.stats().entriesSaved);
}

void test_toJson_single_entry() {
    testIndex.add(makeEntry());

    JsonDocument doc;
    store.toJson(doc);

    JsonArray zones = doc["zones"];
    TEST_ASSERT_EQUAL(1, zones.size());
    TEST_ASSERT_EQUAL(1, store.stats().entriesSaved);

    JsonObject z = zones[0];
    TEST_ASSERT_EQUAL(3736277, z["lat"].as<int32_t>());
    TEST_ASSERT_EQUAL(-7923221, z["lon"].as<int32_t>());
    TEST_ASSERT_EQUAL(1350, z["rad"].as<uint16_t>());
    TEST_ASSERT_EQUAL(0x04, z["band"].as<uint8_t>());
    TEST_ASSERT_EQUAL(24148, z["freq"].as<uint16_t>());
    TEST_ASSERT_EQUAL(10, z["ftol"].as<uint16_t>());
    TEST_ASSERT_EQUAL(100, z["conf"].as<uint8_t>());
    TEST_ASSERT_EQUAL(0, z["miss"].as<uint8_t>());
}

void test_toJson_skips_inactive_slots() {
    int s0 = testIndex.add(makeEntry(1000000, -1000000));
    int s1 = testIndex.add(makeEntry(2000000, -2000000));
    testIndex.add(makeEntry(3000000, -3000000));

    // Remove the middle one.
    testIndex.remove(static_cast<size_t>(s1));
    TEST_ASSERT_EQUAL(2, testIndex.activeCount());

    JsonDocument doc;
    store.toJson(doc);

    JsonArray zones = doc["zones"];
    TEST_ASSERT_EQUAL(2, zones.size());
    TEST_ASSERT_EQUAL(2, store.stats().entriesSaved);
}

void test_toJson_skips_unsupported_band_entries() {
    int slot = testIndex.add(makeEntry());
    TEST_ASSERT_GREATER_OR_EQUAL(0, slot);
    LockoutEntry* e = testIndex.mutableAt(static_cast<size_t>(slot));
    TEST_ASSERT_NOT_NULL(e);

    // Simulate a legacy/corrupt in-memory entry that carries only Ka.
    e->bandMask = 0x02;

    JsonDocument doc;
    store.toJson(doc);

    JsonArray zones = doc["zones"];
    TEST_ASSERT_EQUAL(0, zones.size());
    TEST_ASSERT_EQUAL(0, store.stats().entriesSaved);
}

void test_toJson_keeps_ka_entries_when_policy_enabled() {
    lockoutSetKaLearningEnabled(true);
    int slot = testIndex.add(makeEntry(3736277, -7923221, 34700, 0x02));
    TEST_ASSERT_GREATER_OR_EQUAL(0, slot);

    JsonDocument doc;
    store.toJson(doc);

    JsonArray zones = doc["zones"];
    TEST_ASSERT_EQUAL(1, zones.size());
    TEST_ASSERT_EQUAL(0x02, zones[0]["band"].as<uint8_t>());
}

void test_toJson_all_fields_present() {
    LockoutEntry e = makeEntry();
    e.radiusE5    = 2700;
    e.freqTolMHz  = 15;
    e.flags       = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_MANUAL;
    e.missCount   = 4;
    e.firstSeenMs = 1234567890000LL;
    e.lastSeenMs  = 1234567891000LL;
    e.lastPassMs  = 1234567892000LL;
    e.lastCountedMissMs = 1234567893000LL;
    testIndex.add(e);

    JsonDocument doc;
    store.toJson(doc);

    JsonObject z = doc["zones"][0];
    TEST_ASSERT_EQUAL(2700, z["rad"].as<uint16_t>());
    TEST_ASSERT_EQUAL(15, z["ftol"].as<uint16_t>());
    TEST_ASSERT_EQUAL(LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_MANUAL,
                      z["flags"].as<uint8_t>());
    TEST_ASSERT_EQUAL(4, z["miss"].as<uint8_t>());
    TEST_ASSERT_EQUAL(1234567890000LL, z["first"].as<int64_t>());
    TEST_ASSERT_EQUAL(1234567891000LL, z["last"].as<int64_t>());
    TEST_ASSERT_EQUAL(1234567892000LL, z["pass"].as<int64_t>());
    TEST_ASSERT_EQUAL(1234567893000LL, z["mms"].as<int64_t>());
}

// ================================================================
// fromJson tests
// ================================================================

void test_fromJson_valid_single_entry() {
    // Build a valid JSON doc.
    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 1;
    JsonArray zones = doc["zones"].to<JsonArray>();
    JsonObject z = zones.add<JsonObject>();
    z["lat"]   = 3736277;
    z["lon"]   = -7923221;
    z["rad"]   = 1350;
    z["band"]  = 4;
    z["freq"]  = 24148;
    z["ftol"]  = 10;
    z["conf"]  = 100;
    z["flags"] = 5;  // ACTIVE | LEARNED
    z["miss"]  = 2;
    z["first"] = 1700000000000LL;
    z["last"]  = 1700000060000LL;
    z["pass"]  = 1700000090000LL;
    z["mms"]   = 1700000110000LL;

    bool ok = store.fromJson(doc);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());
    TEST_ASSERT_EQUAL(1, store.stats().loads);
    TEST_ASSERT_EQUAL(1, store.stats().entriesLoaded);

    const LockoutEntry* e = testIndex.at(0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(3736277, e->latE5);
    TEST_ASSERT_EQUAL(-7923221, e->lonE5);
    TEST_ASSERT_EQUAL(1350, e->radiusE5);
    TEST_ASSERT_EQUAL(4, e->bandMask);
    TEST_ASSERT_EQUAL(24148, e->freqMHz);
    TEST_ASSERT_EQUAL(10, e->freqTolMHz);
    TEST_ASSERT_EQUAL(100, e->confidence);
    TEST_ASSERT_EQUAL(5, e->flags);
    TEST_ASSERT_EQUAL(2, e->missCount);
    TEST_ASSERT_TRUE(e->isActive());
    TEST_ASSERT_TRUE(e->isLearned());
    TEST_ASSERT_EQUAL(1700000000000LL, e->firstSeenMs);
    TEST_ASSERT_EQUAL(1700000060000LL, e->lastSeenMs);
    TEST_ASSERT_EQUAL(1700000090000LL, e->lastPassMs);
    TEST_ASSERT_EQUAL(1700000110000LL, e->lastCountedMissMs);
}

void test_fromJson_roundtrip() {
    // Populate index with 3 entries.
    testIndex.add(makeEntry(1000000, -1000000, 24100));
    testIndex.add(makeEntry(2000000, -2000000, 24160, 0x06));  // K + Ka -> sanitize to K
    testIndex.add(makeEntry(3000000, -3000000, 10525, 0x08));  // X
    TEST_ASSERT_EQUAL(3, testIndex.activeCount());

    // Serialize.
    JsonDocument saveDoc;
    store.toJson(saveDoc);

    // Serialize to string, then deserialize to a new doc (simulates file I/O).
    char buf[4096];
    size_t len = serializeJson(saveDoc, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);

    JsonDocument loadDoc;
    DeserializationError err = deserializeJson(loadDoc, buf, len);
    TEST_ASSERT_TRUE(err == DeserializationError::Ok);

    // Clear and restore.
    testIndex.clear();
    TEST_ASSERT_EQUAL(0, testIndex.activeCount());

    bool ok = store.fromJson(loadDoc);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(3, testIndex.activeCount());

    // Verify entries restored (order may differ; check by lat).
    bool found[3] = {false, false, false};
    for (size_t i = 0; i < testIndex.capacity(); ++i) {
        const LockoutEntry* e = testIndex.at(i);
        if (!e || !e->isActive()) continue;
        if (e->latE5 == 1000000) { found[0] = true; TEST_ASSERT_EQUAL(24100, e->freqMHz); }
        if (e->latE5 == 2000000) { found[1] = true; TEST_ASSERT_EQUAL(0x04, e->bandMask); }
        if (e->latE5 == 3000000) { found[2] = true; TEST_ASSERT_EQUAL(0x08, e->bandMask); }
    }
    TEST_ASSERT_TRUE(found[0]);
    TEST_ASSERT_TRUE(found[1]);
    TEST_ASSERT_TRUE(found[2]);
}

void test_fromJson_all_fields_survive_roundtrip() {
    LockoutEntry orig = makeEntry();
    orig.radiusE5    = 2700;
    orig.bandMask    = 0x08;  // X
    orig.freqMHz     = 34720;
    orig.freqTolMHz  = 15;
    orig.confidence  = 42;
    orig.flags       = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_MANUAL;
    orig.missCount   = 6;
    orig.firstSeenMs = 1111111111111LL;
    orig.lastSeenMs  = 2222222222222LL;
    orig.lastPassMs  = 3333333333333LL;
    orig.lastCountedMissMs = 4444444444444LL;
    testIndex.add(orig);

    // Round-trip through JSON string.
    JsonDocument saveDoc;
    store.toJson(saveDoc);
    char buf[2048];
    serializeJson(saveDoc, buf, sizeof(buf));

    JsonDocument loadDoc;
    deserializeJson(loadDoc, buf);
    testIndex.clear();
    store.fromJson(loadDoc);

    TEST_ASSERT_EQUAL(1, testIndex.activeCount());
    const LockoutEntry* e = testIndex.at(0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(orig.latE5,       e->latE5);
    TEST_ASSERT_EQUAL(orig.lonE5,       e->lonE5);
    TEST_ASSERT_EQUAL(orig.radiusE5,    e->radiusE5);
    TEST_ASSERT_EQUAL(orig.bandMask,    e->bandMask);
    TEST_ASSERT_EQUAL(orig.freqMHz,     e->freqMHz);
    TEST_ASSERT_EQUAL(orig.freqTolMHz,  e->freqTolMHz);
    TEST_ASSERT_EQUAL(orig.confidence,  e->confidence);
    TEST_ASSERT_EQUAL(orig.flags,       e->flags);
    TEST_ASSERT_EQUAL(orig.missCount,   e->missCount);
    TEST_ASSERT_EQUAL(orig.firstSeenMs, e->firstSeenMs);
    TEST_ASSERT_EQUAL(orig.lastSeenMs,  e->lastSeenMs);
    TEST_ASSERT_EQUAL(orig.lastPassMs,  e->lastPassMs);
    TEST_ASSERT_EQUAL(orig.lastCountedMissMs, e->lastCountedMissMs);
}

void test_fromJson_wrong_type_rejected() {
    JsonDocument doc;
    doc["_type"]    = "wrong_type";
    doc["_version"] = 1;
    doc["zones"]    = JsonArray();

    bool ok = store.fromJson(doc);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(1, store.stats().loadErrors);
    // Index must not have been cleared (fromJson should fail before clearing).
}

void test_fromJson_wrong_version_rejected() {
    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 99;
    doc["zones"]    = JsonArray();

    bool ok = store.fromJson(doc);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(1, store.stats().loadErrors);
}

void test_fromJson_missing_type_rejected() {
    JsonDocument doc;
    doc["_version"] = 1;
    doc["zones"]    = JsonArray();

    bool ok = store.fromJson(doc);
    TEST_ASSERT_FALSE(ok);
}

void test_fromJson_missing_zones_rejected() {
    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 1;
    // No "zones" key.

    bool ok = store.fromJson(doc);
    TEST_ASSERT_FALSE(ok);
}

void test_fromJson_skips_entry_missing_lat() {
    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 1;
    JsonArray zones = doc["zones"].to<JsonArray>();

    // Entry 1: valid.
    JsonObject z1 = zones.add<JsonObject>();
    z1["lat"] = 1000000;
    z1["lon"] = -1000000;
    z1["band"] = 4;

    // Entry 2: missing lat.
    JsonObject z2 = zones.add<JsonObject>();
    z2["lon"] = -2000000;

    // Entry 3: missing lon.
    JsonObject z3 = zones.add<JsonObject>();
    z3["lat"] = 3000000;

    bool ok = store.fromJson(doc);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());
    TEST_ASSERT_EQUAL(2, store.stats().entriesSkipped);
}

void test_fromJson_skips_unsupported_band() {
    JsonDocument doc;
    doc["_type"] = "v1simple_lockout_zones";
    doc["_version"] = 1;
    JsonArray zones = doc["zones"].to<JsonArray>();

    JsonObject ka = zones.add<JsonObject>();
    ka["lat"] = 1000000;
    ka["lon"] = -1000000;
    ka["band"] = 0x02;  // Ka only

    JsonObject laser = zones.add<JsonObject>();
    laser["lat"] = 2000000;
    laser["lon"] = -2000000;
    laser["band"] = 0x01;  // Laser only

    JsonObject k = zones.add<JsonObject>();
    k["lat"] = 3000000;
    k["lon"] = -3000000;
    k["band"] = 0x04;  // K

    bool ok = store.fromJson(doc);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());
    TEST_ASSERT_EQUAL(2, store.stats().entriesSkipped);
    TEST_ASSERT_EQUAL(1, store.stats().entriesLoaded);
}

void test_fromJson_defaults_optional_fields() {
    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 1;
    JsonArray zones = doc["zones"].to<JsonArray>();

    // Entry with required fields + band; everything else defaults.
    JsonObject z = zones.add<JsonObject>();
    z["lat"] = 5000000;
    z["lon"] = -8000000;
    z["band"] = 4;

    bool ok = store.fromJson(doc);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());

    const LockoutEntry* e = testIndex.at(0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(5000000, e->latE5);
    TEST_ASSERT_EQUAL(-8000000, e->lonE5);
    TEST_ASSERT_EQUAL(1350, e->radiusE5);      // Default radius
    TEST_ASSERT_EQUAL(4, e->bandMask);          // Provided band
    TEST_ASSERT_EQUAL(0, e->freqMHz);           // Default freq
    TEST_ASSERT_EQUAL(10, e->freqTolMHz);       // Default tolerance
    TEST_ASSERT_EQUAL(100, e->confidence);       // Default confidence
    TEST_ASSERT_TRUE(e->isActive());             // Always active
    TEST_ASSERT_EQUAL(0, e->missCount);
    TEST_ASSERT_EQUAL(0, e->firstSeenMs);
    TEST_ASSERT_EQUAL(0, e->lastSeenMs);
    TEST_ASSERT_EQUAL(0, e->lastPassMs);
    TEST_ASSERT_EQUAL(0, e->lastCountedMissMs);
}

void test_fromJson_always_sets_active_flag() {
    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 1;
    JsonArray zones = doc["zones"].to<JsonArray>();
    JsonObject z = zones.add<JsonObject>();
    z["lat"]   = 1000000;
    z["lon"]   = -1000000;
    z["band"]  = 4;
    z["flags"] = 0;  // Deliberately clear all flags including ACTIVE.

    bool ok = store.fromJson(doc);
    TEST_ASSERT_TRUE(ok);

    const LockoutEntry* e = testIndex.at(0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->isActive());  // Must be forced active.
}

void test_fromJson_clears_index_first() {
    // Pre-populate the index.
    testIndex.add(makeEntry(9999999, -9999999));
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());

    // Load an empty zones array.
    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 1;
    doc["zones"].to<JsonArray>();  // Empty array.

    bool ok = store.fromJson(doc);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(0, testIndex.activeCount());
}

void test_fromJson_overflow_truncates() {
    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 1;
    JsonArray zones = doc["zones"].to<JsonArray>();

    // Add more entries than capacity.
    const size_t overflow = LockoutIndex::kCapacity + 5;
    for (size_t i = 0; i < overflow; ++i) {
        JsonObject z = zones.add<JsonObject>();
        z["lat"] = static_cast<int32_t>(1000000 + i);
        z["lon"] = static_cast<int32_t>(-1000000 - i);
        z["band"] = 4;
    }

    bool ok = store.fromJson(doc);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(LockoutIndex::kCapacity, testIndex.activeCount());
    TEST_ASSERT_EQUAL(LockoutIndex::kCapacity, store.stats().entriesLoaded);
}

// ================================================================
// Dirty tracking
// ================================================================

void test_dirty_tracking() {
    TEST_ASSERT_FALSE(store.isDirty());

    store.markDirty();
    TEST_ASSERT_TRUE(store.isDirty());

    store.clearDirty();
    TEST_ASSERT_FALSE(store.isDirty());
}

void test_begin_resets_dirty_and_stats() {
    store.markDirty();
    // Force a load to bump stats.
    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 1;
    doc["zones"].to<JsonArray>();
    store.fromJson(doc);
    TEST_ASSERT_EQUAL(1, store.stats().loads);

    // begin() should reset everything.
    store.begin(&testIndex);
    TEST_ASSERT_FALSE(store.isDirty());
    TEST_ASSERT_EQUAL(0, store.stats().loads);
    TEST_ASSERT_EQUAL(0, store.stats().loadErrors);
}

void test_stats_increment_on_errors() {
    // Multiple failed loads should each increment loadErrors.
    JsonDocument bad;
    bad["_type"]    = "wrong";
    bad["_version"] = 1;

    store.fromJson(bad);
    store.fromJson(bad);
    TEST_ASSERT_EQUAL(2, store.stats().loadErrors);
    TEST_ASSERT_EQUAL(0, store.stats().loads);
}

void test_fromJson_no_index_returns_false() {
    LockoutStore orphan;
    orphan.begin(nullptr);

    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 1;
    doc["zones"].to<JsonArray>();

    bool ok = orphan.fromJson(doc);
    TEST_ASSERT_FALSE(ok);
}

// ================================================================
// Runner
// ================================================================

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // toJson
    RUN_TEST(test_toJson_empty_index);
    RUN_TEST(test_toJson_single_entry);
    RUN_TEST(test_toJson_skips_inactive_slots);
    RUN_TEST(test_toJson_skips_unsupported_band_entries);
    RUN_TEST(test_toJson_keeps_ka_entries_when_policy_enabled);
    RUN_TEST(test_toJson_all_fields_present);

    // fromJson
    RUN_TEST(test_fromJson_valid_single_entry);
    RUN_TEST(test_fromJson_roundtrip);
    RUN_TEST(test_fromJson_all_fields_survive_roundtrip);
    RUN_TEST(test_fromJson_wrong_type_rejected);
    RUN_TEST(test_fromJson_wrong_version_rejected);
    RUN_TEST(test_fromJson_missing_type_rejected);
    RUN_TEST(test_fromJson_missing_zones_rejected);
    RUN_TEST(test_fromJson_skips_entry_missing_lat);
    RUN_TEST(test_fromJson_skips_unsupported_band);
    RUN_TEST(test_fromJson_defaults_optional_fields);
    RUN_TEST(test_fromJson_always_sets_active_flag);
    RUN_TEST(test_fromJson_clears_index_first);
    RUN_TEST(test_fromJson_overflow_truncates);
    RUN_TEST(test_fromJson_no_index_returns_false);

    // Dirty tracking & stats
    RUN_TEST(test_dirty_tracking);
    RUN_TEST(test_begin_resets_dirty_and_stats);
    RUN_TEST(test_stats_increment_on_errors);

    return UNITY_END();
}

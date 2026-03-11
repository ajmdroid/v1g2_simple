#include <unity.h>
#include <ArduinoJson.h>
#include <filesystem>
#include <fstream>
#include <string>

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
static LockoutEntry makeEntry(int32_t latE5      = 1012345,
                              int32_t lonE5      = -2054321,
                              uint16_t freqMHz   = 24148,
                              uint8_t  bandMask  = 0x04,      // BAND_K
                              uint8_t  confidence = 100,
                              uint8_t  flags     = LockoutEntry::FLAG_ACTIVE
                                                 | LockoutEntry::FLAG_LEARNED) {
    LockoutEntry e;
    e.latE5       = latE5;
    e.lonE5       = lonE5;
    e.radiusE5    = 1350;
    e.areaId      = 1;
    e.bandMask    = bandMask;
    e.freqMHz     = freqMHz;
    e.freqTolMHz  = 10;
    e.freqWindowMinMHz = freqMHz;
    e.freqWindowMaxMHz = freqMHz;
    e.confidence  = confidence;
    e.flags       = flags;
    e.missCount   = 0;
    e.firstSeenMs = 1700000000000LL;
    e.lastSeenMs  = 1700000060000LL;
    e.lastPassMs  = 1700000090000LL;
    e.lastCountedMissMs = 0;
    return e;
}

static std::filesystem::path makeFsRoot(const char* testName) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("v1g2_lockout_store_" + std::string(testName));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

static std::filesystem::path resolveFsPath(const std::filesystem::path& root, const char* logicalPath) {
    std::filesystem::path relative = logicalPath ? std::filesystem::path(logicalPath) : std::filesystem::path();
    if (relative.is_absolute()) {
        relative = relative.relative_path();
    }
    return root / relative;
}

static void overwriteFileBytes(const std::filesystem::path& path,
                               std::streamoff offset,
                               const void* data,
                               size_t length) {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    TEST_ASSERT_TRUE(stream.is_open());
    stream.seekp(offset);
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
    TEST_ASSERT_TRUE(stream.good());
}

static void assertEntriesEqual(const LockoutEntry& expected, const LockoutEntry* actual) {
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL(expected.latE5, actual->latE5);
    TEST_ASSERT_EQUAL(expected.lonE5, actual->lonE5);
    TEST_ASSERT_EQUAL(expected.radiusE5, actual->radiusE5);
    TEST_ASSERT_EQUAL(expected.bandMask, actual->bandMask);
    TEST_ASSERT_EQUAL(expected.freqMHz, actual->freqMHz);
    TEST_ASSERT_EQUAL(expected.freqTolMHz, actual->freqTolMHz);
    TEST_ASSERT_EQUAL(expected.confidence, actual->confidence);
    TEST_ASSERT_EQUAL(expected.flags, actual->flags);
    TEST_ASSERT_EQUAL(expected.directionMode, actual->directionMode);
    TEST_ASSERT_EQUAL(expected.headingDeg, actual->headingDeg);
    TEST_ASSERT_EQUAL(expected.headingTolDeg, actual->headingTolDeg);
    TEST_ASSERT_EQUAL(expected.missCount, actual->missCount);
    TEST_ASSERT_EQUAL(expected.firstSeenMs, actual->firstSeenMs);
    TEST_ASSERT_EQUAL(expected.lastSeenMs, actual->lastSeenMs);
    TEST_ASSERT_EQUAL(expected.lastPassMs, actual->lastPassMs);
    TEST_ASSERT_EQUAL(expected.lastCountedMissMs, actual->lastCountedMissMs);
    TEST_ASSERT_EQUAL(expected.isActive(), actual->isActive());
    TEST_ASSERT_EQUAL(expected.isLearned(), actual->isLearned());
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
    TEST_ASSERT_EQUAL(1012345, z["lat"].as<int32_t>());
    TEST_ASSERT_EQUAL(-2054321, z["lon"].as<int32_t>());
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
    int slot = testIndex.add(makeEntry(1012345, -2054321, 34700, 0x02));
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
    e.directionMode = LockoutEntry::DIRECTION_FORWARD;
    e.headingDeg = 87;
    e.headingTolDeg = 18;
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
    TEST_ASSERT_EQUAL(LockoutEntry::DIRECTION_FORWARD, z["dir"].as<uint8_t>());
    TEST_ASSERT_EQUAL(87, z["hdg"].as<int>());
    TEST_ASSERT_EQUAL(18, z["htol"].as<uint8_t>());
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
    z["lat"]   = 1012345;
    z["lon"]   = -2054321;
    z["rad"]   = 1350;
    z["band"]  = 4;
    z["freq"]  = 24148;
    z["ftol"]  = 10;
    z["conf"]  = 100;
    z["flags"] = 5;  // ACTIVE | LEARNED
    z["dir"]   = LockoutEntry::DIRECTION_REVERSE;
    z["hdg"]   = 120;
    z["htol"]  = 25;
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
    TEST_ASSERT_EQUAL(1012345, e->latE5);
    TEST_ASSERT_EQUAL(-2054321, e->lonE5);
    TEST_ASSERT_EQUAL(1350, e->radiusE5);
    TEST_ASSERT_EQUAL(4, e->bandMask);
    TEST_ASSERT_EQUAL(24148, e->freqMHz);
    TEST_ASSERT_EQUAL(10, e->freqTolMHz);
    TEST_ASSERT_EQUAL(100, e->confidence);
    TEST_ASSERT_EQUAL(5, e->flags);
    TEST_ASSERT_EQUAL(LockoutEntry::DIRECTION_REVERSE, e->directionMode);
    TEST_ASSERT_EQUAL(120, e->headingDeg);
    TEST_ASSERT_EQUAL(25, e->headingTolDeg);
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
    orig.directionMode = LockoutEntry::DIRECTION_FORWARD;
    orig.headingDeg = 123;
    orig.headingTolDeg = 17;
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
    TEST_ASSERT_EQUAL(orig.directionMode, e->directionMode);
    TEST_ASSERT_EQUAL(orig.headingDeg, e->headingDeg);
    TEST_ASSERT_EQUAL(orig.headingTolDeg, e->headingTolDeg);
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
    TEST_ASSERT_EQUAL(135, e->radiusE5);       // Default radius (~150m)
    TEST_ASSERT_EQUAL(4, e->bandMask);          // Provided band
    TEST_ASSERT_EQUAL(0, e->freqMHz);           // Default freq
    TEST_ASSERT_EQUAL(10, e->freqTolMHz);       // Default tolerance
    TEST_ASSERT_EQUAL(100, e->confidence);       // Default confidence
    TEST_ASSERT_EQUAL(LockoutEntry::DIRECTION_ALL, e->directionMode);
    TEST_ASSERT_EQUAL(LockoutEntry::HEADING_INVALID, e->headingDeg);
    TEST_ASSERT_EQUAL(45, e->headingTolDeg);
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

void test_fromJson_invalid_direction_metadata_falls_back_to_all() {
    JsonDocument doc;
    doc["_type"]    = "v1simple_lockout_zones";
    doc["_version"] = 1;
    JsonArray zones = doc["zones"].to<JsonArray>();
    JsonObject z = zones.add<JsonObject>();
    z["lat"] = 1000000;
    z["lon"] = -1000000;
    z["band"] = 4;
    z["dir"] = LockoutEntry::DIRECTION_FORWARD;
    z["hdg"] = -1;  // Invalid heading for directional mode.

    bool ok = store.fromJson(doc);
    TEST_ASSERT_TRUE(ok);

    const LockoutEntry* e = testIndex.at(0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(LockoutEntry::DIRECTION_ALL, e->directionMode);
    TEST_ASSERT_EQUAL(LockoutEntry::HEADING_INVALID, e->headingDeg);
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
// Binary persistence tests
// ================================================================

void test_binary_save_load_atomic_roundtrip_on_real_fs() {
    const std::filesystem::path root = makeFsRoot("binary_roundtrip");
    fs::FS fs(root);

    LockoutEntry first = makeEntry(1000000, -1000000, 24120, 0x04);
    first.directionMode = LockoutEntry::DIRECTION_FORWARD;
    first.headingDeg = 87;
    first.headingTolDeg = 22;
    first.missCount = 3;

    LockoutEntry second = makeEntry(2000000, -2000000, 10525, 0x08);
    second.radiusE5 = 2700;
    second.freqTolMHz = 15;
    second.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_MANUAL;
    second.directionMode = LockoutEntry::DIRECTION_REVERSE;
    second.headingDeg = 240;
    second.headingTolDeg = 15;
    second.missCount = 5;
    second.firstSeenMs = 1234567890000LL;
    second.lastSeenMs = 1234567895000LL;
    second.lastPassMs = 1234567900000LL;
    second.lastCountedMissMs = 1234567905000LL;

    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(first));
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(second));

    TEST_ASSERT_TRUE(store.saveBinary(fs, LockoutStore::kBinaryPath));
    TEST_ASSERT_TRUE(fs.exists(LockoutStore::kBinaryPath));
    TEST_ASSERT_EQUAL(1, store.stats().saves);
    TEST_ASSERT_EQUAL(2, store.stats().entriesSaved);

    testIndex.clear();
    TEST_ASSERT_TRUE(store.loadBinary(fs, LockoutStore::kBinaryPath));
    TEST_ASSERT_EQUAL(2, testIndex.activeCount());

    const LockoutEntry* loadedFirst = nullptr;
    const LockoutEntry* loadedSecond = nullptr;
    for (size_t i = 0; i < testIndex.capacity(); ++i) {
        const LockoutEntry* entry = testIndex.at(i);
        if (!entry || !entry->isActive()) {
            continue;
        }
        if (entry->latE5 == first.latE5) {
            loadedFirst = entry;
        } else if (entry->latE5 == second.latE5) {
            loadedSecond = entry;
        }
    }

    assertEntriesEqual(first, loadedFirst);
    assertEntriesEqual(second, loadedSecond);
}

void test_loadBinary_rejects_bad_magic() {
    const std::filesystem::path root = makeFsRoot("bad_magic");
    fs::FS fs(root);

    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeEntry()));
    TEST_ASSERT_TRUE(store.saveBinary(fs, LockoutStore::kBinaryPath));

    const uint8_t badMagic[4] = {'B', 'A', 'D', '!'};
    overwriteFileBytes(resolveFsPath(root, LockoutStore::kBinaryPath), 0, badMagic, sizeof(badMagic));

    testIndex.clear();
    TEST_ASSERT_FALSE(store.loadBinary(fs, LockoutStore::kBinaryPath));
}

void test_loadBinary_rejects_wrong_version() {
    const std::filesystem::path root = makeFsRoot("bad_version");
    fs::FS fs(root);

    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeEntry()));
    TEST_ASSERT_TRUE(store.saveBinary(fs, LockoutStore::kBinaryPath));

    const uint16_t badVersion = 99;
    overwriteFileBytes(resolveFsPath(root, LockoutStore::kBinaryPath), 4, &badVersion, sizeof(badVersion));

    testIndex.clear();
    TEST_ASSERT_FALSE(store.loadBinary(fs, LockoutStore::kBinaryPath));
}

void test_loadBinary_rejects_bad_entry_count() {
    const std::filesystem::path root = makeFsRoot("bad_count");
    fs::FS fs(root);

    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeEntry()));
    TEST_ASSERT_TRUE(store.saveBinary(fs, LockoutStore::kBinaryPath));

    const uint16_t badEntryCount = static_cast<uint16_t>(LockoutIndex::kCapacity + 1);
    overwriteFileBytes(resolveFsPath(root, LockoutStore::kBinaryPath), 6, &badEntryCount, sizeof(badEntryCount));

    testIndex.clear();
    TEST_ASSERT_FALSE(store.loadBinary(fs, LockoutStore::kBinaryPath));
}

void test_loadBinary_rejects_bad_crc() {
    const std::filesystem::path root = makeFsRoot("bad_crc");
    fs::FS fs(root);

    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeEntry()));
    TEST_ASSERT_TRUE(store.saveBinary(fs, LockoutStore::kBinaryPath));

    uint8_t flipped = 0;
    const std::filesystem::path filePath = resolveFsPath(root, LockoutStore::kBinaryPath);
    {
        std::ifstream stream(filePath, std::ios::binary);
        TEST_ASSERT_TRUE(stream.is_open());
        stream.seekg(-1, std::ios::end);
        stream.read(reinterpret_cast<char*>(&flipped), 1);
        TEST_ASSERT_TRUE(stream.good());
    }
    flipped ^= 0xFFu;
    const std::streamoff lastByteOffset =
        static_cast<std::streamoff>(std::filesystem::file_size(filePath) - 1);
    overwriteFileBytes(filePath, lastByteOffset, &flipped, sizeof(flipped));

    testIndex.clear();
    TEST_ASSERT_FALSE(store.loadBinary(fs, LockoutStore::kBinaryPath));
}

void test_loadBinary_rejects_truncated_file() {
    const std::filesystem::path root = makeFsRoot("truncated");
    fs::FS fs(root);

    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeEntry()));
    TEST_ASSERT_TRUE(store.saveBinary(fs, LockoutStore::kBinaryPath));

    const std::filesystem::path filePath = resolveFsPath(root, LockoutStore::kBinaryPath);
    const auto originalSize = std::filesystem::file_size(filePath);
    std::filesystem::resize_file(filePath, originalSize - 1);

    testIndex.clear();
    TEST_ASSERT_FALSE(store.loadBinary(fs, LockoutStore::kBinaryPath));
}

void test_loadBinary_failure_does_not_mutate_live_index() {
    const std::filesystem::path root = makeFsRoot("load_failure_preserves_live_state");
    fs::FS fs(root);

    LockoutEntry persisted = makeEntry(1000000, -1000000, 24120, 0x04);
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(persisted));
    TEST_ASSERT_TRUE(store.saveBinary(fs, LockoutStore::kBinaryPath));

    LockoutEntry sentinel = makeEntry(9000000, -9000000, 10525, 0x08);
    sentinel.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_MANUAL;
    testIndex.clear();
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(sentinel));

    const uint8_t badMagic[4] = {'B', 'A', 'D', '!'};
    overwriteFileBytes(resolveFsPath(root, LockoutStore::kBinaryPath), 0, badMagic, sizeof(badMagic));

    TEST_ASSERT_FALSE(store.loadBinary(fs, LockoutStore::kBinaryPath));
    TEST_ASSERT_EQUAL(1, testIndex.activeCount());

    const LockoutEntry* live = nullptr;
    for (size_t i = 0; i < testIndex.capacity(); ++i) {
        const LockoutEntry* entry = testIndex.at(i);
        if (entry && entry->isActive()) {
            live = entry;
            break;
        }
    }
    assertEntriesEqual(sentinel, live);
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
    RUN_TEST(test_fromJson_invalid_direction_metadata_falls_back_to_all);
    RUN_TEST(test_fromJson_clears_index_first);
    RUN_TEST(test_fromJson_overflow_truncates);
    RUN_TEST(test_fromJson_no_index_returns_false);

    // binary persistence
    RUN_TEST(test_binary_save_load_atomic_roundtrip_on_real_fs);
    RUN_TEST(test_loadBinary_rejects_bad_magic);
    RUN_TEST(test_loadBinary_rejects_wrong_version);
    RUN_TEST(test_loadBinary_rejects_bad_entry_count);
    RUN_TEST(test_loadBinary_rejects_bad_crc);
    RUN_TEST(test_loadBinary_rejects_truncated_file);
    RUN_TEST(test_loadBinary_failure_does_not_mutate_live_index);

    // Dirty tracking & stats
    RUN_TEST(test_dirty_tracking);
    RUN_TEST(test_begin_resets_dirty_and_stats);
    RUN_TEST(test_stats_increment_on_errors);

    return UNITY_END();
}

#include <unity.h>
#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
#endif

#ifndef GPS_RUNTIME_STATUS_DEFINED
#define GPS_RUNTIME_STATUS_DEFINED
struct GpsRuntimeStatus {
    bool enabled = false;
    bool sampleValid = false;
    bool hasFix = false;
    float speedMph = 0.0f;
    uint8_t satellites = 0;
    float hdop = NAN;
    bool locationValid = false;
    float latitudeDeg = NAN;
    float longitudeDeg = NAN;
    bool courseValid = false;
    float courseDeg = NAN;
    uint32_t courseSampleTsMs = 0;
    uint32_t courseAgeMs = UINT32_MAX;
    uint32_t sampleTsMs = 0;
    uint32_t sampleAgeMs = UINT32_MAX;
    uint32_t fixAgeMs = UINT32_MAX;
    uint32_t injectedSamples = 0;
    bool moduleDetected = false;
    bool detectionTimedOut = false;
    bool parserActive = false;
    uint32_t hardwareSamples = 0;
    uint32_t bytesRead = 0;
    uint32_t sentencesSeen = 0;
    uint32_t sentencesParsed = 0;
    uint32_t parseFailures = 0;
    uint32_t checksumFailures = 0;
    uint32_t bufferOverruns = 0;
    uint32_t lastSentenceTsMs = 0;
};
#endif

#include "../../src/modules/lockout/lockout_entry.h"
#include "../../src/modules/lockout/lockout_index.h"
#include "../../src/modules/lockout/lockout_band_policy.cpp"
#include "../../src/modules/lockout/lockout_index.cpp"
#include "../../src/modules/lockout/lockout_store.h"
#include "../../src/modules/lockout/lockout_store.cpp"
#include "../../src/modules/lockout/lockout_enforcer.h"
#include "../../src/modules/lockout/lockout_enforcer.cpp"

static LockoutIndex testIndex;
static LockoutStore testStore;
static LockoutEnforcer enforcer;
static PacketParser parser;

static GpsRuntimeStatus makeGps(float lat, float lon) {
    GpsRuntimeStatus gps;
    gps.enabled = true;
    gps.hasFix = true;
    gps.locationValid = true;
    gps.latitudeDeg = lat;
    gps.longitudeDeg = lon;
    gps.satellites = 7;
    gps.hdop = 1.1f;
    gps.sampleValid = true;
    return gps;
}

static LockoutEntry makeSignature(uint16_t areaId, uint16_t freqMHz) {
    LockoutEntry entry;
    entry.latE5 = 1012345;
    entry.lonE5 = -2054321;
    entry.radiusE5 = 1350;
    entry.areaId = areaId;
    entry.bandMask = 0x04;
    entry.freqMHz = freqMHz;
    entry.freqTolMHz = 10;
    entry.freqWindowMinMHz = freqMHz;
    entry.freqWindowMaxMHz = freqMHz;
    entry.confidence = 100;
    entry.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.firstSeenMs = 1700000000000LL;
    entry.lastSeenMs = 1700000000000LL;
    entry.setAllTime(true);
    return entry;
}

void setUp() {
    lockoutSetKaLearningEnabled(false);
    testIndex.clear();
    testStore.begin(&testIndex);
    parser.reset();
    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_ENFORCE;
    enforcer.begin(&settingsManager, &testIndex, &testStore);
}

void tearDown() {}

void test_learned_subset_in_area_still_mutes() {
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeSignature(11, 24147)));
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeSignature(11, 24169)));

    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24147, true, true)});
    const LockoutEnforcerResult result =
        enforcer.process(1000, 1700000000000LL, parser, makeGps(10.12345f, -20.54321f));

    TEST_ASSERT_TRUE(result.shouldMute);
    TEST_ASSERT_EQUAL(1, result.supportedAlertCount);
    TEST_ASSERT_EQUAL(1, result.matchedAlertCount);
}

void test_new_same_band_frequency_in_area_fails_open() {
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeSignature(11, 24147)));
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeSignature(11, 24169)));

    parser.setAlerts({
        AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24147, true, true),
        AlertData::create(BAND_K, DIR_SIDE, 3, 0, 24169, true, false),
        AlertData::create(BAND_K, DIR_REAR, 2, 0, 24130, true, false)
    });
    const LockoutEnforcerResult result =
        enforcer.process(1000, 1700000000000LL, parser, makeGps(10.12345f, -20.54321f));

    TEST_ASSERT_FALSE(result.shouldMute);
    TEST_ASSERT_EQUAL(3, result.supportedAlertCount);
    TEST_ASSERT_EQUAL(0, result.matchedAlertCount);
}

void test_same_frequency_duplicates_fail_open() {
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeSignature(22, 24147)));

    parser.setAlerts({
        AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24147, true, true),
        AlertData::create(BAND_K, DIR_SIDE, 3, 0, 24147, true, false)
    });
    const LockoutEnforcerResult result =
        enforcer.process(1000, 1700000000000LL, parser, makeGps(10.12345f, -20.54321f));

    TEST_ASSERT_FALSE(result.shouldMute);
    TEST_ASSERT_EQUAL(2, result.supportedAlertCount);
    TEST_ASSERT_EQUAL(0, result.matchedAlertCount);
}

void test_record_hit_expands_runtime_frequency_window() {
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(makeSignature(33, 24147)));
    TEST_ASSERT_FALSE(testIndex.evaluate(1012345, -2054321, 0x04, 24150).shouldMute);

    TEST_ASSERT_EQUAL_UINT8(101, testIndex.recordHit(0, 1700000000000LL, 24150, 9));

    TEST_ASSERT_TRUE(testIndex.evaluate(1012345, -2054321, 0x04, 24150).shouldMute);
}

void test_allTime_false_blocks_clean_pass_when_hour_inactive() {
    LockoutEntry e = makeSignature(44, 24147);
    e.setAllTime(false);
    e.activeHourMask = 0;  // No hours active.
    e.confidence = 40;
    e.missCount = 0;
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(e));

    // recordCleanPassWithPolicy should not count because hourIsExpected is false.
    LockoutCleanPassResult result =
        testIndex.recordCleanPassWithPolicy(0, 1700000000000LL, 12, 0, 3);
    TEST_ASSERT_FALSE(result.counted);
    TEST_ASSERT_EQUAL(40, result.confidence);
}

void test_allTime_true_allows_clean_pass_decay() {
    LockoutEntry e = makeSignature(55, 24147);
    e.setAllTime(true);
    e.confidence = 40;
    e.missCount = 0;
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(e));

    testIndex.recordCleanPass(0, 1700000000000LL);
    const LockoutEntry* stored = testIndex.at(0);
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_TRUE(stored->confidence < 40);
}

void test_v1_json_migration_roundtrip_preserves_entries() {
    // Build a v1 JSON doc with two K-band zones.
    JsonDocument v1Doc;
    v1Doc["_type"] = "v1simple_lockout_zones";
    v1Doc["_version"] = 1;
    JsonArray zones = v1Doc["zones"].to<JsonArray>();
    {
        JsonObject z = zones.add<JsonObject>();
        z["lat"] = 1012345;
        z["lon"] = -2054321;
        z["band"] = 4;
        z["freq"] = 24147;
        z["conf"] = 80;
    }
    {
        JsonObject z = zones.add<JsonObject>();
        z["lat"] = 3056789;
        z["lon"] = -4098765;
        z["band"] = 4;
        z["freq"] = 24169;
        z["conf"] = 60;
    }

    // Load v1.
    TEST_ASSERT_TRUE(testStore.fromJson(v1Doc));
    TEST_ASSERT_EQUAL(2, testIndex.activeCount());

    // Save as v2.
    JsonDocument v2Doc;
    testStore.toJson(v2Doc);
    TEST_ASSERT_EQUAL(2, v2Doc["_version"].as<int>());
    TEST_ASSERT_FALSE(v2Doc["areas"].isNull());

    // Reload v2 into clean index.
    testIndex.clear();
    TEST_ASSERT_TRUE(testStore.fromJson(v2Doc));
    TEST_ASSERT_EQUAL(2, testIndex.activeCount());

    // Verify both entries survived migration.
    bool found24147 = false, found24169 = false;
    for (size_t i = 0; i < testIndex.capacity(); ++i) {
        const LockoutEntry* e = testIndex.at(i);
        if (!e || !e->isActive()) continue;
        if (e->freqMHz == 24147) found24147 = true;
        if (e->freqMHz == 24169) found24169 = true;
    }
    TEST_ASSERT_TRUE(found24147);
    TEST_ASSERT_TRUE(found24169);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_learned_subset_in_area_still_mutes);
    RUN_TEST(test_new_same_band_frequency_in_area_fails_open);
    RUN_TEST(test_same_frequency_duplicates_fail_open);
    RUN_TEST(test_record_hit_expands_runtime_frequency_window);
    RUN_TEST(test_allTime_false_blocks_clean_pass_when_hour_inactive);
    RUN_TEST(test_allTime_true_allows_clean_pass_decay);
    RUN_TEST(test_v1_json_migration_roundtrip_preserves_entries);
    return UNITY_END();
}

#include <unity.h>

// Mocks first — must precede real headers that reference Arduino types.
#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
#endif

// GpsRuntimeStatus is a plain struct — define it here to avoid pulling
// the full GpsRuntimeModule with its UART/hardware dependencies.
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

// Pull implementation for UNIT_TEST build.
#include "../../src/modules/lockout/lockout_entry.h"
#include "../../src/modules/lockout/lockout_index.h"
#include "../../src/modules/lockout/lockout_index.cpp"
#include "../../src/modules/lockout/lockout_enforcer.h"
#include "../../src/modules/lockout/lockout_enforcer.cpp"

static LockoutIndex testIndex;
static LockoutEnforcer enforcer;
static PacketParser parser;

// --- Helpers ---

static GpsRuntimeStatus makeGps(float lat, float lon, bool fix = true) {
    GpsRuntimeStatus g;
    g.enabled = true;
    g.hasFix = fix;
    g.locationValid = fix;
    g.latitudeDeg = lat;
    g.longitudeDeg = lon;
    g.satellites = fix ? 7 : 0;
    g.hdop = fix ? 1.3f : NAN;
    g.sampleValid = fix;
    return g;
}

static LockoutEntry makeEntry(float latDeg, float lonDeg,
                              uint8_t bandMask = 0x04,    // BAND_K
                              uint16_t freqMHz = 24148,
                              uint16_t radiusE5 = 1350) {
    LockoutEntry e;
    e.latE5      = static_cast<int32_t>(lroundf(latDeg * 100000.0f));
    e.lonE5      = static_cast<int32_t>(lroundf(lonDeg * 100000.0f));
    e.radiusE5   = radiusE5;
    e.bandMask   = bandMask;
    e.freqMHz    = freqMHz;
    e.freqTolMHz = 10;
    e.confidence = 100;
    e.flags      = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    e.firstSeenMs = 1700000000000LL;
    e.lastSeenMs  = 1700000060000LL;
    return e;
}

void setUp() {
    testIndex.clear();
    parser.reset();
    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_SHADOW;
    enforcer.begin(&settingsManager, &testIndex);
}

void tearDown() {}

// ================================================================
// Gate: mode OFF
// ================================================================

void test_mode_off_skips_evaluation() {
    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_OFF;
    enforcer.begin(&settingsManager, &testIndex);

    testIndex.add(makeEntry(37.36277f, -79.23221f));
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24148, true, true)});

    GpsRuntimeStatus gps = makeGps(37.36277f, -79.23221f);
    LockoutEnforcerResult r = enforcer.process(1000, 1700000000000LL, parser, gps);

    TEST_ASSERT_FALSE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
    TEST_ASSERT_EQUAL(1, enforcer.stats().skippedOff);
}

// ================================================================
// Gate: no GPS fix
// ================================================================

void test_no_gps_fix_skips_evaluation() {
    testIndex.add(makeEntry(37.36277f, -79.23221f));
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24148, true, true)});

    GpsRuntimeStatus gps = makeGps(0, 0, false);
    LockoutEnforcerResult r = enforcer.process(1000, 0, parser, gps);

    TEST_ASSERT_FALSE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
    TEST_ASSERT_EQUAL(1, enforcer.stats().skippedNoFix);
}

// ================================================================
// Gate: no GPS location (fix but invalid coords)
// ================================================================

void test_no_gps_location_skips_evaluation() {
    testIndex.add(makeEntry(37.36277f, -79.23221f));
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24148, true, true)});

    GpsRuntimeStatus gps;
    gps.hasFix = true;
    gps.locationValid = false;  // Fix but no location
    gps.latitudeDeg = NAN;
    gps.longitudeDeg = NAN;
    LockoutEnforcerResult r = enforcer.process(1000, 0, parser, gps);

    TEST_ASSERT_FALSE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
    TEST_ASSERT_EQUAL(1, enforcer.stats().skippedNoGps);
}

// ================================================================
// No alerts: evaluates but no match
// ================================================================

void test_no_alerts_evaluates_no_match() {
    testIndex.add(makeEntry(37.36277f, -79.23221f));
    // parser has no alerts (default)

    GpsRuntimeStatus gps = makeGps(37.36277f, -79.23221f);
    LockoutEnforcerResult r = enforcer.process(1000, 1700000000000LL, parser, gps);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
    TEST_ASSERT_EQUAL(1, enforcer.stats().evaluations);
    TEST_ASSERT_EQUAL(0, enforcer.stats().matches);
}

// ================================================================
// SHADOW mode: match produces shouldMute but no side-effects
// ================================================================

void test_shadow_mode_match() {
    testIndex.add(makeEntry(37.36277f, -79.23221f));
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24148, true, true)});

    GpsRuntimeStatus gps = makeGps(37.36277f, -79.23221f);
    LockoutEnforcerResult r = enforcer.process(1000, 1700000100000LL, parser, gps);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_TRUE(r.shouldMute);
    TEST_ASSERT_EQUAL(0, r.matchIndex);
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_SHADOW, r.mode);
    TEST_ASSERT_EQUAL(1, enforcer.stats().evaluations);
    TEST_ASSERT_EQUAL(1, enforcer.stats().matches);

    // Verify confidence was bumped via recordHit
    TEST_ASSERT_EQUAL(101, testIndex.at(0)->confidence);
    // Verify lastSeenMs was updated
    TEST_ASSERT_EQUAL(1700000100000LL, testIndex.at(0)->lastSeenMs);
}

// ================================================================
// ADVISORY mode: same behavior as SHADOW (for now)
// ================================================================

void test_advisory_mode_match() {
    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_ADVISORY;
    enforcer.begin(&settingsManager, &testIndex);

    testIndex.add(makeEntry(37.36277f, -79.23221f));
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24148, true, true)});

    GpsRuntimeStatus gps = makeGps(37.36277f, -79.23221f);
    LockoutEnforcerResult r = enforcer.process(1000, 1700000100000LL, parser, gps);

    TEST_ASSERT_TRUE(r.shouldMute);
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_ADVISORY, r.mode);
}

// ================================================================
// No match when outside lockout zone
// ================================================================

void test_no_match_outside_zone() {
    testIndex.add(makeEntry(37.36277f, -79.23221f, 0x04, 24148, 1350));
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24148, true, true)});

    // Location far away from the lockout zone
    GpsRuntimeStatus gps = makeGps(38.0f, -80.0f);
    LockoutEnforcerResult r = enforcer.process(1000, 1700000000000LL, parser, gps);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
    TEST_ASSERT_EQUAL(-1, r.matchIndex);
}

// ================================================================
// No match when wrong band
// ================================================================

void test_no_match_wrong_band() {
    testIndex.add(makeEntry(37.36277f, -79.23221f, 0x04, 24148));  // K-band only
    parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true)});

    GpsRuntimeStatus gps = makeGps(37.36277f, -79.23221f);
    LockoutEnforcerResult r = enforcer.process(1000, 1700000000000LL, parser, gps);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
}

// ================================================================
// No match when wrong frequency
// ================================================================

void test_no_match_wrong_freq() {
    testIndex.add(makeEntry(37.36277f, -79.23221f, 0x04, 24148, 1350));
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24300, true, true)});

    GpsRuntimeStatus gps = makeGps(37.36277f, -79.23221f);
    LockoutEnforcerResult r = enforcer.process(1000, 1700000000000LL, parser, gps);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
}

// ================================================================
// Stats accumulate across multiple calls
// ================================================================

void test_stats_accumulate() {
    testIndex.add(makeEntry(37.36277f, -79.23221f));
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24148, true, true)});
    GpsRuntimeStatus gps = makeGps(37.36277f, -79.23221f);

    enforcer.process(1000, 1700000000000LL, parser, gps);
    enforcer.process(2000, 1700000001000LL, parser, gps);
    enforcer.process(3000, 1700000002000LL, parser, gps);

    TEST_ASSERT_EQUAL(3, enforcer.stats().evaluations);
    TEST_ASSERT_EQUAL(3, enforcer.stats().matches);
}

// ================================================================
// Empty index: evaluates, no match
// ================================================================

void test_empty_index_no_match() {
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24148, true, true)});
    GpsRuntimeStatus gps = makeGps(37.36277f, -79.23221f);

    LockoutEnforcerResult r = enforcer.process(1000, 1700000000000LL, parser, gps);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
    TEST_ASSERT_EQUAL(0, enforcer.stats().matches);
}

// ================================================================
// lastResult() reflects most recent call
// ================================================================

void test_lastResult_reflects_latest() {
    testIndex.add(makeEntry(37.36277f, -79.23221f));
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24148, true, true)});

    // First call: match
    GpsRuntimeStatus gps1 = makeGps(37.36277f, -79.23221f);
    enforcer.process(1000, 1700000000000LL, parser, gps1);
    TEST_ASSERT_TRUE(enforcer.lastResult().shouldMute);

    // Second call: no match (far away)
    GpsRuntimeStatus gps2 = makeGps(40.0f, -80.0f);
    enforcer.process(2000, 1700000001000LL, parser, gps2);
    TEST_ASSERT_FALSE(enforcer.lastResult().shouldMute);
}

// ================================================================
// Epoch 0 still evaluates (time not yet available)
// ================================================================

void test_epoch_zero_still_evaluates() {
    testIndex.add(makeEntry(37.36277f, -79.23221f));
    parser.setAlerts({AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24148, true, true)});
    GpsRuntimeStatus gps = makeGps(37.36277f, -79.23221f);

    // epoch = 0 means time not synced yet — should still evaluate,
    // but recordHit will get epochMs=0 (index handles this gracefully).
    LockoutEnforcerResult r = enforcer.process(1000, 0, parser, gps);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_TRUE(r.shouldMute);
}

// ================================================================
// Runner
// ================================================================

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();

    RUN_TEST(test_mode_off_skips_evaluation);
    RUN_TEST(test_no_gps_fix_skips_evaluation);
    RUN_TEST(test_no_gps_location_skips_evaluation);
    RUN_TEST(test_no_alerts_evaluates_no_match);
    RUN_TEST(test_shadow_mode_match);
    RUN_TEST(test_advisory_mode_match);
    RUN_TEST(test_no_match_outside_zone);
    RUN_TEST(test_no_match_wrong_band);
    RUN_TEST(test_no_match_wrong_freq);
    RUN_TEST(test_stats_accumulate);
    RUN_TEST(test_empty_index_no_match);
    RUN_TEST(test_lastResult_reflects_latest);
    RUN_TEST(test_epoch_zero_still_evaluates);

    return UNITY_END();
}

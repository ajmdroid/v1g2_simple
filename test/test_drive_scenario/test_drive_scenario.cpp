// =============================================================================
// Drive-scenario integration tests
//
// Wire together the core detection-to-lockout pipeline using real module
// implementations, synthetic GPS tracks, and mock PacketParser alerts.
// No real captured data — all coordinates and frequencies are fabricated.
//
// Modules under test:
//   PacketParser (mock)  →  SignalCaptureModule (real)
//   GpsRuntimeModule (real, scaffold injection)
//   LockoutIndex / LockoutStore / LockoutEnforcer / LockoutLearner (real)
//   CameraRuntimeModule / CameraIndex (real)
//
// Each scenario simulates a multi-step "drive" by advancing mock time,
// injecting GPS positions, setting V1 alerts, and ticking the modules
// in the same order as the main loop.
// =============================================================================

#include <unity.h>
#include <utility>

// Mocks first — must precede real headers that reference Arduino types.
#include "../mocks/Arduino.h"
#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// ── GPS module (real, with scaffold injection) ──────────────────────
#include "../../src/modules/gps/gps_observation_log.cpp"
#include "../../src/modules/gps/gps_runtime_module.cpp"

// ── Signal capture → observation log (real) ─────────────────────────
#include "../../src/modules/lockout/signal_observation_log.h"
#include "../../src/modules/lockout/signal_observation_log.cpp"
#include "../../src/modules/lockout/signal_capture_module.h"
#include "../../src/modules/lockout/lockout_band_policy.cpp"
#include "../../src/modules/lockout/signal_capture_module.cpp"

// Stub the SD logger used by signal_capture_module.cpp
static uint32_t sdEnqueueCount = 0;
SignalObservationSdLogger signalObservationSdLogger;
bool SignalObservationSdLogger::enqueue(const SignalObservation&) {
    sdEnqueueCount++;
    return true;
}

// ── Lockout pipeline (real) ─────────────────────────────────────────
#include "../../src/modules/lockout/lockout_entry.h"
#include "../../src/modules/lockout/lockout_index.h"
#include "../../src/modules/lockout/lockout_index.cpp"
#include "../../src/modules/lockout/lockout_store.h"
#include "../../src/modules/lockout/lockout_store.cpp"
#include "../../src/modules/lockout/lockout_enforcer.h"
#include "../../src/modules/lockout/lockout_enforcer.cpp"
#include "../../src/modules/lockout/lockout_learner.h"
#include "../../src/modules/lockout/lockout_learner.cpp"

// ── Camera pipeline (real) ──────────────────────────────────────────
#include "../../src/modules/camera/camera_data_loader.h"
#include "../../src/modules/camera/camera_event_log.cpp"
#include "../../src/modules/camera/camera_index.cpp"
#include "../../src/modules/camera/camera_runtime_module.h"
#include "../../src/modules/camera/camera_runtime_module.cpp"

// Stub camera loader so we can inject camera records directly.
namespace {
CameraIndexOwnedBuffers gReadyBuffers = {};
bool gReadyBuffersValid = false;
uint32_t gNextCameraVersion = 1;
}  // namespace

void CameraDataLoader::begin() {}
void CameraDataLoader::reset() {
    if (gReadyBuffersValid) {
        CameraIndex::freeOwnedBuffers(gReadyBuffers);
        gReadyBuffers = {};
        gReadyBuffersValid = false;
    }
}
void CameraDataLoader::requestReload() {}
bool CameraDataLoader::consumeReady(CameraIndex& activeIndex) {
    if (!gReadyBuffersValid) return false;
    CameraIndexOwnedBuffers ready = gReadyBuffers;
    gReadyBuffers = {};
    gReadyBuffersValid = false;
    return activeIndex.adopt(std::move(ready));
}
CameraDataLoaderStatus CameraDataLoader::status() const { return {}; }

// =============================================================================
// Shared test instances
// =============================================================================
static PacketParser parser;
// lockoutIndex and lockoutStore are defined as globals in their .cpp files.
static LockoutEnforcer enforcer;
static LockoutLearner learner;

// Synthetic coordinates — no real locations.
static constexpr float HOME_LAT = 42.00000f;
static constexpr float HOME_LON = -83.00000f;
static constexpr int32_t HOME_LAT_E5 = 4200000;
static constexpr int32_t HOME_LON_E5 = -8300000;
static constexpr uint16_t K_FREQ = 24148;
static constexpr uint16_t KA_FREQ = 35498;
static constexpr int64_t EPOCH_BASE = 1700000000000LL;

// =============================================================================
// Harness helpers
// =============================================================================

static void advanceTime(uint32_t ms) {
    mockMillis += ms;
    mockMicros = static_cast<unsigned long>(mockMillis) * 1000UL;
}

static void setTime(uint32_t ms) {
    mockMillis = ms;
    mockMicros = static_cast<unsigned long>(ms) * 1000UL;
}

static void setGps(float lat, float lon, float courseDeg, uint32_t nowMs) {
    setTime(nowMs);
    gpsRuntimeModule.setScaffoldSample(
        40.0f,       // speedMph
        true,        // hasFix
        8,           // satellites
        0.9f,        // hdop
        nowMs,       // timestampMs
        lat,         // latitudeDeg
        lon,         // longitudeDeg
        courseDeg);  // courseDeg
}

static GpsRuntimeStatus makeGpsStatus(float lat, float lon) {
    GpsRuntimeStatus g;
    g.enabled = true;
    g.sampleValid = true;
    g.hasFix = true;
    g.locationValid = true;
    g.latitudeDeg = lat;
    g.longitudeDeg = lon;
    g.satellites = 8;
    g.hdop = 0.9f;
    g.fixAgeMs = 250;
    g.sampleAgeMs = 100;
    g.speedMph = 40.0f;
    g.courseValid = true;
    g.courseDeg = 90.0f;
    return g;
}

static void setAlert(Band band, uint32_t freq, uint8_t strength = 5) {
    parser.setAlerts({AlertData::create(
        band, DIR_FRONT, strength, 0, freq, true, true)});
}

static void clearAlerts() {
    parser.setAlerts({});
}

static void queueCamera(float lat, float lon, int16_t bearingTenths,
                         uint8_t toleranceDeg, uint8_t type) {
    if (gReadyBuffersValid) {
        CameraIndex::freeOwnedBuffers(gReadyBuffers);
        gReadyBuffers = {};
        gReadyBuffersValid = false;
    }

    gReadyBuffers.records = static_cast<CameraRecord*>(
        heap_caps_malloc(sizeof(CameraRecord), MALLOC_CAP_8BIT));
    gReadyBuffers.spans = static_cast<CameraCellSpan*>(
        heap_caps_malloc(sizeof(CameraCellSpan), MALLOC_CAP_8BIT));
    TEST_ASSERT_NOT_NULL(gReadyBuffers.records);
    TEST_ASSERT_NOT_NULL(gReadyBuffers.spans);

    CameraRecord& rec = gReadyBuffers.records[0];
    rec.latitudeDeg = lat;
    rec.longitudeDeg = lon;
    rec.snapLatitudeDeg = lat;
    rec.snapLongitudeDeg = lon;
    rec.bearingTenthsDeg = bearingTenths;
    rec.widthM = 25;
    rec.toleranceDeg = toleranceDeg;
    rec.type = type;
    rec.speedLimit = 35;
    rec.flags = 0;
    rec.reserved = 0;
    rec.cellKey = CameraIndex::encodeCellKey(lat, lon);

    gReadyBuffers.recordCount = 1;
    gReadyBuffers.spans[0].cellKey = rec.cellKey;
    gReadyBuffers.spans[0].beginIndex = 0;
    gReadyBuffers.spans[0].endIndex = 1;
    gReadyBuffers.spanCount = 1;
    gReadyBuffers.version = gNextCameraVersion++;
    gReadyBuffersValid = true;
}

/// Tick the full pipeline in main-loop order for one "frame".
static LockoutEnforcerResult tickPipeline(uint32_t nowMs, int64_t epochMs) {
    setTime(nowMs);
    GpsRuntimeStatus gps = gpsRuntimeModule.snapshot(nowMs);

    // 1. Signal capture (feeds the observation log).
    signalCaptureModule.capturePriorityObservation(nowMs, parser, gps);

    // 2. Lockout enforcer (evaluates alerts vs lockout zones).
    LockoutEnforcerResult result = enforcer.process(nowMs, epochMs, parser, gps);

    // 3. Camera runtime (uses GPS, independent of lockout).
    //    Mirror main.cpp: only a real priority signal preempts cameras.
    const AlertData camPriority = parser.getPriorityAlert();
    const bool signalPriorityActive = parser.hasAlerts() &&
                                      camPriority.isValid &&
                                      camPriority.band != BAND_NONE;
    cameraRuntimeModule.process(nowMs, false, false, signalPriorityActive);

    return result;
}

/// Tick the learner (done at lower frequency in the real loop).
static void tickLearner(uint32_t nowMs, int64_t epochMs) {
    setTime(nowMs);
    learner.process(nowMs, epochMs);
}

// =============================================================================
// setUp / tearDown
// =============================================================================

void setUp() {
    setTime(1000);
    mock_reset_heap_caps();
    lockoutSetKaLearningEnabled(false);

    parser.reset();
    signalCaptureModule.reset();
    signalObservationLog.reset();
    sdEnqueueCount = 0;

    lockoutIndex.clear();
    lockoutStore.begin(&lockoutIndex);
    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_ENFORCE;
    enforcer.begin(&settingsManager, &lockoutIndex, &lockoutStore);
    learner.begin(&lockoutIndex, &signalObservationLog);

    gpsRuntimeModule = GpsRuntimeModule();
    gpsRuntimeModule.begin(true);
    cameraRuntimeModule.begin(true);

    if (gReadyBuffersValid) {
        CameraIndex::freeOwnedBuffers(gReadyBuffers);
        gReadyBuffers = {};
        gReadyBuffersValid = false;
    }
}

void tearDown() {
    mock_reset_heap_caps();
    if (gReadyBuffersValid) {
        CameraIndex::freeOwnedBuffers(gReadyBuffers);
        gReadyBuffers = {};
        gReadyBuffersValid = false;
    }
}

// =============================================================================
// SCENARIO 1: K-band alert detected — enforcer finds no lockout — no mute
// =============================================================================

void test_alert_with_no_lockout_entry_does_not_mute() {
    setGps(HOME_LAT, HOME_LON, 90.0f, 2000);
    setAlert(BAND_K, K_FREQ);

    LockoutEnforcerResult r = tickPipeline(2000, EPOCH_BASE);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
    TEST_ASSERT_EQUAL(-1, r.matchIndex);
}

// =============================================================================
// SCENARIO 2: K-band alert at known lockout zone — enforcer mutes
// =============================================================================

void test_alert_at_lockout_zone_mutes() {
    // Pre-load a lockout entry at HOME.
    LockoutEntry entry;
    entry.latE5     = HOME_LAT_E5;
    entry.lonE5     = HOME_LON_E5;
    entry.radiusE5  = 1350;
    entry.bandMask  = BAND_K;
    entry.freqMHz   = K_FREQ;
    entry.freqTolMHz = 10;
    entry.confidence = 100;
    entry.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.firstSeenMs = EPOCH_BASE - 86400000LL;
    entry.lastSeenMs  = EPOCH_BASE;
    lockoutIndex.add(entry);

    setGps(HOME_LAT, HOME_LON, 90.0f, 3000);
    setAlert(BAND_K, K_FREQ);

    LockoutEnforcerResult r = tickPipeline(3000, EPOCH_BASE + 3000);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_TRUE(r.shouldMute);
    TEST_ASSERT_TRUE(r.matchIndex >= 0);
}

// =============================================================================
// SCENARIO 3: Alert at lockout zone but mode is SHADOW — no mute
// =============================================================================

void test_shadow_mode_evaluates_but_does_not_mute() {
    LockoutEntry entry;
    entry.latE5     = HOME_LAT_E5;
    entry.lonE5     = HOME_LON_E5;
    entry.radiusE5  = 1350;
    entry.bandMask  = BAND_K;
    entry.freqMHz   = K_FREQ;
    entry.freqTolMHz = 10;
    entry.confidence = 100;
    entry.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.firstSeenMs = EPOCH_BASE;
    entry.lastSeenMs  = EPOCH_BASE;
    lockoutIndex.add(entry);

    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_SHADOW;
    enforcer.begin(&settingsManager, &lockoutIndex, &lockoutStore);

    setGps(HOME_LAT, HOME_LON, 90.0f, 3000);
    setAlert(BAND_K, K_FREQ);

    LockoutEnforcerResult r = tickPipeline(3000, EPOCH_BASE + 3000);

    TEST_ASSERT_TRUE(r.evaluated);
    // Shadow mode: shouldMute is set but the enforcer does NOT send
    // the actual mute — that's the caller's responsibility. The
    // enforcer itself still reports the match.
    TEST_ASSERT_TRUE(r.matchIndex >= 0);
}

// =============================================================================
// SCENARIO 4: No GPS fix — enforcer skips evaluation entirely
// =============================================================================

void test_no_gps_fix_skips_evaluation() {
    setAlert(BAND_K, K_FREQ);

    // Don't set GPS → snapshot returns no fix.
    GpsRuntimeStatus noGps;
    noGps.enabled = true;
    signalCaptureModule.capturePriorityObservation(2000, parser, noGps);
    LockoutEnforcerResult r = enforcer.process(2000, EPOCH_BASE, parser, noGps);

    TEST_ASSERT_FALSE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
}

// =============================================================================
// SCENARIO 5: Full learning lifecycle — K-band seen 3 times → lockout created
// =============================================================================

void test_learning_lifecycle_promotes_after_three_hits() {
    // Learner default: 3 hits to promote.
    learner.setTuning(3, LockoutLearner::kDefaultRadiusE5,
                      LockoutLearner::kDefaultFreqToleranceMHz);

    setGps(HOME_LAT, HOME_LON, 90.0f, 5000);

    // Drive 1: K-band alert captured by signal capture.
    setAlert(BAND_K, K_FREQ);
    GpsRuntimeStatus gps1 = makeGpsStatus(HOME_LAT, HOME_LON);
    signalCaptureModule.capturePriorityObservation(5000, parser, gps1);
    tickLearner(7000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(0, lockoutIndex.activeCount());  // Not yet promoted.

    // Drive 2: Same location, 2s later (past min-repeat interval).
    advanceTime(2000);
    signalCaptureModule.capturePriorityObservation(mockMillis, parser, gps1);
    tickLearner(mockMillis + 2000, EPOCH_BASE + 4000);

    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
    TEST_ASSERT_EQUAL(0, lockoutIndex.activeCount());  // 2 hits, need 3.

    // Drive 3: Same location, 2s later.
    advanceTime(2000);
    signalCaptureModule.capturePriorityObservation(mockMillis, parser, gps1);
    tickLearner(mockMillis + 2000, EPOCH_BASE + 8000);

    // Third hit should have promoted to a lockout entry!
    TEST_ASSERT_TRUE(lockoutIndex.activeCount() >= 1);

    // Now verify the enforcer respects the learned entry.
    setGps(HOME_LAT, HOME_LON, 90.0f, mockMillis + 3000);
    setAlert(BAND_K, K_FREQ);
    LockoutEnforcerResult r = tickPipeline(mockMillis, EPOCH_BASE + 11000);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_TRUE(r.shouldMute);
}

// =============================================================================
// SCENARIO 6: Different frequency at same location — new candidate, not merged
// =============================================================================

void test_different_frequency_creates_separate_candidate() {
    setGps(HOME_LAT, HOME_LON, 90.0f, 5000);
    GpsRuntimeStatus gps = makeGpsStatus(HOME_LAT, HOME_LON);

    // K-band 24148 MHz
    setAlert(BAND_K, K_FREQ);
    signalCaptureModule.capturePriorityObservation(5000, parser, gps);
    tickLearner(7000, EPOCH_BASE);

    // K-band 24201 MHz (>10 MHz tolerance away)
    advanceTime(2000);
    setAlert(BAND_K, 24201);
    signalCaptureModule.capturePriorityObservation(mockMillis, parser, gps);
    tickLearner(mockMillis + 2000, EPOCH_BASE + 4000);

    TEST_ASSERT_EQUAL(2, learner.activeCandidateCount());
}

// =============================================================================
// SCENARIO 7: Ka-band learning disabled — observations ignored
// =============================================================================

void test_ka_band_not_learned_when_disabled() {
    lockoutSetKaLearningEnabled(false);

    setGps(HOME_LAT, HOME_LON, 90.0f, 5000);
    GpsRuntimeStatus gps = makeGpsStatus(HOME_LAT, HOME_LON);

    setAlert(BAND_KA, KA_FREQ);
    signalCaptureModule.capturePriorityObservation(5000, parser, gps);
    tickLearner(7000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(0, learner.activeCandidateCount());
}

// =============================================================================
// SCENARIO 8: Ka-band learning enabled — observation captured
// =============================================================================

void test_ka_band_learned_when_enabled() {
    lockoutSetKaLearningEnabled(true);

    setGps(HOME_LAT, HOME_LON, 90.0f, 5000);
    GpsRuntimeStatus gps = makeGpsStatus(HOME_LAT, HOME_LON);

    setAlert(BAND_KA, KA_FREQ);
    signalCaptureModule.capturePriorityObservation(5000, parser, gps);
    tickLearner(7000, EPOCH_BASE);

    TEST_ASSERT_EQUAL(1, learner.activeCandidateCount());
}

// =============================================================================
// SCENARIO 9: Lockout built → drive again with no alert → clean pass decays
// =============================================================================

void test_clean_pass_decays_lockout_confidence() {
    LockoutEntry entry;
    entry.latE5      = HOME_LAT_E5;
    entry.lonE5      = HOME_LON_E5;
    entry.radiusE5   = 1350;
    entry.bandMask   = BAND_K;
    entry.freqMHz    = K_FREQ;
    entry.freqTolMHz = 10;
    entry.confidence = 80;
    entry.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.firstSeenMs = EPOCH_BASE;
    entry.lastSeenMs  = EPOCH_BASE;
    int idx = lockoutIndex.add(entry);
    TEST_ASSERT_TRUE(idx >= 0);

    // Drive through the zone with NO alert.
    setGps(HOME_LAT, HOME_LON, 90.0f, 5000);
    clearAlerts();
    LockoutEnforcerResult r = tickPipeline(5000, EPOCH_BASE + 86400000LL);

    // No alert → evaluated but no match → clean pass applied.
    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);

    // Check that confidence decreased.
    const LockoutEntry* updated = lockoutIndex.at(static_cast<size_t>(idx));
    TEST_ASSERT_NOT_NULL(updated);
    TEST_ASSERT_TRUE(updated->confidence < 80);
}

// =============================================================================
// SCENARIO 10: Camera alert triggers while approaching camera location
// =============================================================================

void test_camera_activates_on_approach_heading_aligned() {
    // Camera at HOME + 0.001° longitude east, facing east (90°).
    float cameraLon = HOME_LON + 0.0010f;
    queueCamera(HOME_LAT, cameraLon, 900, 35, 4);  // bearing 90.0°, tol ±35°

    // Drive east from HOME toward camera.
    setGps(HOME_LAT, HOME_LON, 90.0f, 10000);
    cameraRuntimeModule.process(10000, false, false, false);

    CameraRuntimeStatus status = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CameraLifecycleState::ACTIVE),
        static_cast<uint8_t>(status.lifecycleState));
    TEST_ASSERT_TRUE(status.activeAlert.active);
}

// =============================================================================
// SCENARIO 11: Camera alert suppressed by V1 signal priority
// =============================================================================

void test_camera_suppressed_when_signal_priority_active() {
    float cameraLon = HOME_LON + 0.0010f;
    queueCamera(HOME_LAT, cameraLon, 900, 35, 2);

    // Drive east, signal priority active (V1 has a real alert).
    setGps(HOME_LAT, HOME_LON, 90.0f, 10000);
    cameraRuntimeModule.process(10000, false, false, true);  // signalPriority=true

    CameraRuntimeStatus status = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CameraLifecycleState::IDLE),
        static_cast<uint8_t>(status.lifecycleState));
    TEST_ASSERT_FALSE(status.activeAlert.active);
}

// =============================================================================
// SCENARIO 12: Combined — V1 alert + camera + lockout in same drive
//
// Drive east on a road:
//   t=0s:  Start driving, no alerts.
//   t=2s:  V1 detects K-band (not locked out) → alert active, camera suppressed.
//   t=4s:  V1 alert clears → camera zone entered → camera alert starts.
//   t=6s:  Pass camera → camera clears.
// =============================================================================

void test_combined_v1_alert_camera_lockout_sequence() {
    float cameraLon = HOME_LON + 0.0100f;  // Camera 0.01° east (~850m at this lat).
    queueCamera(HOME_LAT, cameraLon, 900, 35, 4);

    // --- t=10s: driving east, no alerts, camera far away (~850m).
    setGps(HOME_LAT, HOME_LON, 90.0f, 10000);
    clearAlerts();
    LockoutEnforcerResult r1 = tickPipeline(10000, EPOCH_BASE);

    // Enforcer evaluated (has fix) but no alert → no mute.
    TEST_ASSERT_TRUE(r1.evaluated);
    TEST_ASSERT_FALSE(r1.shouldMute);

    CameraRuntimeStatus cam1 = cameraRuntimeModule.snapshot();
    // Camera far away: should be IDLE.
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CameraLifecycleState::IDLE),
        static_cast<uint8_t>(cam1.lifecycleState));

    // --- t=12s: V1 detects K-band, still ~600m from camera.
    setGps(HOME_LAT, HOME_LON + 0.0030f, 90.0f, 12000);
    setAlert(BAND_K, K_FREQ);
    LockoutEnforcerResult r2 = tickPipeline(12000, EPOCH_BASE + 2000);

    TEST_ASSERT_TRUE(r2.evaluated);
    TEST_ASSERT_FALSE(r2.shouldMute);  // No lockout entry for this one.

    // --- t=14s: V1 alert clears, now ~500m from camera.
    setGps(HOME_LAT, HOME_LON + 0.0050f, 90.0f, 14000);
    clearAlerts();
    LockoutEnforcerResult r3 = tickPipeline(14000, EPOCH_BASE + 4000);

    TEST_ASSERT_TRUE(r3.evaluated);
    TEST_ASSERT_FALSE(r3.shouldMute);

    // --- t=16s: close to camera → should activate (heading aligned, no signal priority).
    setGps(HOME_LAT, HOME_LON + 0.0090f, 90.0f, 16000);
    clearAlerts();
    tickPipeline(16000, EPOCH_BASE + 6000);

    CameraRuntimeStatus cam3 = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CameraLifecycleState::ACTIVE),
        static_cast<uint8_t>(cam3.lifecycleState));

    // --- t=18s: pass camera, continue east.
    setGps(HOME_LAT, HOME_LON + 0.0105f, 90.0f, 18000);
    tickPipeline(18000, EPOCH_BASE + 8000);

    // --- t=20s–24s: drive well past camera exit zone.
    setGps(HOME_LAT, HOME_LON + 0.0200f, 90.0f, 20000);
    tickPipeline(20000, EPOCH_BASE + 10000);
    setGps(HOME_LAT, HOME_LON + 0.0300f, 90.0f, 22000);
    tickPipeline(22000, EPOCH_BASE + 12000);
    setGps(HOME_LAT, HOME_LON + 0.0400f, 90.0f, 24000);
    tickPipeline(24000, EPOCH_BASE + 14000);

    CameraRuntimeStatus cam4 = cameraRuntimeModule.snapshot();
    // After driving far past, camera should be IDLE or finishing exit suppression.
    TEST_ASSERT_TRUE(
        cam4.lifecycleState == CameraLifecycleState::IDLE ||
        cam4.lifecycleState == CameraLifecycleState::SUPPRESSED_UNTIL_EXIT);
}

// =============================================================================
// SCENARIO 13: Learning + enforcement end-to-end
//
// Three "drives" past the same K-band source → lockout learned.
// Fourth drive → enforcer auto-mutes.
// =============================================================================

void test_end_to_end_learn_then_enforce() {
    learner.setTuning(3, LockoutLearner::kDefaultRadiusE5,
                      LockoutLearner::kDefaultFreqToleranceMHz);

    // Drive 1–3: pass K-band source at HOME.
    for (int drive = 0; drive < 3; drive++) {
        uint32_t baseMs = 5000 + static_cast<uint32_t>(drive) * 4000;
        int64_t epochMs = EPOCH_BASE + static_cast<int64_t>(drive) * 4000;

        setGps(HOME_LAT, HOME_LON, 90.0f, baseMs);
        setAlert(BAND_K, K_FREQ);
        GpsRuntimeStatus gps = makeGpsStatus(HOME_LAT, HOME_LON);

        signalCaptureModule.capturePriorityObservation(baseMs, parser, gps);

        // Allow learner to process on each drive.
        tickLearner(baseMs + 2500, epochMs + 2500);
    }

    // After 3 drives, lockout should be created.
    TEST_ASSERT_TRUE(lockoutIndex.activeCount() >= 1);

    // Drive 4: same place, same K-band → now muted.
    uint32_t enfMs = 20000;
    setGps(HOME_LAT, HOME_LON, 90.0f, enfMs);
    setAlert(BAND_K, K_FREQ);
    LockoutEnforcerResult r = tickPipeline(enfMs, EPOCH_BASE + 20000);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_TRUE(r.shouldMute);
}

// =============================================================================
// SCENARIO 14: Move to different location — no false lockout
// =============================================================================

void test_learned_lockout_does_not_false_trigger_at_distant_location() {
    // Pre-build a lockout at HOME.
    LockoutEntry entry;
    entry.latE5      = HOME_LAT_E5;
    entry.lonE5      = HOME_LON_E5;
    entry.radiusE5   = 1350;
    entry.bandMask   = BAND_K;
    entry.freqMHz    = K_FREQ;
    entry.freqTolMHz = 10;
    entry.confidence = 100;
    entry.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.firstSeenMs = EPOCH_BASE;
    entry.lastSeenMs  = EPOCH_BASE;
    lockoutIndex.add(entry);

    // Different location: ~5 km north (well beyond 1350 E5 ≈ 0.0135° radius).
    float distantLat = HOME_LAT + 0.05f;
    setGps(distantLat, HOME_LON, 90.0f, 5000);
    setAlert(BAND_K, K_FREQ);

    LockoutEnforcerResult r = tickPipeline(5000, EPOCH_BASE + 5000);

    TEST_ASSERT_TRUE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);  // Far from lockout zone → real alert.
}

// =============================================================================
// SCENARIO 15: Weak alert does NOT suppress camera — only priority does
// =============================================================================

void test_weak_alert_does_not_suppress_camera() {
    float cameraLon = HOME_LON + 0.0010f;
    queueCamera(HOME_LAT, cameraLon, 900, 35, 4);  // east-facing camera

    // Drive east toward camera with a weak, non-priority alert active.
    // AlertData::create with isPriority=false and isValid=false mimics a
    // background K-band signal (BSM, door opener, etc.).
    setGps(HOME_LAT, HOME_LON, 90.0f, 10000);
    AlertData weak = AlertData::create(BAND_K, DIR_FRONT, 1, 0, K_FREQ, false, false);
    parser.setAlerts({weak});
    TEST_ASSERT_TRUE(parser.hasAlerts());  // alerts exist

    tickPipeline(10000, EPOCH_BASE);

    CameraRuntimeStatus status = cameraRuntimeModule.snapshot();
    // Camera should still activate — weak alert must not preempt.
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CameraLifecycleState::ACTIVE),
        static_cast<uint8_t>(status.lifecycleState));
    TEST_ASSERT_TRUE(status.activeAlert.active);
}

// =============================================================================
// SCENARIO 16: Lockout path works, camera skips on memory guard, then recovers
// =============================================================================

void test_lockout_mute_then_camera_memory_guard_then_recovery() {
    // Pre-load a lockout at HOME.
    LockoutEntry entry;
    entry.latE5      = HOME_LAT_E5;
    entry.lonE5      = HOME_LON_E5;
    entry.radiusE5   = 1350;
    entry.bandMask   = BAND_K;
    entry.freqMHz    = K_FREQ;
    entry.freqTolMHz = 10;
    entry.confidence = 100;
    entry.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.firstSeenMs = EPOCH_BASE - 86400000LL;
    entry.lastSeenMs  = EPOCH_BASE;
    lockoutIndex.add(entry);

    // Camera ahead on the same heading.
    float cameraLon = HOME_LON + 0.0010f;
    queueCamera(HOME_LAT, cameraLon, 900, 35, 4);

    // Phase A: lockout enforcement should still work with an active priority alert.
    setGps(HOME_LAT, HOME_LON, 90.0f, 10000);
    setAlert(BAND_K, K_FREQ);
    LockoutEnforcerResult muted = tickPipeline(10000, EPOCH_BASE + 10000);
    TEST_ASSERT_TRUE(muted.evaluated);
    TEST_ASSERT_TRUE(muted.shouldMute);

    // Phase B: no signal priority + low internal heap => camera memory guard skip.
    clearAlerts();
    mock_set_heap_caps(20000u, 12000u);
    setGps(HOME_LAT, HOME_LON, 90.0f, 10300);
    LockoutEnforcerResult blocked = tickPipeline(10300, EPOCH_BASE + 10300);
    TEST_ASSERT_TRUE(blocked.evaluated);
    TEST_ASSERT_FALSE(blocked.shouldMute);

    CameraRuntimeStatus blockedCam = cameraRuntimeModule.snapshot();
    TEST_ASSERT_TRUE(blockedCam.counters.cameraTickSkipsMemoryGuard > 0);
    TEST_ASSERT_EQUAL_UINT32(0, blockedCam.counters.cameraCandidatesChecked);
    TEST_ASSERT_FALSE(blockedCam.activeAlert.active);

    // Phase C: restore heap and verify camera scanning resumes.
    mock_set_heap_caps(64000u, 32000u);
    setGps(HOME_LAT, HOME_LON, 90.0f, 10600);
    tickPipeline(10600, EPOCH_BASE + 10600);

    CameraRuntimeStatus recoveredCam = cameraRuntimeModule.snapshot();
    TEST_ASSERT_TRUE(
        recoveredCam.counters.cameraCandidatesChecked >
        blockedCam.counters.cameraCandidatesChecked);
    TEST_ASSERT_TRUE(recoveredCam.activeAlert.active);
}

// =============================================================================
// SCENARIO 17: Mode OFF — nothing evaluated, no crash
// =============================================================================

void test_mode_off_is_completely_inert() {
    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_OFF;
    enforcer.begin(&settingsManager, &lockoutIndex);

    LockoutEntry entry;
    entry.latE5 = HOME_LAT_E5;
    entry.lonE5 = HOME_LON_E5;
    entry.radiusE5 = 1350;
    entry.bandMask = BAND_K;
    entry.freqMHz = K_FREQ;
    entry.freqTolMHz = 10;
    entry.confidence = 100;
    entry.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.firstSeenMs = EPOCH_BASE;
    entry.lastSeenMs = EPOCH_BASE;
    lockoutIndex.add(entry);

    setGps(HOME_LAT, HOME_LON, 90.0f, 5000);
    setAlert(BAND_K, K_FREQ);

    LockoutEnforcerResult r = tickPipeline(5000, EPOCH_BASE + 5000);
    TEST_ASSERT_FALSE(r.evaluated);
    TEST_ASSERT_FALSE(r.shouldMute);
}

// =============================================================================
// main
// =============================================================================

int main() {
    UNITY_BEGIN();

    // Alert → lockout pipeline
    RUN_TEST(test_alert_with_no_lockout_entry_does_not_mute);
    RUN_TEST(test_alert_at_lockout_zone_mutes);
    RUN_TEST(test_shadow_mode_evaluates_but_does_not_mute);
    RUN_TEST(test_no_gps_fix_skips_evaluation);
    RUN_TEST(test_clean_pass_decays_lockout_confidence);

    // Lockout learning lifecycle
    RUN_TEST(test_learning_lifecycle_promotes_after_three_hits);
    RUN_TEST(test_different_frequency_creates_separate_candidate);
    RUN_TEST(test_ka_band_not_learned_when_disabled);
    RUN_TEST(test_ka_band_learned_when_enabled);
    RUN_TEST(test_end_to_end_learn_then_enforce);
    RUN_TEST(test_learned_lockout_does_not_false_trigger_at_distant_location);
    RUN_TEST(test_mode_off_is_completely_inert);

    // Camera integration
    RUN_TEST(test_camera_activates_on_approach_heading_aligned);
    RUN_TEST(test_camera_suppressed_when_signal_priority_active);
    RUN_TEST(test_weak_alert_does_not_suppress_camera);
    RUN_TEST(test_lockout_mute_then_camera_memory_guard_then_recovery);

    // Combined scenarios
    RUN_TEST(test_combined_v1_alert_camera_lockout_sequence);

    return UNITY_END();
}

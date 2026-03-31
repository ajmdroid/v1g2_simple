// =============================================================================
// Captured-log replay integration tests
//
// Feeds committed replay fixtures through the real production pipeline and
// compares observed behaviour to expected.json assertions.
//
// Fixture layout (under test/fixtures/replay/<scenario>/):
//   meta.json      – scenario metadata and lane tag
//   packets.csv    – timestamp_ms,frame_hex
//   gps.csv        – timestamp_ms,lat,lon,speed_mph,course_deg,has_fix
//   expected.json  – assertions for alerts, mutes, lockout transitions, etc.
//
// The replay harness is invoked by scripts/run_replay_suite.py which selects
// the scenarios for the requested lane and runs this test binary.
//
// Environment: native-replay (see platformio.ini)
// =============================================================================

#include <unity.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

// Mocks first
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

// ── Real modules (same wiring pattern as test_drive_scenario) ───────
#include "../../src/modules/gps/gps_observation_log.cpp"
#include "../../src/modules/gps/gps_runtime_module.cpp"

#include "../../src/modules/lockout/signal_observation_log.h"
#include "../../src/modules/lockout/signal_observation_log.cpp"
#include "../../src/modules/lockout/signal_capture_module.h"
#include "../../src/modules/lockout/lockout_band_policy.cpp"
#include "../../src/modules/lockout/signal_capture_module.cpp"

static uint32_t sdEnqueueCount = 0;
SignalObservationSdLogger signalObservationSdLogger;
bool SignalObservationSdLogger::enqueue(const SignalObservation&) {
    sdEnqueueCount++;
    return true;
}

#include "../../src/modules/lockout/lockout_entry.h"
#include "../../src/modules/lockout/lockout_index.h"
#include "../../src/modules/lockout/lockout_index.cpp"
#include "../../src/modules/lockout/lockout_store.h"
#include "../../src/modules/lockout/lockout_store.cpp"
#include "../../src/modules/lockout/lockout_enforcer.h"
#include "../../src/modules/lockout/lockout_enforcer.cpp"
#include "../../src/modules/lockout/road_map_reader.h"
#include "../../src/modules/lockout/road_map_reader.cpp"
#include "../../src/modules/lockout/lockout_learner.h"
#include "../../src/modules/lockout/lockout_learner.cpp"

// ── Replay data structures ──────────────────────────────────────────

struct ReplayPacket {
    uint32_t timestamp_ms;
    std::vector<uint8_t> frame;
};

struct ReplayGpsSample {
    uint32_t timestamp_ms;
    float lat;
    float lon;
    float speed_mph;
    float course_deg;
    bool has_fix;
};

// ── Shared test state ───────────────────────────────────────────────
static PacketParser parser;
static LockoutEnforcer enforcer;
static LockoutLearner learner;

static std::vector<ReplayPacket> replayPackets;
static std::vector<ReplayGpsSample> replayGps;

// ── Harness helpers ─────────────────────────────────────────────────

static void setTime(uint32_t ms) {
    mockMillis = ms;
    mockMicros = static_cast<unsigned long>(ms) * 1000UL;
}

static void resetPipeline() {
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
    learner.begin(&lockoutIndex, &signalObservationLog, &lockoutStore);

    gpsRuntimeModule = GpsRuntimeModule();
    gpsRuntimeModule.begin(true, &gpsObservationLog);

    replayPackets.clear();
    replayGps.clear();
}

// ── Placeholder test (scaffolding for Phase 1) ──────────────────────
// The full CSV/JSON loading and assertion checking will be implemented
// when the first real captured fixtures are committed.

void test_replay_harness_compiles_and_resets(void) {
    resetPipeline();
    TEST_ASSERT_EQUAL(0, replayPackets.size());
    TEST_ASSERT_EQUAL(0, replayGps.size());
}

void test_replay_fixture_directory_exists(void) {
    // Verify the fixture root is accessible (relative to project root).
    // This test is a build canary — if the replay env compiles and this
    // passes, the harness wiring is intact.
    TEST_IGNORE_MESSAGE("Replay fixture loading not yet implemented");
}

// =============================================================================
void setUp() { resetPipeline(); }
void tearDown() {}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_replay_harness_compiles_and_resets);
    RUN_TEST(test_replay_fixture_directory_exists);
    return UNITY_END();
}

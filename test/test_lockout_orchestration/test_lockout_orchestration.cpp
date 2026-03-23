#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SettingsManager settingsManager;
#endif

#include <algorithm>

#include "../../src/modules/lockout/lockout_orchestration_module.h"
#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/lockout/lockout_index.h"
#include "../../src/modules/lockout/signal_capture_module.h"
#include "../../src/modules/speed/speed_source_selector.h"
#include "../../src/time_service.h"
#include "../../src/modules/lockout/lockout_runtime_mute_controller.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/lockout/lockout_pre_quiet_controller.cpp"
#include "../../src/modules/gps/gps_lockout_safety.cpp"

// ---------------------------------------------------------------------------
// Lightweight stubs for orchestration dependencies.
// We only implement methods directly touched by lockout_orchestration_module.
// ---------------------------------------------------------------------------

static LockoutEnforcerResult g_fakeEnforcerResult;
static size_t g_fakeNearbyCount = 0;
static uint32_t g_captureCalls = 0;
static int64_t g_fakeEpochMs = 0;
static int64_t g_lastEnforcerEpochMs = -1;
static int32_t g_lastEnforcerTzOffsetMinutes = -1;

void LockoutEnforcer::begin(const SettingsManager* settings,
                            LockoutIndex* index,
                            LockoutStore* store) {
    settings_ = settings;
    index_ = index;
    store_ = store;
    lastResult_ = LockoutEnforcerResult{};
    stats_ = Stats{};
}

LockoutEnforcerResult LockoutEnforcer::process(uint32_t nowMs,
                                               int64_t epochMs,
                                               int32_t tzOffsetMinutes,
                                               const PacketParser& parser,
                                               const GpsRuntimeStatus& gpsStatus) {
    (void)nowMs;
    (void)epochMs;
    (void)tzOffsetMinutes;
    (void)parser;
    (void)gpsStatus;
    g_lastEnforcerEpochMs = epochMs;
    g_lastEnforcerTzOffsetMinutes = tzOffsetMinutes;
    lastResult_ = g_fakeEnforcerResult;
    return lastResult_;
}

size_t LockoutIndex::findNearby(int32_t latE5,
                                int32_t lonE5,
                                int16_t* out,
                                size_t outCap) const {
    (void)latE5;
    (void)lonE5;
    if (!out || outCap == 0) return 0;
    const size_t count = std::min(g_fakeNearbyCount, outCap);
    for (size_t i = 0; i < count; ++i) {
        out[i] = static_cast<int16_t>(i);
    }
    return count;
}

size_t LockoutIndex::findNearbyInflated(int32_t latE5,
                                        int32_t lonE5,
                                        uint16_t bufferE5,
                                        int16_t* out,
                                        size_t outCap) const {
    (void)bufferE5;
    return findNearby(latE5, lonE5, out, outCap);
}

size_t LockoutIndex::findNearbyDirectional(int32_t latE5,
                                           int32_t lonE5,
                                           bool courseValid,
                                           float courseDeg,
                                           uint16_t bufferE5,
                                           int16_t* out,
                                           size_t outCap) const {
    (void)courseValid;
    (void)courseDeg;
    (void)bufferE5;
    return findNearby(latE5, lonE5, out, outCap);
}

void SignalCaptureModule::reset() {
    for (size_t i = 0; i < kRecentBucketCount; ++i) {
        recentBuckets_[i].valid = false;
    }
    nextRecentBucketIndex_ = 0;
}

void SignalCaptureModule::capturePriorityObservation(uint32_t nowMs,
                                                     const PacketParser& parser,
                                                     const GpsRuntimeStatus& gpsStatus,
                                                     const SpeedSelection& selectedSpeed,
                                                     bool captureUnsupportedBandsToSd) {
    (void)nowMs;
    (void)parser;
    (void)gpsStatus;
    (void)selectedSpeed;
    (void)captureUnsupportedBandsToSd;
    g_captureCalls++;
}

int64_t TimeService::nowEpochMsOr0() const {
    return g_fakeEpochMs;
}

#include "../../src/modules/lockout/lockout_orchestration_module.cpp"

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static LockoutOrchestrationModule module;
static V1BLEClient ble;
static PacketParser parser;
static SettingsManager settings;
static V1Display display;
static LockoutEnforcer enforcer;
static LockoutIndex lockoutIndexInst;
static SignalCaptureModule sigCapture;
static SystemEventBus eventBus;
static PerfCounters perfCounterState;
static TimeService timeSvc;
static QuietCoordinatorModule quiet;
static GpsRuntimeStatus gps;
SpeedSourceSelector speedSourceSelector;

static void setOverrideCondition(bool active) {
    if (active) {
        parser.state.activeBands = static_cast<uint8_t>(BAND_KA);
        parser.state.muted = true;
        return;
    }
    parser.state.activeBands = static_cast<uint8_t>(BAND_NONE);
    parser.state.muted = false;
}

static LockoutOrchestrationResult runOnce(uint32_t nowMs, bool proxyConnected = false) {
    mockMillis = nowMs;
    return module.process(nowMs, gps, proxyConnected, false);
}

void setUp() {
    ble.reset();
    display.reset();
    parser.reset();
    settings = SettingsManager{};
    eventBus.reset();
    perfCounterState.reset();
    module.reset();

    settings.settings.gpsLockoutMode = LOCKOUT_RUNTIME_OFF;
    settings.settings.gpsLockoutPreQuiet = false;
    settings.settings.gpsLockoutCoreGuardEnabled = false;

    g_fakeEnforcerResult = LockoutEnforcerResult{};
    g_fakeNearbyCount = 0;
    g_captureCalls = 0;
    g_fakeEpochMs = 0;
    g_lastEnforcerEpochMs = -1;
    g_lastEnforcerTzOffsetMinutes = -1;
    quiet.begin(&ble, &parser);

    gps = GpsRuntimeStatus{};
    gps.locationValid = false;

    module.begin(&ble,
                 &parser,
                 &settings,
                 &display,
                 &enforcer,
                 &lockoutIndexInst,
                 &sigCapture,
                 &eventBus,
                 &perfCounterState,
                 &timeSvc,
                 &quiet);
}

void tearDown() {}

void test_override_unmute_sends_on_first_eligible_frame() {
    setOverrideCondition(true);

    runOnce(1000);

    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
}

void test_override_unmute_retries_every_400ms() {
    setOverrideCondition(true);

    runOnce(1000);
    runOnce(1399);  // Still within retry window.
    runOnce(1400);  // Retry window elapsed.

    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
}

void test_override_unmute_caps_at_15_attempts() {
    setOverrideCondition(true);

    for (uint32_t i = 0; i < 25; ++i) {
        runOnce(1000 + (i * 400));
    }
    runOnce(20000);

    TEST_ASSERT_EQUAL_INT(15, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
}

void test_override_unmute_resets_and_rearms_after_condition_clears() {
    setOverrideCondition(true);

    runOnce(1000);
    runOnce(1400);
    runOnce(1800);
    TEST_ASSERT_EQUAL_INT(3, ble.setMuteCalls);

    setOverrideCondition(false);
    runOnce(1900);

    setOverrideCondition(true);
    runOnce(1950);  // Re-arm should send immediately after reset.

    TEST_ASSERT_EQUAL_INT(4, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
}

void test_proxy_connected_path_resets_retry_state() {
    setOverrideCondition(true);

    runOnce(1000, false);
    runOnce(1100, true);   // Proxy path should call reset().
    runOnce(1200, false);  // Would be blocked by cadence if reset did not occur.

    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_FALSE(ble.lastMuteValue);
}

void test_null_time_service_falls_back_to_zero_epoch_and_offset() {
    module.begin(&ble,
                 &parser,
                 &settings,
                 &display,
                 &enforcer,
                 &lockoutIndexInst,
                 &sigCapture,
                 &eventBus,
                 &perfCounterState,
                 nullptr,
                 &quiet);

    runOnce(1000);

    TEST_ASSERT_EQUAL_INT64(0, g_lastEnforcerEpochMs);
    TEST_ASSERT_EQUAL_INT32(0, g_lastEnforcerTzOffsetMinutes);
    TEST_ASSERT_EQUAL_UINT32(1, g_captureCalls);
}

void test_pre_quiet_returns_volume_command_without_sending_ble_volume() {
    settings.settings.gpsLockoutMode = LOCKOUT_RUNTIME_ENFORCE;
    settings.settings.gpsLockoutPreQuiet = true;
    parser.state.mainVolume = 6;
    parser.state.muteVolume = 2;
    g_fakeEnforcerResult.mode = LOCKOUT_RUNTIME_ENFORCE;
    gps.locationValid = true;
    gps.latitudeDeg = 40.7128f;
    gps.longitudeDeg = -74.0060f;
    g_fakeNearbyCount = 1;

    const LockoutOrchestrationResult first = runOnce(1000);
    const LockoutOrchestrationResult second = runOnce(1100);

    TEST_ASSERT_FALSE(first.volumeCommand.hasAction());
    TEST_ASSERT_TRUE(second.volumeCommand.hasAction());
    TEST_ASSERT_EQUAL(LockoutVolumeCommandType::PreQuietDrop, second.volumeCommand.type);
    TEST_ASSERT_EQUAL_UINT8(2, second.volumeCommand.volume);
    TEST_ASSERT_EQUAL_UINT8(2, second.volumeCommand.muteVolume);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_override_unmute_sends_on_first_eligible_frame);
    RUN_TEST(test_override_unmute_retries_every_400ms);
    RUN_TEST(test_override_unmute_caps_at_15_attempts);
    RUN_TEST(test_override_unmute_resets_and_rearms_after_condition_clears);
    RUN_TEST(test_proxy_connected_path_resets_retry_state);
    RUN_TEST(test_null_time_service_falls_back_to_zero_epoch_and_offset);
    RUN_TEST(test_pre_quiet_returns_volume_command_without_sending_ble_volume);
    return UNITY_END();
}

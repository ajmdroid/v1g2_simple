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
#include "../../src/modules/volume_fade/volume_fade_module.h"
#include "../../src/time_service.h"
#include "../../src/modules/lockout/lockout_runtime_mute_controller.cpp"
#include "../../src/modules/lockout/lockout_pre_quiet_controller.cpp"
#include "../../src/modules/gps/gps_lockout_safety.cpp"

// ---------------------------------------------------------------------------
// Lightweight stubs for orchestration dependencies.
// We only implement methods directly touched by lockout_orchestration_module.
// ---------------------------------------------------------------------------

static LockoutEnforcerResult g_fakeEnforcerResult;
static size_t g_fakeNearbyCount = 0;
static uint32_t g_captureCalls = 0;
static uint32_t g_baselineHintCalls = 0;
static int64_t g_fakeEpochMs = 0;

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
                                               const PacketParser& parser,
                                               const GpsRuntimeStatus& gpsStatus) {
    (void)nowMs;
    (void)epochMs;
    (void)parser;
    (void)gpsStatus;
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

void SignalCaptureModule::reset() {
    lastValid_ = false;
    lastSample_ = SignalObservation{};
}

void SignalCaptureModule::capturePriorityObservation(uint32_t nowMs,
                                                     const PacketParser& parser,
                                                     const GpsRuntimeStatus& gpsStatus,
                                                     bool captureUnsupportedBandsToSd) {
    (void)nowMs;
    (void)parser;
    (void)gpsStatus;
    (void)captureUnsupportedBandsToSd;
    g_captureCalls++;
}

VolumeFadeModule::VolumeFadeModule() {}

void VolumeFadeModule::begin(SettingsManager* settings) {
    this->settings = settings;
}

VolumeFadeAction VolumeFadeModule::process(const VolumeFadeContext& ctx) {
    (void)ctx;
    return VolumeFadeAction{};
}

void VolumeFadeModule::reset() {}

void VolumeFadeModule::setBaselineHint(uint8_t mainVol, uint8_t muteVol, uint32_t nowMs) {
    (void)mainVol;
    (void)muteVol;
    (void)nowMs;
    g_baselineHintCalls++;
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
static VolumeFadeModule volFade;
static SystemEventBus eventBus;
static PerfCounters perfCounterState;
static TimeService timeSvc;
static GpsRuntimeStatus gps;

static void setOverrideCondition(bool active) {
    if (active) {
        parser.state.activeBands = static_cast<uint8_t>(BAND_KA);
        parser.state.muted = true;
        return;
    }
    parser.state.activeBands = static_cast<uint8_t>(BAND_NONE);
    parser.state.muted = false;
}

static void runOnce(uint32_t nowMs, bool proxyConnected = false) {
    mockMillis = nowMs;
    module.process(nowMs, gps, proxyConnected, false);
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
    g_baselineHintCalls = 0;
    g_fakeEpochMs = 0;

    gps = GpsRuntimeStatus{};
    gps.locationValid = false;

    module.begin(&ble,
                 &parser,
                 &settings,
                 &display,
                 &enforcer,
                 &lockoutIndexInst,
                 &sigCapture,
                 &volFade,
                 &eventBus,
                 &perfCounterState,
                 &timeSvc);
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_override_unmute_sends_on_first_eligible_frame);
    RUN_TEST(test_override_unmute_retries_every_400ms);
    RUN_TEST(test_override_unmute_caps_at_15_attempts);
    RUN_TEST(test_override_unmute_resets_and_rearms_after_condition_clears);
    RUN_TEST(test_proxy_connected_path_resets_retry_state);
    return UNITY_END();
}

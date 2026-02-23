#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/display.h"
#include "../mocks/ble_client.h"
#include "../mocks/modules/ble/ble_queue_module.h"
#include "../mocks/modules/display/display_preview_module.h"
#include "../mocks/modules/display/display_restore_module.h"
#include "../mocks/packet_parser.h"
#include "../mocks/settings.h"
#include "../mocks/modules/gps/gps_runtime_module.h"
#include "../mocks/obd_handler.h"
#include "../mocks/modules/lockout/lockout_orchestration_module.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/display/display_orchestration_module.cpp"

static V1Display display;
static V1BLEClient ble;
static BleQueueModule bleQueue;
static DisplayPreviewModule preview;
static DisplayRestoreModule restore;
static PacketParser parser;
static GpsRuntimeModule gpsRuntime;
static OBDHandler obdHandler;
static LockoutOrchestrationModule lockout;
static DisplayOrchestrationModule module;

static void beginModule() {
    module.begin(&display,
                 &ble,
                 &bleQueue,
                 &preview,
                 &restore,
                 &parser,
                 &settingsManager,
                 &gpsRuntime,
                 &obdHandler,
                 &lockout);
}

void setUp() {
    display.reset();
    ble.reset();
    bleQueue.reset();
    preview = DisplayPreviewModule{};
    restore = DisplayRestoreModule{};
    parser.reset();
    gpsRuntime = GpsRuntimeModule{};
    obdHandler = OBDHandler{};
    lockout = LockoutOrchestrationModule{};
    settingsManager = SettingsManager{};
    beginModule();
}

void tearDown() {}

void test_process_early_updates_ble_context_and_proxy_status() {
    DisplayOrchestrationEarlyContext ctx;
    ctx.nowMs = 1200;
    ctx.bootSplashHoldActive = false;
    ctx.overloadThisLoop = false;
    ctx.bleContext = {true, true, -55, -66};
    ctx.bleReceiving = true;

    module.processEarly(ctx);

    TEST_ASSERT_EQUAL(1, display.setBleContextCalls);
    TEST_ASSERT_TRUE(display.lastBleContext.v1Connected);
    TEST_ASSERT_TRUE(display.lastBleContext.proxyConnected);
    TEST_ASSERT_EQUAL(-55, display.lastBleContext.v1Rssi);
    TEST_ASSERT_EQUAL(-66, display.lastBleContext.proxyRssi);
    TEST_ASSERT_EQUAL(1, display.setBLEProxyStatusCalls);
    TEST_ASSERT_TRUE(display.lastBleProxyEnabled);
    TEST_ASSERT_TRUE(display.lastBleProxyConnected);
    TEST_ASSERT_TRUE(display.lastBleReceiving);
}

void test_process_early_updates_preview_or_restore_path() {
    DisplayOrchestrationEarlyContext ctx;
    ctx.bootSplashHoldActive = false;
    ctx.overloadThisLoop = false;
    ctx.bleContext = {false, false, 0, 0};

    preview.setRunning(true);
    module.processEarly(ctx);
    TEST_ASSERT_EQUAL(1, preview.updateCalls);
    TEST_ASSERT_EQUAL(0, restore.processCalls);

    preview.setRunning(false);
    module.processEarly(ctx);
    TEST_ASSERT_EQUAL(1, restore.processCalls);
}

void test_parsed_frame_sets_status_indicators_and_requests_pipeline() {
    ble.setConnected(true);
    ble.setProxyConnected(false);
    settingsManager.settings.obdEnabled = true;
    gpsRuntime.nextSnapshot.enabled = true;
    gpsRuntime.nextSnapshot.hasFix = true;
    gpsRuntime.nextSnapshot.stableHasFix = true;
    gpsRuntime.nextSnapshot.satellites = 9;
    gpsRuntime.nextSnapshot.stableSatellites = 9;
    obdHandler.setConnected(true);
    obdHandler.setValidData(true);
    lockout.nextResult.prioritySuppressed = true;

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 5000;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = false;
    ctx.enableSignalTraceLogging = true;

    const auto result = module.processParsedFrame(ctx);

    TEST_ASSERT_TRUE(result.lockoutEvaluated);
    TEST_ASSERT_TRUE(result.lockoutPrioritySuppressed);
    TEST_ASSERT_TRUE(result.runDisplayPipeline);
    TEST_ASSERT_EQUAL(1, lockout.processCalls);
    TEST_ASSERT_EQUAL(1, display.setGpsSatellitesCalls);
    TEST_ASSERT_EQUAL(1, display.setObdConnectedCalls);
    TEST_ASSERT_TRUE(display.lastGpsEnabled);
    TEST_ASSERT_TRUE(display.lastGpsHasFix);
    TEST_ASSERT_EQUAL(9, display.lastGpsSatellites);
    TEST_ASSERT_TRUE(display.lastObdEnabled);
    TEST_ASSERT_TRUE(display.lastObdConnected);
    TEST_ASSERT_TRUE(display.lastObdHasData);
}

void test_parsed_frame_skips_pipeline_when_preview_running() {
    preview.setRunning(true);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 7000;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = false;

    const auto result = module.processParsedFrame(ctx);
    TEST_ASSERT_TRUE(result.lockoutEvaluated);
    TEST_ASSERT_FALSE(result.runDisplayPipeline);
}

void test_stale_lockout_badge_is_cleared_when_disconnected() {
    ble.setConnected(false);
    bleQueue.setLastParsedTimestamp(1000);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 5000;
    ctx.parsedReady = false;
    ctx.bootSplashHoldActive = false;

    module.processParsedFrame(ctx);

    TEST_ASSERT_EQUAL(1, display.setLockoutIndicatorCalls);
    TEST_ASSERT_FALSE(display.lastLockoutIndicatorValue);
}

void test_lightweight_refresh_updates_frequency_and_cards() {
    ble.setConnected(true);
    preview.setRunning(false);

    parser.state.muted = false;
    parser.state.bogeyCounterChar = '1';
    parser.state.hasPhotoAlert = false;
    parser.setAlerts({
        AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 35500, true, true)
    });

    DisplayOrchestrationRefreshContext ctx;
    ctx.nowMs = 500;
    ctx.bootSplashHoldActive = false;
    ctx.overloadLateThisLoop = true;
    ctx.pipelineRanThisLoop = false;

    const auto result = module.processLightweightRefresh(ctx);

    TEST_ASSERT_TRUE(result.signalPriorityActive);
    TEST_ASSERT_EQUAL(1, display.refreshFrequencyOnlyCalls);
    TEST_ASSERT_EQUAL(35500u, display.lastFrequencyMHz);
    TEST_ASSERT_EQUAL(1, display.refreshSecondaryAlertCardsCalls);
    TEST_ASSERT_EQUAL(1, display.lastSecondaryAlertCount);
}

void test_lightweight_refresh_falls_back_from_invalid_priority() {
    ble.setConnected(true);
    preview.setRunning(false);

    parser.state.muted = false;
    parser.state.hasPhotoAlert = false;
    parser.state.bogeyCounterChar = '3';
    parser.hasAlertsFlag = true;
    parser.priorityAlert = AlertData::create(BAND_NONE, DIR_NONE, 0, 0, 0, true, false);
    parser.alerts = {
        AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24150, true, false),
        AlertData::create(BAND_KA, DIR_REAR, 5, 0, 33820, true, false)
    };

    DisplayOrchestrationRefreshContext ctx;
    ctx.nowMs = 500;
    ctx.bootSplashHoldActive = false;
    ctx.overloadLateThisLoop = true;
    ctx.pipelineRanThisLoop = false;

    const auto result = module.processLightweightRefresh(ctx);

    TEST_ASSERT_TRUE(result.signalPriorityActive);
    TEST_ASSERT_EQUAL(1, display.refreshFrequencyOnlyCalls);
    TEST_ASSERT_EQUAL(24150u, display.lastFrequencyMHz);
    TEST_ASSERT_EQUAL(BAND_K, display.lastFrequencyBand);
}

void test_pipeline_draw_resets_frequency_timer_for_same_tick() {
    ble.setConnected(true);
    parser.setAlerts({
        AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24150, true, true)
    });

    DisplayOrchestrationRefreshContext ctx;
    ctx.nowMs = 1000;
    ctx.bootSplashHoldActive = false;
    ctx.overloadLateThisLoop = false;
    ctx.pipelineRanThisLoop = true;

    module.processLightweightRefresh(ctx);
    TEST_ASSERT_EQUAL(0, display.refreshFrequencyOnlyCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_early_updates_ble_context_and_proxy_status);
    RUN_TEST(test_process_early_updates_preview_or_restore_path);
    RUN_TEST(test_parsed_frame_sets_status_indicators_and_requests_pipeline);
    RUN_TEST(test_parsed_frame_skips_pipeline_when_preview_running);
    RUN_TEST(test_stale_lockout_badge_is_cleared_when_disconnected);
    RUN_TEST(test_lightweight_refresh_updates_frequency_and_cards);
    RUN_TEST(test_lightweight_refresh_falls_back_from_invalid_priority);
    RUN_TEST(test_pipeline_draw_resets_frequency_timer_for_same_tick);
    return UNITY_END();
}

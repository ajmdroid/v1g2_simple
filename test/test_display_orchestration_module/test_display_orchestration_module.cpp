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
#include "../mocks/modules/lockout/lockout_orchestration_module.h"
#include "../mocks/modules/volume_fade/volume_fade_module.h"
#include "../mocks/modules/speed_mute/speed_mute_module.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/lockout/lockout_runtime_mute_controller.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/display/display_orchestration_module.cpp"

static V1Display display;
static V1BLEClient ble;
static BleQueueModule bleQueue;
static DisplayPreviewModule preview;
static DisplayRestoreModule restore;
static PacketParser parser;
static GpsRuntimeModule gpsRuntime;
static LockoutOrchestrationModule lockout;
static VolumeFadeModule volumeFade;
static SpeedMuteModule speedMute;
static QuietCoordinatorModule quiet;
static DisplayOrchestrationModule module;

static void beginModule() {
    quiet.begin(&ble, &parser);
    module.begin(&display,
                 &ble,
                 &bleQueue,
                 &preview,
                 &restore,
                 &parser,
                 &settingsManager,
                 &gpsRuntime,
                 &lockout,
                 &volumeFade,
                 &speedMute,
                 &quiet);
}

void setUp() {
    display.reset();
    ble.reset();
    bleQueue.reset();
    preview = DisplayPreviewModule{};
    restore = DisplayRestoreModule{};
    parser.reset();
    gpsRuntime = GpsRuntimeModule{};
    lockout = LockoutOrchestrationModule{};
    volumeFade = VolumeFadeModule{};
    speedMute = SpeedMuteModule{};
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
    gpsRuntime.nextSnapshot.enabled = true;
    gpsRuntime.nextSnapshot.hasFix = true;
    gpsRuntime.nextSnapshot.stableHasFix = true;
    gpsRuntime.nextSnapshot.satellites = 9;
    gpsRuntime.nextSnapshot.stableSatellites = 9;
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
    TEST_ASSERT_TRUE(display.lastGpsEnabled);
    TEST_ASSERT_TRUE(display.lastGpsHasFix);
    TEST_ASSERT_EQUAL(9, display.lastGpsSatellites);
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
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

void test_parsed_frame_syncs_prequiet_presentation_from_coordinator() {
    quiet.setPreQuietActive(true);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 7050;
    ctx.parsedReady = false;
    ctx.bootSplashHoldActive = false;

    module.processParsedFrame(ctx);

    TEST_ASSERT_EQUAL(1, display.setPreQuietActiveCalls);
    TEST_ASSERT_TRUE(display.lastPreQuietActiveValue);
}

void test_parsed_frame_syncs_speedvol_zero_presentation_from_coordinator() {
    parser.state.mainVolume = 6;
    parser.state.muteVolume = 2;
    speedMute.begin(true, 25, 3, 0);
    speedMute.state_.muteActive = true;

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 7800;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = false;

    module.processParsedFrame(ctx);

    TEST_ASSERT_EQUAL(1, display.setSpeedVolZeroActiveCalls);
    TEST_ASSERT_TRUE(display.lastSpeedVolZeroActiveValue);
}

void test_parsed_frame_executes_lockout_restore_through_single_volume_owner() {
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::PreQuietRestore;
    lockout.nextResult.volumeCommand.volume = 7;
    lockout.nextResult.volumeCommand.muteVolume = 2;

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 8100;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = false;

    module.processParsedFrame(ctx);

    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL(1, volumeFade.setBaselineHintCalls);
    TEST_ASSERT_EQUAL_UINT8(7, volumeFade.lastHintVolume);
    TEST_ASSERT_EQUAL_UINT8(2, volumeFade.lastHintMuteVolume);
    TEST_ASSERT_EQUAL_UINT32(8100, volumeFade.lastHintNowMs);
}

void test_parsed_frame_executes_volume_fade_when_pipeline_runs() {
    parser.state.mainVolume = 8;
    parser.state.muteVolume = 2;
    parser.state.muted = false;
    parser.setAlerts({
        AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 35500, true, true)
    });
    volumeFade.nextAction.type = VolumeFadeAction::Type::FADE_DOWN;
    volumeFade.nextAction.targetVolume = 3;
    volumeFade.nextAction.targetMuteVolume = 1;

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 5200;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = false;

    module.processParsedFrame(ctx);

    TEST_ASSERT_EQUAL(1, volumeFade.processCalls);
    TEST_ASSERT_TRUE(volumeFade.lastContext.hasAlert);
    TEST_ASSERT_EQUAL_UINT8(8, volumeFade.lastContext.currentVolume);
    TEST_ASSERT_EQUAL_UINT8(2, volumeFade.lastContext.currentMuteVolume);
    TEST_ASSERT_EQUAL_UINT16(35500u, volumeFade.lastContext.currentFrequency);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(3, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastMuteVolume);
}

void test_parsed_frame_skips_volume_fade_when_lockout_volume_command_owns_frame() {
    parser.state.mainVolume = 8;
    parser.state.muteVolume = 2;
    parser.state.muted = false;
    parser.setAlerts({
        AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 35500, true, true)
    });
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::PreQuietDrop;
    lockout.nextResult.volumeCommand.volume = 2;
    lockout.nextResult.volumeCommand.muteVolume = 0;
    volumeFade.nextAction.type = VolumeFadeAction::Type::FADE_DOWN;
    volumeFade.nextAction.targetVolume = 3;
    volumeFade.nextAction.targetMuteVolume = 1;

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 5400;
    ctx.parsedReady = true;
    ctx.bootSplashHoldActive = false;

    module.processParsedFrame(ctx);

    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
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

void test_lightweight_refresh_clears_frequency_when_idle() {
    ble.setConnected(true);
    preview.setRunning(false);
    parser.reset();

    DisplayOrchestrationRefreshContext ctx;
    ctx.nowMs = 500;
    ctx.bootSplashHoldActive = false;
    ctx.overloadLateThisLoop = false;
    ctx.pipelineRanThisLoop = false;

    const auto result = module.processLightweightRefresh(ctx);

    TEST_ASSERT_FALSE(result.signalPriorityActive);
    TEST_ASSERT_EQUAL(1, display.refreshFrequencyOnlyCalls);
    TEST_ASSERT_EQUAL(0u, display.lastFrequencyMHz);
    TEST_ASSERT_EQUAL(BAND_NONE, display.lastFrequencyBand);
}

void test_lightweight_refresh_does_not_touch_frequency_while_preview_running() {
    ble.setConnected(true);
    preview.setRunning(true);
    parser.setAlerts({
        AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 35500, true, true)
    });

    DisplayOrchestrationRefreshContext ctx;
    ctx.nowMs = 500;
    ctx.bootSplashHoldActive = false;
    ctx.overloadLateThisLoop = false;
    ctx.pipelineRanThisLoop = false;

    module.processLightweightRefresh(ctx);

    TEST_ASSERT_EQUAL(0, display.refreshFrequencyOnlyCalls);
    TEST_ASSERT_EQUAL(0, display.refreshSecondaryAlertCardsCalls);
}

// --- Pre-quiet restore retry tests ---

void test_prequiet_restore_retries_until_v1_confirms() {
    // Issue a pre-quiet restore with target volume = 7
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::PreQuietRestore;
    lockout.nextResult.volumeCommand.volume = 7;
    lockout.nextResult.volumeCommand.muteVolume = 2;
    // V1 still reports old volume (5)
    parser.setMainVolume(5);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;

    module.processParsedFrame(ctx);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);  // Initial send

    // Next frame after retry interval — should retry since volume != 7
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::None;
    ctx.nowMs = 1000 + 80;  // past 75ms retry interval
    ble.setVolumeCalls = 0;

    module.processParsedFrame(ctx);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);  // Retry fired
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    // Volume fade should be suppressed during pending restore
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

void test_prequiet_restore_confirmed_when_v1_volume_matches() {
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::PreQuietRestore;
    lockout.nextResult.volumeCommand.volume = 7;
    lockout.nextResult.volumeCommand.muteVolume = 2;
    parser.setMainVolume(5);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 2000;
    ctx.parsedReady = true;

    module.processParsedFrame(ctx);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);

    // V1 echoes back the correct volume
    parser.setMainVolume(7);
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::None;
    ctx.nowMs = 2100;
    ble.setVolumeCalls = 0;
    volumeFade.processCalls = 0;

    module.processParsedFrame(ctx);
    // No retry needed — confirmed
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    // Volume fade should run again now that pending is cleared
    TEST_ASSERT_EQUAL(1, volumeFade.processCalls);
}

void test_prequiet_restore_timeout_clears_pending() {
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::PreQuietRestore;
    lockout.nextResult.volumeCommand.volume = 7;
    lockout.nextResult.volumeCommand.muteVolume = 2;
    parser.setMainVolume(3);  // V1 never confirms

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 3000;
    ctx.parsedReady = true;

    module.processParsedFrame(ctx);

    // Advance past timeout (2000ms)
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::None;
    ctx.nowMs = 3000 + 2001;
    ble.setVolumeCalls = 0;
    volumeFade.processCalls = 0;

    module.processParsedFrame(ctx);
    // Timed out — no retry, fade runs again
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL(1, volumeFade.processCalls);
}

void test_prequiet_drop_clears_pending_restore() {
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::PreQuietRestore;
    lockout.nextResult.volumeCommand.volume = 7;
    lockout.nextResult.volumeCommand.muteVolume = 2;
    parser.setMainVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 4000;
    ctx.parsedReady = true;

    module.processParsedFrame(ctx);  // Arms pending restore

    // New drop command should clear the pending restore
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::PreQuietDrop;
    lockout.nextResult.volumeCommand.volume = 1;
    lockout.nextResult.volumeCommand.muteVolume = 1;
    ctx.nowMs = 4050;
    ble.setVolumeCalls = 0;

    module.processParsedFrame(ctx);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);

    // Next frame: no pending restore, fade should run
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::None;
    ctx.nowMs = 4200;
    ble.setVolumeCalls = 0;
    volumeFade.processCalls = 0;

    module.processParsedFrame(ctx);
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);  // No retry
    TEST_ASSERT_EQUAL(1, volumeFade.processCalls);  // Fade runs
}

// --- Speed volume tests ---

// Helper to enable speed volume and make it active (muteActive = true).
static void enableSpeedVolume(uint8_t targetVol, uint8_t originalVol = 7,
                              uint8_t originalMuteVol = 2) {
    speedMute.settings_.v1Volume = targetVol;
    speedMute.settings_.enabled = true;
    speedMute.state_.muteActive = true;
    parser.setMainVolume(originalVol);
    parser.setMuteVolume(originalMuteVol);
}

void test_speed_vol_drops_volume_when_mute_active() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;

    module.processParsedFrame(ctx);

    // Should have sent setVolume with target 3, mute vol unchanged (2).
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(3, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    // Volume hint should be set on lockout with original volume.
    TEST_ASSERT_EQUAL(1, lockout.setVolumeHintCalls);
    TEST_ASSERT_EQUAL_UINT8(7, lockout.volumeHintMain);
    TEST_ASSERT_EQUAL_UINT8(2, lockout.volumeHintMute);
    // Volume fade should be gated.
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

void test_speed_vol_confirmed_by_v1_holds_steady() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate

    // V1 confirms the target volume.
    parser.setMainVolume(3);
    ble.setVolumeCalls = 0;
    ctx.nowMs = 1100;

    module.processParsedFrame(ctx);
    // No retry needed — confirmed.
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    // Still busy — fade still gated.
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

void test_speed_vol_retries_until_v1_confirms_drop() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate

    // V1 hasn't confirmed (still at 7).
    ble.setVolumeCalls = 0;
    ctx.nowMs = 1000 + 80;  // Past retry interval (75ms).

    module.processParsedFrame(ctx);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(3, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_speed_vol_restores_when_mute_deactivates() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate

    // Speed rises above threshold — mute deactivates.
    speedMute.state_.muteActive = false;
    parser.setMainVolume(3);  // V1 still at lowered volume.
    ble.setVolumeCalls = 0;
    volumeFade.setBaselineHintCalls = 0;
    lockout.clearVolumeHintCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Should restore to original volume.
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    // Baseline hint injected into volume fade.
    TEST_ASSERT_EQUAL(1, volumeFade.setBaselineHintCalls);
    TEST_ASSERT_EQUAL_UINT8(7, volumeFade.lastHintVolume);
    TEST_ASSERT_EQUAL_UINT8(2, volumeFade.lastHintMuteVolume);
    // Volume hint cleared on lockout.
    TEST_ASSERT_EQUAL(1, lockout.clearVolumeHintCalls);
}

void test_speed_vol_restore_retries_until_confirmed() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate

    // Deactivate.
    speedMute.state_.muteActive = false;
    parser.setMainVolume(3);
    ble.setVolumeCalls = 0;
    ctx.nowMs = 2000;
    module.processParsedFrame(ctx);  // Restore issued

    // V1 still hasn't confirmed (still at 3).
    ble.setVolumeCalls = 0;
    ctx.nowMs = 2000 + 80;  // Past retry interval.

    module.processParsedFrame(ctx);
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);

    // Confirm arrives.
    parser.setMainVolume(7);
    ble.setVolumeCalls = 0;
    volumeFade.processCalls = 0;
    ctx.nowMs = 2200;

    module.processParsedFrame(ctx);
    // No retry — confirmed. Fade should run again.
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL(1, volumeFade.processCalls);
}

void test_speed_vol_restore_times_out() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate

    // Deactivate.
    speedMute.state_.muteActive = false;
    parser.setMainVolume(3);
    ctx.nowMs = 2000;
    module.processParsedFrame(ctx);  // Restore issued

    // V1 never confirms — advance past timeout (2000ms).
    ble.setVolumeCalls = 0;
    volumeFade.processCalls = 0;
    ctx.nowMs = 2000 + 2001;

    module.processParsedFrame(ctx);
    // Timed out — no retry. Fade runs again.
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL(1, volumeFade.processCalls);
}

void test_speed_vol_defers_when_prequiet_active() {
    // Pre-quiet is currently in DROPPED phase.
    lockout.mockPreQuietActive = true;
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;

    module.processParsedFrame(ctx);

    // Speed volume should NOT activate (pre-quiet owns volume).
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL(0, lockout.setVolumeHintCalls);
}

void test_speed_vol_defers_when_pq_restore_pending() {
    // Issue a pre-quiet restore first to arm pending.
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::PreQuietRestore;
    lockout.nextResult.volumeCommand.volume = 7;
    lockout.nextResult.volumeCommand.muteVolume = 2;
    parser.setMainVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Arms pending PQ restore.

    // Now enable speed volume — PQ restore is still pending (V1 at 3, target 7).
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::None;
    speedMute.settings_.v1Volume = 3;
    speedMute.settings_.enabled = true;
    speedMute.state_.muteActive = true;
    ble.setVolumeCalls = 0;
    ctx.nowMs = 1080;  // Past 75ms PQ restore retry interval.

    module.processParsedFrame(ctx);

    // Speed volume should defer — PQ restore pending. The only setVolume
    // should be the PQ restore retry (not a speed volume drop).
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL(0, lockout.setVolumeHintCalls);
}

void test_speed_vol_active_prequiet_enters_captures_correct_original() {
    // Speed volume active at vol 3, original was 7.
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate speed vol.

    // Verify hint was set with original volume.
    TEST_ASSERT_EQUAL(1, lockout.setVolumeHintCalls);
    TEST_ASSERT_EQUAL_UINT8(7, lockout.volumeHintMain);

    // Now pre-quiet enters while speed vol is active.
    // The volume hint ensures pre-quiet will capture 7 (not 3).
    lockout.mockPreQuietActive = true;
    lockout.nextResult.volumeCommand.type = LockoutVolumeCommandType::PreQuietDrop;
    lockout.nextResult.volumeCommand.volume = 0;
    lockout.nextResult.volumeCommand.muteVolume = 0;
    parser.setMainVolume(3);  // V1 is at speed-lowered volume.
    ble.setVolumeCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Pre-quiet drop command should execute.
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
}

void test_speed_vol_band_override_restores_on_ka() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate speed vol.

    // Ka alert appears.
    parser.setActiveBands(0x02);  // BAND_KA
    parser.setAlerts({
        AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 35500, true, true)
    });
    ble.setVolumeCalls = 0;
    lockout.clearVolumeHintCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Should restore to original.
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL(1, lockout.clearVolumeHintCalls);
}

void test_speed_vol_band_override_restores_on_laser() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate speed vol.

    // Laser alert appears.
    parser.setActiveBands(0x01);  // BAND_LASER
    parser.setAlerts({
        AlertData::create(BAND_LASER, DIR_FRONT, 8, 0, 0, true, true)
    });
    ble.setVolumeCalls = 0;
    lockout.clearVolumeHintCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Should restore to original.
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL(1, lockout.clearVolumeHintCalls);
}

void test_speed_vol_no_band_override_for_k_band() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate speed vol.

    // K-band alert appears — should NOT trigger override (only Laser/Ka do).
    parser.setActiveBands(0x04);  // BAND_K
    parser.setAlerts({
        AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24150, true, true)
    });
    parser.setMainVolume(3);
    ble.setVolumeCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Should NOT restore — K band is not overridden.
    TEST_ASSERT_EQUAL(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL(0, lockout.clearVolumeHintCalls);
}

void test_speed_vol_settings_disable_restores() {
    enableSpeedVolume(3);

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;
    module.processParsedFrame(ctx);  // Activate speed vol.

    // User changes settings: disables V1 volume (set to 0xFF).
    speedMute.settings_.v1Volume = 0xFF;
    parser.setMainVolume(3);
    ble.setVolumeCalls = 0;
    lockout.clearVolumeHintCalls = 0;
    volumeFade.setBaselineHintCalls = 0;
    ctx.nowMs = 2000;

    module.processParsedFrame(ctx);

    // Should restore to original.
    TEST_ASSERT_EQUAL(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(7, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL(1, lockout.clearVolumeHintCalls);
    TEST_ASSERT_EQUAL(1, volumeFade.setBaselineHintCalls);
}

void test_speed_vol_gates_volume_fade() {
    enableSpeedVolume(3);
    // Use K-band alert (not overridden) to keep speed vol active.
    parser.state.muted = false;
    parser.setActiveBands(0x04);  // BAND_K
    parser.setAlerts({
        AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24150, true, true)
    });
    volumeFade.nextAction.type = VolumeFadeAction::Type::FADE_DOWN;
    volumeFade.nextAction.targetVolume = 2;
    volumeFade.nextAction.targetMuteVolume = 1;

    DisplayOrchestrationParsedContext ctx;
    ctx.nowMs = 1000;
    ctx.parsedReady = true;

    module.processParsedFrame(ctx);

    // Volume fade should not run while speed vol is active.
    TEST_ASSERT_EQUAL(0, volumeFade.processCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_early_updates_ble_context_and_proxy_status);
    RUN_TEST(test_process_early_updates_preview_or_restore_path);
    RUN_TEST(test_parsed_frame_sets_status_indicators_and_requests_pipeline);
    RUN_TEST(test_parsed_frame_skips_pipeline_when_preview_running);
    RUN_TEST(test_parsed_frame_syncs_prequiet_presentation_from_coordinator);
    RUN_TEST(test_parsed_frame_syncs_speedvol_zero_presentation_from_coordinator);
    RUN_TEST(test_parsed_frame_executes_lockout_restore_through_single_volume_owner);
    RUN_TEST(test_parsed_frame_executes_volume_fade_when_pipeline_runs);
    RUN_TEST(test_parsed_frame_skips_volume_fade_when_lockout_volume_command_owns_frame);
    RUN_TEST(test_stale_lockout_badge_is_cleared_when_disconnected);
    RUN_TEST(test_lightweight_refresh_updates_frequency_and_cards);
    RUN_TEST(test_lightweight_refresh_falls_back_from_invalid_priority);
    RUN_TEST(test_pipeline_draw_resets_frequency_timer_for_same_tick);
    RUN_TEST(test_lightweight_refresh_clears_frequency_when_idle);
    RUN_TEST(test_lightweight_refresh_does_not_touch_frequency_while_preview_running);
    RUN_TEST(test_prequiet_restore_retries_until_v1_confirms);
    RUN_TEST(test_prequiet_restore_confirmed_when_v1_volume_matches);
    RUN_TEST(test_prequiet_restore_timeout_clears_pending);
    RUN_TEST(test_prequiet_drop_clears_pending_restore);
    RUN_TEST(test_speed_vol_drops_volume_when_mute_active);
    RUN_TEST(test_speed_vol_confirmed_by_v1_holds_steady);
    RUN_TEST(test_speed_vol_retries_until_v1_confirms_drop);
    RUN_TEST(test_speed_vol_restores_when_mute_deactivates);
    RUN_TEST(test_speed_vol_restore_retries_until_confirmed);
    RUN_TEST(test_speed_vol_restore_times_out);
    RUN_TEST(test_speed_vol_defers_when_prequiet_active);
    RUN_TEST(test_speed_vol_defers_when_pq_restore_pending);
    RUN_TEST(test_speed_vol_active_prequiet_enters_captures_correct_original);
    RUN_TEST(test_speed_vol_band_override_restores_on_ka);
    RUN_TEST(test_speed_vol_band_override_restores_on_laser);
    RUN_TEST(test_speed_vol_no_band_override_for_k_band);
    RUN_TEST(test_speed_vol_settings_disable_restores);
    RUN_TEST(test_speed_vol_gates_volume_fade);
    return UNITY_END();
}

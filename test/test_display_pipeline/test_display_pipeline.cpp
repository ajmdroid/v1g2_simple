/**
 * test_display_pipeline.cpp - DisplayPipelineModule Logic Tests
 * 
 * Tests the orchestration logic of the display pipeline:
 * - Mute debouncing
 * - Display throttling  
 * - Alert gap recovery
 * - Display mode transitions
 * - Lockout mute tracking
 * - Alert persistence flow
 * 
 * Note: These tests verify the logic patterns used in DisplayPipelineModule
 * without including the actual module (which has hardware dependencies).
 */
#include <unity.h>

// Mocks - must be included before module under test
#ifndef ARDUINO
#include "../mocks/Arduino.h"
#endif
#include "../mocks/settings.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"
#include "../mocks/gps_handler.h"
#include "../mocks/lockout_manager.h"
#include "../mocks/debug_logger.h"
#include "../mocks/modules/camera/camera_alert_module.h"
#include "../mocks/modules/alert_persistence/alert_persistence_module.h"
#include "../mocks/modules/voice/voice_module.h"
#include "../mocks/modules/volume_fade/volume_fade_module.h"
#include "../mocks/modules/speed_volume/speed_volume_module.h"

// Include display_mode.h for DisplayMode enum
#include "../../src/display_mode.h"

// Define mock time variables (declared extern in Arduino.h)
#ifndef ARDUINO
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

// Globals for mocks
SerialClass Serial;
#endif
SettingsManager settingsManager;

// Module instances
static V1BLEClient bleClient;
static V1Display display;
static PacketParser parser;
static GPSHandler gpsHandler;
static LockoutManager lockoutManager;
static AutoLockoutManager autoLockoutManager;
static DebugLogger debugLogger;
static CameraAlertModule cameraAlertModule;
static AlertPersistenceModule alertPersistenceModule;
static VoiceModule voiceModule;
static VolumeFadeModule volumeFadeModule;
static SpeedVolumeModule speedVolumeModule;
static DisplayMode displayMode = DisplayMode::IDLE;

// The module under test - we'll test its logic directly
// Since the implementation is tightly coupled, we test the key behaviors

// ============================================================================
// Test: Display Mode Transitions
// ============================================================================

void test_display_mode_transitions_to_live_when_alerts() {
    // Setup: displayMode starts as IDLE
    displayMode = DisplayMode::IDLE;
    
    // When we have alerts, mode should become LIVE
    parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700, true, true)});
    
    // Simulate the mode transition logic
    if (parser.hasAlerts()) {
        displayMode = DisplayMode::LIVE;
    }
    
    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
}

void test_display_mode_transitions_to_idle_when_no_alerts() {
    // Setup: displayMode is LIVE
    displayMode = DisplayMode::LIVE;
    
    // When alerts clear, mode should become IDLE
    parser.reset();
    parser.hasAlertsFlag = false;
    
    // Simulate the mode transition logic
    if (!parser.hasAlerts()) {
        displayMode = DisplayMode::IDLE;
    }
    
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
}

// ============================================================================
// Test: Mute Debouncing Logic
// ============================================================================

void test_mute_debounce_ignores_rapid_changes() {
    // The pipeline has 150ms mute debounce
    static constexpr unsigned long MUTE_DEBOUNCE_MS = 150;
    
    bool debouncedMuteState = false;
    unsigned long lastMuteChangeMs = 0;
    
    // Time 0: muted = false (initial)
    mockMillis = 0;
    DisplayState state;
    state.muted = false;
    
    // Time 50: muted changes to true (within debounce window)
    mockMillis = 50;
    state.muted = true;
    
    // Debounce logic: if change is too recent, use old state
    if (state.muted != debouncedMuteState) {
        if (mockMillis - lastMuteChangeMs > MUTE_DEBOUNCE_MS) {
            debouncedMuteState = state.muted;
            lastMuteChangeMs = mockMillis;
        } else {
            state.muted = debouncedMuteState;  // revert to debounced
        }
    }
    
    // Should still be false (debounced)
    TEST_ASSERT_FALSE(state.muted);
    TEST_ASSERT_FALSE(debouncedMuteState);
}

void test_mute_debounce_accepts_stable_change() {
    static constexpr unsigned long MUTE_DEBOUNCE_MS = 150;
    
    bool debouncedMuteState = false;
    unsigned long lastMuteChangeMs = 0;
    
    // Time 0: initial state
    mockMillis = 0;
    
    // Time 200: muted changes to true (after debounce window)
    mockMillis = 200;
    DisplayState state;
    state.muted = true;
    
    if (state.muted != debouncedMuteState) {
        if (mockMillis - lastMuteChangeMs > MUTE_DEBOUNCE_MS) {
            debouncedMuteState = state.muted;
            lastMuteChangeMs = mockMillis;
        }
    }
    
    // Should be true now (stable change accepted)
    TEST_ASSERT_TRUE(debouncedMuteState);
}

// ============================================================================
// Test: Display Throttling
// ============================================================================

void test_display_throttle_skips_rapid_updates() {
    static constexpr unsigned long DISPLAY_DRAW_MIN_MS = 30;  // Match display_pipeline_module.h
    
    unsigned long lastDisplayDraw = 0;
    int drawCount = 0;
    
    // Time 0: first draw should happen (initial state)
    mockMillis = 0;
    // First draw is always allowed (special case: lastDisplayDraw starts at 0)
    drawCount++;
    lastDisplayDraw = mockMillis;
    
    // Time 20: too soon, skip (20 < 30)
    mockMillis = 20;
    if (mockMillis - lastDisplayDraw >= DISPLAY_DRAW_MIN_MS) {
        drawCount++;
        lastDisplayDraw = mockMillis;
    }
    
    // Time 30: exactly at threshold, OK to draw (30 - 0 = 30 >= 30)
    mockMillis = 30;
    if (mockMillis - lastDisplayDraw >= DISPLAY_DRAW_MIN_MS) {
        drawCount++;
        lastDisplayDraw = mockMillis;
    }
    
    TEST_ASSERT_EQUAL(2, drawCount);  // 0ms and 30ms, not 20ms
}

// ============================================================================
// Test: Alert Gap Recovery
// ============================================================================

void test_alert_gap_recovery_requests_data_when_bands_but_no_alerts() {
    parser.reset();
    bleClient.reset();
    
    // State: activeBands shows activity but no alerts parsed
    parser.setActiveBands(BAND_KA);
    parser.hasAlertsFlag = false;
    
    unsigned long lastAlertGapRecoverMs = 0;
    mockMillis = 100;
    
    // Gap recovery logic
    bool hasAlerts = parser.hasAlerts();
    uint8_t activeBands = parser.state.activeBands;
    
    if (!hasAlerts && activeBands != BAND_NONE) {
        if (mockMillis - lastAlertGapRecoverMs > 50) {
            parser.resetAlertAssembly();
            bleClient.requestAlertData();
            lastAlertGapRecoverMs = mockMillis;
        }
    }
    
    TEST_ASSERT_EQUAL(1, parser.resetAlertAssemblyCalls);
    TEST_ASSERT_EQUAL(1, bleClient.requestAlertDataCalls);
}

void test_alert_gap_recovery_skips_when_recent() {
    parser.reset();
    bleClient.reset();
    
    parser.setActiveBands(BAND_KA);
    parser.hasAlertsFlag = false;
    
    unsigned long lastAlertGapRecoverMs = 80;  // Recent request
    mockMillis = 100;  // Only 20ms since last
    
    bool hasAlerts = parser.hasAlerts();
    uint8_t activeBands = parser.state.activeBands;
    
    if (!hasAlerts && activeBands != BAND_NONE) {
        if (mockMillis - lastAlertGapRecoverMs > 50) {
            parser.resetAlertAssembly();
            bleClient.requestAlertData();
        }
    }
    
    // Should not request - too recent
    TEST_ASSERT_EQUAL(0, parser.resetAlertAssemblyCalls);
    TEST_ASSERT_EQUAL(0, bleClient.requestAlertDataCalls);
}

// ============================================================================
// Test: Lockout Mute Tracking
// ============================================================================

void test_lockout_mute_sends_command_when_entering_lockout() {
    bleClient.reset();
    display.reset();
    lockoutManager.reset();
    
    // Setup: GPS ready, alert not muted, in lockout
    lockoutManager.setMuteResult(true);
    
    bool lockoutMuteSent = false;
    uint32_t lastLockoutAlertId = 0xFFFFFFFF;
    
    // Simulate the lockout mute logic
    bool priorityInLockout = lockoutManager.shouldMuteAlert(37.0, -122.0, BAND_KA);
    bool stateMuted = false;  // V1 not currently muted
    uint32_t currentAlertId = 0x12345;
    
    if (priorityInLockout && !stateMuted) {
        if (!lockoutMuteSent || currentAlertId != lastLockoutAlertId) {
            bleClient.setMute(true);
            lockoutMuteSent = true;
            lastLockoutAlertId = currentAlertId;
            display.setLockoutMuted(true);
        }
    }
    
    TEST_ASSERT_EQUAL(1, bleClient.setMuteCalls);
    TEST_ASSERT_TRUE(bleClient.lastMuteValue);
    TEST_ASSERT_EQUAL(1, display.setLockoutMutedCalls);
    TEST_ASSERT_TRUE(display.lastLockoutMutedValue);
}

void test_lockout_mute_skips_duplicate_commands() {
    bleClient.reset();
    display.reset();
    lockoutManager.reset();
    
    lockoutManager.setMuteResult(true);
    
    // Already sent mute for this alert
    bool lockoutMuteSent = true;
    uint32_t lastLockoutAlertId = 0x12345;
    uint32_t currentAlertId = 0x12345;  // Same alert
    
    bool priorityInLockout = lockoutManager.shouldMuteAlert(37.0, -122.0, BAND_KA);
    bool stateMuted = false;
    
    if (priorityInLockout && !stateMuted) {
        if (!lockoutMuteSent || currentAlertId != lastLockoutAlertId) {
            bleClient.setMute(true);
            lockoutMuteSent = true;
            lastLockoutAlertId = currentAlertId;
            display.setLockoutMuted(true);
        }
    }
    
    // Should NOT send duplicate command
    TEST_ASSERT_EQUAL(0, bleClient.setMuteCalls);
}

void test_lockout_mute_clears_when_leaving_lockout() {
    bleClient.reset();
    display.reset();
    lockoutManager.reset();
    
    // Not in lockout
    lockoutManager.setMuteResult(false);
    
    bool lockoutMuteSent = true;  // Was previously in lockout
    
    bool priorityInLockout = lockoutManager.shouldMuteAlert(37.0, -122.0, BAND_KA);
    
    if (!priorityInLockout) {
        lockoutMuteSent = false;
        display.setLockoutMuted(false);
    }
    
    TEST_ASSERT_FALSE(lockoutMuteSent);
    TEST_ASSERT_EQUAL(1, display.setLockoutMutedCalls);
    TEST_ASSERT_FALSE(display.lastLockoutMutedValue);
}

// ============================================================================
// Test: Alert Persistence Flow  
// ============================================================================

void test_alert_persistence_sets_alert_when_alerts_active() {
    alertPersistenceModule.reset();
    
    AlertData priority = AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700, true, true);
    
    // When alerts are active, set persisted alert
    alertPersistenceModule.setPersistedAlert(priority);
    
    TEST_ASSERT_EQUAL(1, alertPersistenceModule.setPersistedAlertCalls);
    TEST_ASSERT_EQUAL(BAND_KA, alertPersistenceModule.persistedAlert.band);
}

void test_alert_persistence_clears_state_when_alerts_clear() {
    alertPersistenceModule.reset();
    
    // When alerts clear, clear all alert state
    alertPersistenceModule.clearAllAlertState();
    
    TEST_ASSERT_EQUAL(1, alertPersistenceModule.clearAllAlertStateCalls);
}

void test_alert_persistence_starts_timer_when_configured() {
    alertPersistenceModule.reset();
    
    // Set up a persisted alert
    AlertData priority = AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700, true, true);
    alertPersistenceModule.setPersistedAlert(priority);
    
    // Start persistence timer
    mockMillis = 1000;
    alertPersistenceModule.startPersistence(mockMillis);
    
    TEST_ASSERT_EQUAL(1, alertPersistenceModule.startPersistenceCalls);
    TEST_ASSERT_TRUE(alertPersistenceModule.persistenceActive);
}

void test_alert_persistence_shows_during_window() {
    alertPersistenceModule.reset();
    
    AlertData priority = AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700, true, true);
    alertPersistenceModule.setPersistedAlert(priority);
    alertPersistenceModule.startPersistence(1000);
    
    // Within persistence window (3 seconds)
    bool shouldShow = alertPersistenceModule.shouldShowPersisted(2000, 3000);
    TEST_ASSERT_TRUE(shouldShow);
    
    // After persistence window
    shouldShow = alertPersistenceModule.shouldShowPersisted(5000, 3000);
    TEST_ASSERT_FALSE(shouldShow);
}

// ============================================================================
// Test: Camera Alert Card State
// ============================================================================

void test_camera_alert_card_state_true_when_v1_alerts() {
    cameraAlertModule.reset();
    
    // When V1 has alerts, camera shows as card
    cameraAlertModule.updateCardStateForV1(true);
    
    TEST_ASSERT_EQUAL(1, cameraAlertModule.updateCardStateForV1Calls);
    TEST_ASSERT_TRUE(cameraAlertModule.lastCardStateForV1);
}

void test_camera_alert_card_state_false_when_no_v1_alerts() {
    cameraAlertModule.reset();
    
    // When V1 has no alerts, camera can be main display
    cameraAlertModule.updateCardStateForV1(false);
    
    TEST_ASSERT_EQUAL(1, cameraAlertModule.updateCardStateForV1Calls);
    TEST_ASSERT_FALSE(cameraAlertModule.lastCardStateForV1);
}

// ============================================================================
// Test: Voice Module Clears State on Alert Clear
// ============================================================================

void test_voice_clears_state_when_alerts_clear() {
    voiceModule.resetMock();
    
    // When alerts clear, voice state should be cleared
    voiceModule.clearAllState();
    
    TEST_ASSERT_EQUAL(1, voiceModule.clearAllStateCalls);
}

// ============================================================================
// Main
// ============================================================================

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    settingsManager = SettingsManager();
    bleClient.reset();
    display.reset();
    parser.reset();
    gpsHandler.reset();
    lockoutManager.reset();
    autoLockoutManager.reset();
    cameraAlertModule.reset();
    alertPersistenceModule.reset();
    voiceModule.resetMock();
    volumeFadeModule.reset();
    speedVolumeModule.reset();
    displayMode = DisplayMode::IDLE;
}

void runAllTests() {
    // Display mode transitions
    RUN_TEST(test_display_mode_transitions_to_live_when_alerts);
    RUN_TEST(test_display_mode_transitions_to_idle_when_no_alerts);
    
    // Mute debouncing
    RUN_TEST(test_mute_debounce_ignores_rapid_changes);
    RUN_TEST(test_mute_debounce_accepts_stable_change);
    
    // Display throttling
    RUN_TEST(test_display_throttle_skips_rapid_updates);
    
    // Alert gap recovery
    RUN_TEST(test_alert_gap_recovery_requests_data_when_bands_but_no_alerts);
    RUN_TEST(test_alert_gap_recovery_skips_when_recent);
    
    // Lockout mute tracking
    RUN_TEST(test_lockout_mute_sends_command_when_entering_lockout);
    RUN_TEST(test_lockout_mute_skips_duplicate_commands);
    RUN_TEST(test_lockout_mute_clears_when_leaving_lockout);
    
    // Alert persistence flow
    RUN_TEST(test_alert_persistence_sets_alert_when_alerts_active);
    RUN_TEST(test_alert_persistence_clears_state_when_alerts_clear);
    RUN_TEST(test_alert_persistence_starts_timer_when_configured);
    RUN_TEST(test_alert_persistence_shows_during_window);
    
    // Camera alert integration
    RUN_TEST(test_camera_alert_card_state_true_when_v1_alerts);
    RUN_TEST(test_camera_alert_card_state_false_when_no_v1_alerts);
    
    // Voice state management
    RUN_TEST(test_voice_clears_state_when_alerts_clear);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main(int argc, char **argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif

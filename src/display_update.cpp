/**
 * Update methods — extracted from display.cpp (Phase 3C/3D)
 *
 * Contains update(DisplayState), update(AlertData, ...), refreshFrequencyOnly,
 * refreshSecondaryAlertCards, updatePersisted.
 */

#include "display.h"
#include "../include/display_layout.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "../include/display_log.h"
#include "../include/display_flush.h"
#include "../include/display_vol_warn.h"
#include "settings.h"
#include "audio_beep.h"
#include "perf_metrics.h"
#include "packet_parser.h"
using DisplayLayout::SECONDARY_ROW_HEIGHT;
using DisplayLayout::PRIMARY_ZONE_HEIGHT;

// RSSI periodic update timer (shared between resting and alert modes)
static unsigned long s_lastRssiUpdateMs = 0;
static constexpr unsigned long RSSI_UPDATE_INTERVAL_MS = 2000;  // Update RSSI every 2 seconds

inline bool shouldRefreshRssi(unsigned long nowMs) {
    return (nowMs - s_lastRssiUpdateMs) >= RSSI_UPDATE_INTERVAL_MS;
}

inline void markRssiRefreshed(unsigned long nowMs) {
    s_lastRssiUpdateMs = nowMs;
}

// Debug timing for display operations (set to true to profile display)
static constexpr bool DISPLAY_PERF_TIMING = false;  // Disable for production
static unsigned long _dispPerfStart = 0;
#define DISP_PERF_START() do { if (DISPLAY_PERF_TIMING) _dispPerfStart = micros(); } while(0)
#define DISP_PERF_LOG(label) do { if (DISPLAY_PERF_TIMING) { \
    unsigned long _dur = micros() - _dispPerfStart; \
    if (_dur > 5000) Serial.printf("[DISP] %s: %luus\n", label, _dur); \
    _dispPerfStart = micros(); \
} } while(0)

namespace {

PerfDisplayRenderPath liveRenderPathForScenario() {
    const PerfDisplayRenderScenario scenario = perfGetDisplayRenderScenario();
    if (scenario == PerfDisplayRenderScenario::Restore) {
        return PerfDisplayRenderPath::Restore;
    }
    if (scenario == PerfDisplayRenderScenario::PreviewFirstFrame ||
        scenario == PerfDisplayRenderScenario::PreviewSteadyFrame) {
        return PerfDisplayRenderPath::Preview;
    }
    return PerfDisplayRenderPath::Full;
}

PerfDisplayRenderPath restingRenderPathForScenario() {
    const PerfDisplayRenderScenario scenario = perfGetDisplayRenderScenario();
    if (scenario == PerfDisplayRenderScenario::Restore) {
        return PerfDisplayRenderPath::Restore;
    }
    return PerfDisplayRenderPath::RestingFull;
}

PerfDisplayRenderPath persistedRenderPathForScenario() {
    return (perfGetDisplayRenderScenario() == PerfDisplayRenderScenario::Restore)
               ? PerfDisplayRenderPath::Restore
               : PerfDisplayRenderPath::Persisted;
}

void recordDisplayRedrawReasonIf(bool condition, PerfDisplayRedrawReason reason) {
    if (condition) {
        perfRecordDisplayRedrawReason(reason);
    }
}

}  // namespace

void V1Display::drawStatusStrip(const DisplayState& state,
                                char topChar,
                                bool topMuted,
                                bool topDot) {
    syncTopIndicators(millis());
    drawTopCounter(topChar, topMuted, topDot);
    const V1Settings& s = settingsManager.get();
    static bool rightStripCleared = false;
    const bool showVolumeAndRssi = state.supportsVolume() && !s.hideVolumeIndicator;
    if (showVolumeAndRssi) {
        drawVolumeIndicator(state.mainVolume, state.muteVolume);
        drawRssiIndicator(bleCtx_.v1Rssi);
        rightStripCleared = false;
    } else {
        // Clear once on transition into hidden/unsupported state.
        if (!rightStripCleared) {
            FILL_RECT(8, 75, 75, 68, PALETTE_BG);
            rightStripCleared = true;
        }
    }
}

void V1Display::updateStatusStripIncremental(const DisplayState& state,
                                             char topChar,
                                             bool topMuted,
                                             bool topDot,
                                             bool volumeChanged,
                                             bool rssiNeedsUpdate,
                                             bool bogeyCounterChanged,
                                             uint8_t& lastMainVol,
                                             uint8_t& lastMuteVol,
                                             uint8_t& lastBogeyByte,
                                             unsigned long now,
                                             bool& flushLeftStrip,
                                             bool& flushRightStrip) {
    syncTopIndicators(now);
    const V1Settings& s = settingsManager.get();
    const bool showVolumeAndRssi = state.supportsVolume() && !s.hideVolumeIndicator;

    if (volumeChanged && showVolumeAndRssi) {
        lastMainVol = state.mainVolume;
        lastMuteVol = state.muteVolume;
        drawVolumeIndicator(state.mainVolume, state.muteVolume);
        drawRssiIndicator(bleCtx_.v1Rssi);
        markRssiRefreshed(now);  // Reset RSSI timer when we update with volume
        flushRightStrip = true;
    } else if (rssiNeedsUpdate && showVolumeAndRssi) {
        // Periodic RSSI-only update
        drawRssiIndicator(bleCtx_.v1Rssi);
        markRssiRefreshed(now);
        flushRightStrip = true;
    }

    if (bogeyCounterChanged) {
        lastBogeyByte = state.bogeyCounterByte;
        drawTopCounter(topChar, topMuted, topDot);
        flushLeftStrip = true;
    }

    drawLockoutIndicator();
    drawGpsIndicator();
    drawObdIndicator();
}

void V1Display::update(const DisplayState& state) {
    // Track if we're transitioning FROM persisted mode (need full redraw)
    bool wasPersistedMode = persistedMode;
    persistedMode = false;  // Not in persisted mode
    const bool requestedTrackingReset = dirty.resetTracking;
    
    // Don't process resting update if we're in Scanning mode - wait for showResting() to be called
    if (currentScreen == ScreenMode::Scanning) {
        return;
    }
    
    static bool firstUpdate = true;
    static bool wasInFlashPeriod = false;
    
    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    multiAlertMode = false;  // No cards to draw in resting state
    
    // Check if profile flash period just expired (needs redraw to clear)
    bool inFlashPeriod = (millis() - profileChangedTime) < HIDE_TIMEOUT_MS;
    bool flashJustExpired = wasInFlashPeriod && !inFlashPeriod;
    wasInFlashPeriod = inFlashPeriod;

    // Band debouncing: keep bands visible for a short grace period to prevent flicker
    static unsigned long restingBandLastSeen[4] = {0, 0, 0, 0};  // L, Ka, K, X
    static uint8_t restingDebouncedBands = 0;
    const unsigned long BAND_GRACE_MS = 100;  // Reduced from 200ms for snappier response
    unsigned long now = millis();
    
    if (state.activeBands & BAND_LASER) restingBandLastSeen[0] = now;
    if (state.activeBands & BAND_KA)    restingBandLastSeen[1] = now;
    if (state.activeBands & BAND_K)     restingBandLastSeen[2] = now;
    if (state.activeBands & BAND_X)     restingBandLastSeen[3] = now;
    
    restingDebouncedBands = state.activeBands;
    if ((now - restingBandLastSeen[0]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_LASER;
    if ((now - restingBandLastSeen[1]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_KA;
    if ((now - restingBandLastSeen[2]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_K;
    if ((now - restingBandLastSeen[3]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_X;

    // In resting mode (no alerts), never show muted visual - just normal display
    // Apps commonly set main volume to 0 when idle, adjusting on new alerts
    // The muted state should only affect active alert display, not resting
    bool effectiveMuted = false;

    // Track last debounced bands for change detection
    static uint8_t lastRestingDebouncedBands = 0;
    static uint8_t lastRestingSignalBars = 0;
    static uint8_t lastRestingArrows = 0;
    static uint8_t lastRestingMainVol = 255;
    static uint8_t lastRestingMuteVol = 255;
    static uint8_t lastRestingBogeyByte = 0;  // Track V1's bogey counter for change detection
    
    // Reset resting statics when change tracking reset is requested (on V1 disconnect)
    if (dirty.resetTracking) {
        firstUpdate = true;
        lastRestingDebouncedBands = 0;
        lastRestingSignalBars = 0;
        lastRestingArrows = 0;
        lastRestingMainVol = 255;
        lastRestingMuteVol = 255;
        lastRestingBogeyByte = 0;
        s_lastRssiUpdateMs = 0;
        // Don't clear the flag here - let the alert update() clear it
    }
    
    // Check if RSSI needs periodic refresh (every 2 seconds)
    bool rssiNeedsUpdate = shouldRefreshRssi(now);
    
    // Check if transitioning from a non-resting visual mode.
    bool leavingLiveMode = (currentScreen == ScreenMode::Live);
    
    // Separate full redraw triggers from incremental updates
    bool needsFullRedraw =
        firstUpdate ||
        flashJustExpired ||
        wasPersistedMode ||  // Force full redraw when leaving persisted mode
        leavingLiveMode ||   // Force full redraw when alerts end (clear cards/frequency)
        restingDebouncedBands != lastRestingDebouncedBands ||
        effectiveMuted != lastState.muted;
    
    bool arrowsChanged = (state.arrows != lastRestingArrows);
    bool signalBarsChanged = (state.signalBars != lastRestingSignalBars);
    bool volumeChanged = (state.mainVolume != lastRestingMainVol || state.muteVolume != lastRestingMuteVol);
    bool bogeyCounterChanged = (state.bogeyCounterByte != lastRestingBogeyByte);
    
    // Check if volume zero warning needs a flashing redraw
    bool currentProxyConnected = bleCtx_.proxyConnected;
    bool volZero = (state.mainVolume == 0 && state.hasVolumeData);
    if (volZeroWarn.needsFlashRedraw(volZero, currentProxyConnected, preQuietActive_)) {
        needsFullRedraw = true;
    }
    
    if (!needsFullRedraw && !arrowsChanged && !signalBarsChanged && !volumeChanged && !bogeyCounterChanged && !rssiNeedsUpdate) {
        if (drawRestTelemetryCards(false)) {
            perfRecordDisplayRenderPath(PerfDisplayRenderPath::CardsOnly);
            flushRegion(DisplayLayout::CONTENT_LEFT_MARGIN,
                        SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT,
                        DisplayLayout::CONTENT_AVAILABLE_WIDTH,
                        SECONDARY_ROW_HEIGHT);
        }
        return;
    }
    
    if (!needsFullRedraw && (arrowsChanged || signalBarsChanged || volumeChanged || bogeyCounterChanged || rssiNeedsUpdate)) {
        perfRecordDisplayRenderPath(PerfDisplayRenderPath::RestingIncremental);
        // Incremental update - only redraw what changed
        bool flushLeftStrip = false;
        bool flushRightStrip = false;

        if (arrowsChanged) {
            lastRestingArrows = state.arrows;
            drawDirectionArrow(state.arrows, effectiveMuted, state.flashBits);
            flushRightStrip = true;
        }
        if (signalBarsChanged) {
            lastRestingSignalBars = state.signalBars;
            Band primaryBand = BAND_KA;
            if (restingDebouncedBands & BAND_LASER) primaryBand = BAND_LASER;
            else if (restingDebouncedBands & BAND_KA) primaryBand = BAND_KA;
            else if (restingDebouncedBands & BAND_K) primaryBand = BAND_K;
            else if (restingDebouncedBands & BAND_X) primaryBand = BAND_X;
            drawVerticalSignalBars(state.signalBars, state.signalBars, primaryBand, effectiveMuted);
            flushRightStrip = true;
        }
        updateStatusStripIncremental(state,
                                     state.bogeyCounterChar,
                                     effectiveMuted,
                                     state.bogeyCounterDot,
                                     volumeChanged,
                                     rssiNeedsUpdate,
                                     bogeyCounterChanged,
                                     lastRestingMainVol,
                                     lastRestingMuteVol,
                                     lastRestingBogeyByte,
                                     now,
                                     flushLeftStrip,
                                     flushRightStrip);
        const bool cardsChanged = drawRestTelemetryCards(false);
        (void)flushLeftStrip;
        (void)flushRightStrip;
        (void)cardsChanged;
        DISPLAY_FLUSH();
        lastState = state;
        return;
    }

    perfRecordDisplayRenderPath(restingRenderPathForScenario());
    recordDisplayRedrawReasonIf(firstUpdate, PerfDisplayRedrawReason::FirstRun);
    recordDisplayRedrawReasonIf(wasPersistedMode, PerfDisplayRedrawReason::LeavePersisted);
    recordDisplayRedrawReasonIf(leavingLiveMode, PerfDisplayRedrawReason::LeaveLive);
    recordDisplayRedrawReasonIf(restingDebouncedBands != lastRestingDebouncedBands,
                                PerfDisplayRedrawReason::BandSetChange);
    recordDisplayRedrawReasonIf(arrowsChanged, PerfDisplayRedrawReason::ArrowChange);
    recordDisplayRedrawReasonIf(signalBarsChanged, PerfDisplayRedrawReason::SignalBarChange);
    recordDisplayRedrawReasonIf(volumeChanged, PerfDisplayRedrawReason::VolumeChange);
    recordDisplayRedrawReasonIf(bogeyCounterChanged,
                                PerfDisplayRedrawReason::BogeyCounterChange);
    recordDisplayRedrawReasonIf(rssiNeedsUpdate, PerfDisplayRedrawReason::RssiRefresh);
    recordDisplayRedrawReasonIf(flashJustExpired, PerfDisplayRedrawReason::FlashTick);
    recordDisplayRedrawReasonIf(requestedTrackingReset || (effectiveMuted != lastState.muted),
                                PerfDisplayRedrawReason::ForceRedraw);

    // Full redraw needed
    firstUpdate = false;
    lastRestingDebouncedBands = restingDebouncedBands;
    lastRestingArrows = state.arrows;
    lastRestingSignalBars = state.signalBars;
    lastRestingMainVol = state.mainVolume;
    lastRestingMuteVol = state.muteVolume;
    lastRestingBogeyByte = state.bogeyCounterByte;
    markRssiRefreshed(now);  // Reset RSSI timer on full redraw
    
    uint32_t stageStartUs = micros();
    drawBaseFrame();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BaseFrame,
                                      micros() - stageStartUs);
    // Use V1's decoded bogey counter byte - shows mode, volume, etc.
    char topChar = state.bogeyCounterChar;
    stageStartUs = micros();
    drawStatusStrip(state, topChar, effectiveMuted, state.bogeyCounterDot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::StatusStrip,
                                      micros() - stageStartUs);
    drawBandIndicators(restingDebouncedBands, effectiveMuted);
    // BLE proxy status indicator
    
    // Determine primary band for frequency and signal bar coloring
    Band primaryBand = BAND_NONE;
    if (restingDebouncedBands & BAND_LASER) primaryBand = BAND_LASER;
    else if (restingDebouncedBands & BAND_KA) primaryBand = BAND_KA;
    else if (restingDebouncedBands & BAND_K) primaryBand = BAND_K;
    else if (restingDebouncedBands & BAND_X) primaryBand = BAND_X;
    
    // Volume-zero warning: 15s delay → 10s flashing "VOL 0" → acknowledge
    bool proxyConnected = bleCtx_.proxyConnected;
    bool showVolumeWarning = volZeroWarn.evaluate(
        volZero, proxyConnected, preQuietActive_, play_vol0_beep);
    
    if (showVolumeWarning) {
        drawVolumeZeroWarning();
    } else {
        stageStartUs = micros();
        drawFrequency(0, primaryBand, effectiveMuted);
        perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Frequency,
                                          micros() - stageStartUs);
    }

    stageStartUs = micros();
    drawVerticalSignalBars(state.signalBars, state.signalBars, primaryBand, effectiveMuted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);
    // Never draw arrows in resting display - arrows should only appear in live mode
    // when we have actual alert data with frequency. If display packet has arrows but
    // no alert packet arrived, we shouldn't show arrows without frequency.
    stageStartUs = micros();
    drawDirectionArrow(DIR_NONE, effectiveMuted, 0);
    drawMuteIcon(effectiveMuted);
    drawLockoutIndicator();
    drawGpsIndicator();
    drawObdIndicator();
    drawProfileIndicator(currentProfileSlot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::ArrowsIcons,
                                      micros() - stageStartUs);
    
    // Clear any persisted card slots when entering resting state
    AlertData emptyPriority;
    stageStartUs = micros();
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, effectiveMuted);
    drawRestTelemetryCards(true);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Cards,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    DISPLAY_FLUSH();  // Push canvas to display
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Flush,
                                      micros() - stageStartUs);

    dirty.resetTracking = false;

    if (currentScreen != ScreenMode::Resting) {
        perfRecordDisplayScreenTransition(
            static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
            PerfDisplayScreen::Resting,
            millis());
    }
    currentScreen = ScreenMode::Resting;  // Set screen mode after redraw complete
    lastState = state;
}

void V1Display::refreshFrequencyOnly(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    drawFrequency(freqMHz, band, muted, isPhotoRadar);

    const uint32_t nowMs = millis();
    syncTopIndicators(nowMs);

    const bool gpsShow = gpsSatEnabled_ && gpsSatHasFix_;
    const uint8_t gpsSats = gpsShow ? gpsSatCount_ : 0;
    const bool obdShow = obdEnabled_;
    const bool obdConnected = obdShow && obdConnected_;

    const bool forceBadgeFlush = dirty.lockout || dirty.gpsIndicator || dirty.obdIndicator;

    drawLockoutIndicator();
    drawGpsIndicator();
    drawObdIndicator();

    static bool badgeCacheValid = false;
    static bool lastLockoutShown = false;
    static bool lastGpsShown = false;
    static uint8_t lastGpsSats = 0;
    static bool lastObdShown = false;
    static bool lastObdConnected = false;

    const bool badgeStripChanged =
        !badgeCacheValid ||
        forceBadgeFlush ||
        (lockoutIndicatorShown_ != lastLockoutShown) ||
        (gpsShow != lastGpsShown) ||
        (gpsSats != lastGpsSats) ||
        (obdShow != lastObdShown) ||
        (obdConnected != lastObdConnected);

    if (badgeStripChanged) {
        // Top status strip containing GPS / lockout / OBD badges.
        flushRegion(120, 0, 320, 36);
        badgeCacheValid = true;
        lastLockoutShown = lockoutIndicatorShown_;
        lastGpsShown = gpsShow;
        lastGpsSats = gpsSats;
        lastObdShown = obdShow;
        lastObdConnected = obdConnected;
    }

    if (frequencyRenderDirty) {
        if (frequencyDirtyValid) {
            flushRegion(frequencyDirtyX, frequencyDirtyY, frequencyDirtyW, frequencyDirtyH);
        } else {
            // Conservative fallback when no precise dirty region was captured.
            flushRegion(DisplayLayout::CONTENT_LEFT_MARGIN,
                        DisplayLayout::PRIMARY_ZONE_Y,
                        DisplayLayout::CONTENT_AVAILABLE_WIDTH,
                        DisplayLayout::PRIMARY_ZONE_HEIGHT);
        }
    }
}

void V1Display::refreshSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted) {
    drawSecondaryAlertCards(alerts, alertCount, priority, muted);
    flushRegion(0,
                SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT,
                SCREEN_WIDTH,
                SECONDARY_ROW_HEIGHT);
}

// Persisted alert display - shows last alert in dark grey after V1 clears it
// Only draws frequency, band, and arrows - no signal bars, no mute badge
// Bogey counter shows V1 mode (from state), not "1"
void V1Display::updatePersisted(const AlertData& alert, const DisplayState& state) {
    if (!alert.isValid) {
        persistedMode = false;
        update(state);  // Fall back to normal resting display
        return;
    }
    
    // Enable persisted mode so draw functions use PALETTE_PERSISTED instead of PALETTE_MUTED
    persistedMode = true;
    
    if (currentScreen != ScreenMode::Persisted) {
        perfRecordDisplayScreenTransition(
            static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
            PerfDisplayScreen::Persisted,
            millis());
    }
    currentScreen = ScreenMode::Persisted;
    perfRecordDisplayRenderPath(persistedRenderPathForScenario());
    
    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    multiAlertMode = false;  // No cards to draw
    wasInMultiAlertMode = false;
    
    uint32_t stageStartUs = micros();
    drawBaseFrame();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BaseFrame,
                                      micros() - stageStartUs);
    
    // Bogey counter shows V1's decoded display - NOT greyed, always visible
    char topChar = state.bogeyCounterChar;
    stageStartUs = micros();
    drawStatusStrip(state, topChar, false, state.bogeyCounterDot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::StatusStrip,
                                      micros() - stageStartUs);
    
    // Band indicator in persisted color
    uint8_t bandMask = alert.band;
    stageStartUs = micros();
    drawBandIndicators(bandMask, true);  // muted=true triggers PALETTE_MUTED_OR_PERSISTED
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);
    
    // Frequency in persisted color (pass muted=true)
    const bool isPhotoRadar =
        (alert.photoType != 0) ||
        state.hasPhotoAlert ||
        (state.bogeyCounterChar == 'P');
    stageStartUs = micros();
    drawFrequency(alert.frequency, alert.band, true, isPhotoRadar);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Frequency,
                                      micros() - stageStartUs);
    
    // No signal bars - just draw empty
    stageStartUs = micros();
    drawVerticalSignalBars(0, 0, alert.band, true);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);
    
    // Arrows in dark grey
    stageStartUs = micros();
    drawDirectionArrow(alert.direction, true);  // muted=true for grey
    
    // No mute badge
    // drawMuteIcon intentionally skipped

    // Profile indicator still shown
    drawProfileIndicator(currentProfileSlot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::ArrowsIcons,
                                      micros() - stageStartUs);
    
    // Clear card area AND expire all tracked card slots (no cards during persisted state)
    // This prevents stale cards from reappearing when returning to live alerts
    AlertData emptyPriority;
    stageStartUs = micros();
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, true);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Cards,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    DISPLAY_FLUSH();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Flush,
                                      micros() - stageStartUs);
}

// Multi-alert update: draws priority alert with secondary alert cards below
void V1Display::update(const AlertData& priority, const AlertData* allAlerts, int alertCount, const DisplayState& state) {
    // Check if we're transitioning FROM persisted mode (need full redraw to restore colors)
    bool wasPersistedMode = persistedMode;
    persistedMode = false;  // Not in persisted mode
    const bool requestedTrackingReset = dirty.resetTracking;

    // Get settings reference for priorityArrowOnly
    const V1Settings& s = settingsManager.get();

    // If no valid priority alert, return (caller should use updatePersisted or update(state) instead)
    if (!priority.isValid || priority.band == BAND_NONE) {
        PERF_INC(displayLiveInvalidPrioritySkips);
        return;
    }

    // Track screen mode transitions - force redraw when entering live mode from resting/scanning
    bool enteringLiveMode = (currentScreen != ScreenMode::Live);
    if (enteringLiveMode) {
        DISPLAY_LOG("[DISP] Entering Live mode (was %d), alertCount=%d\n", 
                    (int)currentScreen, alertCount);
        perfRecordDisplayScreenTransition(
            static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
            PerfDisplayScreen::Live,
            millis());
    }
    currentScreen = ScreenMode::Live;

    // Always use multi-alert mode (raised layout for cards)
    dirty.multiAlert = true;
    multiAlertMode = true;

    // V1 is source of truth - use activeBands directly, no debouncing
    // This allows V1's native blinking to come through

    char liveTopCounterChar = state.bogeyCounterChar;
    bool liveTopCounterDot = state.bogeyCounterDot;

    // Change detection: check if we need to redraw
    static AlertData lastPriority;
    static uint8_t lastBogeyByte = 0;  // Track V1's bogey counter byte for change detection
    static DisplayState lastMultiState;
    static bool firstRun = true;
    static AlertData lastSecondary[PacketParser::MAX_ALERTS];  // Track all 15 possible V1 alerts for change detection
    static uint8_t lastArrows = 0;
    static uint8_t lastSignalBars = 0;
    static uint8_t lastActiveBands = 0;
    
    // Check if reset was requested (e.g., on V1 disconnect)
    if (dirty.resetTracking) {
        lastPriority = AlertData();
        lastBogeyByte = 0;
        lastMultiState = DisplayState();
        firstRun = true;
        for (int i = 0; i < PacketParser::MAX_ALERTS; i++) lastSecondary[i] = AlertData();
        lastArrows = 0;
        lastSignalBars = 0;
        lastActiveBands = 0;
        dirty.resetTracking = false;
    }
    
    bool needsRedraw = false;
    
    // Frequency tolerance for V1 jitter (V1 can report ±1-3 MHz variation between packets)
    const uint32_t FREQ_TOLERANCE_MHZ = 5;
    auto freqDifferent = [FREQ_TOLERANCE_MHZ](uint32_t a, uint32_t b) -> bool {
        uint32_t diff = (a > b) ? (a - b) : (b - a);
        return diff > FREQ_TOLERANCE_MHZ;
    };
    
    // Always redraw on first run, entering live mode, or when transitioning from persisted mode
    if (firstRun) { needsRedraw = true; firstRun = false; }
    else if (enteringLiveMode) { needsRedraw = true; }
    else if (wasPersistedMode) { needsRedraw = true; }
    // V1 is source of truth - always redraw when priority alert changes
    // Use frequency tolerance to avoid full redraws from V1 jitter
    else if (freqDifferent(priority.frequency, lastPriority.frequency)) { needsRedraw = true; }
    else if (priority.band != lastPriority.band) { needsRedraw = true; }
    else if (state.muted != lastMultiState.muted) { needsRedraw = true; }
    // Note: bogey counter changes are handled via incremental update (bogeyCounterChanged) for rapid response
    
    // Also check if any secondary alert changed (set-based, not order-based)
    // V1 may reorder alerts by signal strength - we only care if the SET of alerts changed
    bool secondarySetChanged = false;
    if (!needsRedraw) {
        // Compare counts first
        int lastAlertCount = 0;
        for (int i = 0; i < PacketParser::MAX_ALERTS; i++) {
            if (lastSecondary[i].band != BAND_NONE) lastAlertCount++;
        }
        if (alertCount != lastAlertCount) {
            needsRedraw = true;
            secondarySetChanged = true;
        } else {
            // Check if any current alert is NOT in last set (set membership test)
            // Use frequency tolerance (±5 MHz) to handle V1 jitter
            const uint32_t FREQ_TOLERANCE_MHZ = 5;
            for (int i = 0; i < alertCount && i < PacketParser::MAX_ALERTS && !needsRedraw; i++) {
                bool foundInLast = false;
                for (int j = 0; j < PacketParser::MAX_ALERTS; j++) {
                    if (allAlerts[i].band == lastSecondary[j].band) {
                        if (allAlerts[i].band == BAND_LASER) {
                            foundInLast = true;
                        } else {
                            uint32_t diff = (allAlerts[i].frequency > lastSecondary[j].frequency) 
                                ? (allAlerts[i].frequency - lastSecondary[j].frequency) 
                                : (lastSecondary[j].frequency - allAlerts[i].frequency);
                            if (diff <= FREQ_TOLERANCE_MHZ) foundInLast = true;
                        }
                        if (foundInLast) break;
                    }
                }
                if (!foundInLast) {
                    needsRedraw = true;
                    secondarySetChanged = true;
                }
            }
        }
    }
    
    // Track arrow, signal bar, and band changes separately for incremental update
    // Arrow display depends on per-profile priorityArrowOnly setting
    // When priorityArrowOnly is enabled, still respect V1's arrow blinking by masking with state.arrows
    // V1 handles blinking by toggling image1 arrow bits - we follow that
    Direction arrowsToShow;
    if (settingsManager.getSlotPriorityArrowOnly(s.activeSlot)) {
        // Show priority arrow only when V1 is also showing that direction
        arrowsToShow = static_cast<Direction>(state.priorityArrow & state.arrows);
    } else {
        arrowsToShow = state.arrows;
    }
    bool arrowsChanged = (arrowsToShow != lastArrows);
    bool signalBarsChanged = (state.signalBars != lastSignalBars);
    bool bandsChanged = (state.activeBands != lastActiveBands);
    bool bogeyCounterChanged = (state.bogeyCounterByte != lastBogeyByte);

    // Volume tracking
    static uint8_t lastMainVol = 255;
    static uint8_t lastMuteVol = 255;
    bool volumeChanged = (state.mainVolume != lastMainVol || state.muteVolume != lastMuteVol);
    
    // Check if RSSI needs periodic refresh (every 2 seconds)
    unsigned long now = millis();
    bool rssiNeedsUpdate = shouldRefreshRssi(now);
    
    // Force periodic redraw when something is flashing (for blink animation)
    // Check if any arrows or bands are marked as flashing
    bool hasFlashing = (state.flashBits != 0) || (state.bandFlashBits != 0);
    static unsigned long lastFlashRedraw = 0;
    bool needsFlashUpdate = false;
    if (hasFlashing) {
        if (now - lastFlashRedraw >= 75) {  // Redraw at ~13Hz for smoother blink
            needsFlashUpdate = true;
            lastFlashRedraw = now;
        }
    }
    
    if (!needsRedraw && !arrowsChanged && !signalBarsChanged && !bandsChanged && !needsFlashUpdate && !volumeChanged && !bogeyCounterChanged && !rssiNeedsUpdate) {
        // Nothing changed on main display, but still process cards for expiration
        drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
        if (secondaryCardsRenderDirty_) {
            perfRecordDisplayRenderPath(PerfDisplayRenderPath::CardsOnly);
            flushRegion(DisplayLayout::CONTENT_LEFT_MARGIN,
                        SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT,
                        DisplayLayout::CONTENT_AVAILABLE_WIDTH,
                        SECONDARY_ROW_HEIGHT);
        }
        return;
    }
    
    if (!needsRedraw && (arrowsChanged || signalBarsChanged || bandsChanged || needsFlashUpdate || volumeChanged || bogeyCounterChanged || rssiNeedsUpdate)) {
        perfRecordDisplayRenderPath(PerfDisplayRenderPath::Incremental);
        // Only arrows, signal bars, bands, or bogey count changed - do incremental update without full redraw
        // Also handle flash updates (periodic redraw for blink animation)
        bool flushLeftStrip = false;
        bool flushRightStrip = false;

        if (arrowsChanged || (needsFlashUpdate && state.flashBits != 0)) {
            lastArrows = arrowsToShow;
            drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
            flushRightStrip = true;
        }
        if (signalBarsChanged) {
            lastSignalBars = state.signalBars;
            drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
            flushRightStrip = true;
        }
        if (bandsChanged || (needsFlashUpdate && state.bandFlashBits != 0)) {
            lastActiveBands = state.activeBands;
            drawBandIndicators(state.activeBands, state.muted, state.bandFlashBits);
            flushLeftStrip = true;
        }
        updateStatusStripIncremental(state,
                                     liveTopCounterChar,
                                     state.muted,
                                     liveTopCounterDot,
                                     volumeChanged,
                                     rssiNeedsUpdate,
                                     bogeyCounterChanged,
                                     lastMainVol,
                                     lastMuteVol,
                                     lastBogeyByte,
                                     now,
                                     flushLeftStrip,
                                     flushRightStrip);
        // Still process cards so they can expire and be cleared
        drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
        (void)flushLeftStrip;
        (void)flushRightStrip;
        DISPLAY_FLUSH();
        return;
    }

    perfRecordDisplayRenderPath(liveRenderPathForScenario());
    recordDisplayRedrawReasonIf(firstRun, PerfDisplayRedrawReason::FirstRun);
    recordDisplayRedrawReasonIf(enteringLiveMode, PerfDisplayRedrawReason::EnterLive);
    recordDisplayRedrawReasonIf(wasPersistedMode, PerfDisplayRedrawReason::LeavePersisted);
    recordDisplayRedrawReasonIf(freqDifferent(priority.frequency, lastPriority.frequency),
                                PerfDisplayRedrawReason::FrequencyChange);
    recordDisplayRedrawReasonIf(priority.band != lastPriority.band,
                                PerfDisplayRedrawReason::BandSetChange);
    recordDisplayRedrawReasonIf(arrowsChanged, PerfDisplayRedrawReason::ArrowChange);
    recordDisplayRedrawReasonIf(signalBarsChanged,
                                PerfDisplayRedrawReason::SignalBarChange);
    recordDisplayRedrawReasonIf(volumeChanged, PerfDisplayRedrawReason::VolumeChange);
    recordDisplayRedrawReasonIf(bogeyCounterChanged,
                                PerfDisplayRedrawReason::BogeyCounterChange);
    recordDisplayRedrawReasonIf(rssiNeedsUpdate, PerfDisplayRedrawReason::RssiRefresh);
    recordDisplayRedrawReasonIf(needsFlashUpdate, PerfDisplayRedrawReason::FlashTick);
    recordDisplayRedrawReasonIf(requestedTrackingReset ||
                                    secondarySetChanged ||
                                    (state.muted != lastMultiState.muted),
                                PerfDisplayRedrawReason::ForceRedraw);

    // Full redraw needed - store current state for next comparison
    lastPriority = priority;
    lastBogeyByte = state.bogeyCounterByte;
    lastMultiState = state;
    // Use same arrowsToShow logic as computed above for change detection
    lastArrows = arrowsToShow;
    lastSignalBars = state.signalBars;
    lastActiveBands = state.activeBands;
    lastMainVol = state.mainVolume;
    lastMuteVol = state.muteVolume;
    markRssiRefreshed(now);  // Reset RSSI timer on full redraw
    // Store all alerts for change detection (V1 supports up to 15)
    // We only display primary + 2 cards, but track all for accurate change detection
    for (int i = 0; i < PacketParser::MAX_ALERTS; i++) {
        lastSecondary[i] = (i < alertCount) ? allAlerts[i] : AlertData();
    }
    
    DISP_PERF_START();
    uint32_t stageStartUs = micros();
    drawBaseFrame();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BaseFrame,
                                      micros() - stageStartUs);
    DISP_PERF_LOG("drawBaseFrame");

    // V1 is source of truth - use activeBands directly (allows blinking)
    uint8_t bandMask = state.activeBands;
    
    // Bogey counter - use V1's decoded byte (shows J=Junk, P=Photo, volume, etc.)
    stageStartUs = micros();
    drawStatusStrip(state, liveTopCounterChar, state.muted, liveTopCounterDot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::StatusStrip,
                                      micros() - stageStartUs);
    DISP_PERF_LOG("counters+vol");
    
    // Main alert display (frequency, bands, arrows, signal bars)
    // Use state.signalBars which is the MAX across ALL alerts (calculated in packet_parser)
    const bool isPhotoRadar =
        (priority.photoType != 0) ||
        state.hasPhotoAlert ||
        (liveTopCounterChar == 'P');
    stageStartUs = micros();
    drawFrequency(priority.frequency, priority.band, state.muted, isPhotoRadar);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Frequency,
                                      micros() - stageStartUs);
    DISP_PERF_LOG("drawFrequency");
    stageStartUs = micros();
    drawBandIndicators(bandMask, state.muted, state.bandFlashBits);
    drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);
    DISP_PERF_LOG("bands+bars");
    
    // Arrow display: use priority arrow only if setting enabled, otherwise all V1 arrows
    // (arrowsToShow already computed above for change detection)
    stageStartUs = micros();
    drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
    drawMuteIcon(state.muted);
    drawLockoutIndicator();
    drawGpsIndicator();
    drawObdIndicator();
    drawProfileIndicator(currentProfileSlot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::ArrowsIcons,
                                      micros() - stageStartUs);
    DISP_PERF_LOG("arrows+icons");
    
    // Force card redraw since drawBaseFrame cleared the screen
    dirty.cards = true;
    
    // Draw secondary alert cards at bottom
    stageStartUs = micros();
    drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Cards,
                                      micros() - stageStartUs);
    DISP_PERF_LOG("cards");
    
    // Keep dirty.multiAlert true while in multi-alert - only reset when going to single-alert mode

    stageStartUs = micros();
    DISPLAY_FLUSH();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Flush,
                                      micros() - stageStartUs);
    DISP_PERF_LOG("flush");

    lastAlert = priority;
    lastState = state;
}

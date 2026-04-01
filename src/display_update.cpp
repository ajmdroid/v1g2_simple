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

// Singleton-scoped render cache for the single production display path.
// The V1Display API remains object-shaped, but the hot-path renderer still
// shares one cache until a larger per-instance refactor is justified.
struct DisplayRenderCache {
    static constexpr unsigned long RSSI_UPDATE_INTERVAL_MS = 2000;

    bool rightStripCleared = false;

    bool restingFirstUpdate = true;
    bool restingWasInFlashPeriod = false;
    unsigned long restingBandLastSeen[4] = {0, 0, 0, 0};  // L, Ka, K, X
    uint8_t lastRestingDebouncedBands = 0;
    uint8_t lastRestingSignalBars = 0;
    uint8_t lastRestingArrows = 0;
    uint8_t lastRestingMainVol = 255;
    uint8_t lastRestingMuteVol = 255;
    uint8_t lastRestingBogeyByte = 0;

    bool badgeCacheValid = false;
    bool lastGpsShown = false;
    uint8_t lastGpsSats = 0;
    bool lastObdShown = false;
    bool lastObdConnected = false;

    AlertData liveLastPriority{};
    uint8_t liveLastBogeyByte = 0;
    DisplayState liveLastMultiState{};
    bool liveFirstRun = true;
    AlertData liveLastSecondary[PacketParser::MAX_ALERTS]{};
    uint8_t liveLastArrows = 0;
    uint8_t liveLastSignalBars = 0;
    uint8_t liveLastActiveBands = 0;
    uint8_t liveLastMainVol = 255;
    uint8_t liveLastMuteVol = 255;
    unsigned long liveLastFlashRedraw = 0;

    unsigned long lastRssiUpdateMs = 0;

    void resetRestingTracking() {
        restingFirstUpdate = true;
        lastRestingDebouncedBands = 0;
        lastRestingSignalBars = 0;
        lastRestingArrows = 0;
        lastRestingMainVol = 255;
        lastRestingMuteVol = 255;
        lastRestingBogeyByte = 0;
        lastRssiUpdateMs = 0;
    }

    void resetLiveTracking() {
        liveLastPriority = AlertData();
        liveLastBogeyByte = 0;
        liveLastMultiState = DisplayState();
        liveFirstRun = true;
        for (int i = 0; i < PacketParser::MAX_ALERTS; ++i) {
            liveLastSecondary[i] = AlertData();
        }
        liveLastArrows = 0;
        liveLastSignalBars = 0;
        liveLastActiveBands = 0;
    }
};

DisplayRenderCache s_displayRenderCache;

inline bool shouldRefreshRssi(unsigned long nowMs) {
    return (nowMs - s_displayRenderCache.lastRssiUpdateMs) >=
           DisplayRenderCache::RSSI_UPDATE_INTERVAL_MS;
}

inline void markRssiRefreshed(unsigned long nowMs) {
    s_displayRenderCache.lastRssiUpdateMs = nowMs;
}

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
    DisplayRenderCache& cache = s_displayRenderCache;
    syncTopIndicators(millis());
    drawTopCounter(topChar, topMuted, topDot);
    const V1Settings& s = settingsManager.get();
    const bool showVolumeAndRssi = state.supportsVolume() && !s.hideVolumeIndicator;
    if (showVolumeAndRssi) {
        drawVolumeIndicator(state.mainVolume, state.muteVolume);
        drawRssiIndicator(bleCtx_.v1Rssi);
        cache.rightStripCleared = false;
    } else {
        // Clear once on transition into hidden/unsupported state.
        if (!cache.rightStripCleared) {
            FILL_RECT(8, 75, 75, 68, PALETTE_BG);
            cache.rightStripCleared = true;
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

    drawGpsIndicator();
    drawObdIndicator();
}

void V1Display::update(const DisplayState& state) {
    DisplayRenderCache& cache = s_displayRenderCache;

    // Track if we're transitioning FROM persisted mode (need full redraw)
    bool wasPersistedMode = persistedMode_;
    persistedMode_ = false;  // Not in persisted mode
    const bool requestedTrackingReset = dirty.resetTracking;

    // Don't process resting update if we're in Scanning mode - wait for showResting() to be called
    if (currentScreen_ == ScreenMode::Scanning) {
        return;
    }

    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    multiAlertMode_ = false;  // No cards to draw in resting state

    // Check if profile flash period just expired (needs redraw to clear)
    bool inFlashPeriod = (millis() - profileChangedTime_) < HIDE_TIMEOUT_MS;
    bool flashJustExpired = cache.restingWasInFlashPeriod && !inFlashPeriod;
    cache.restingWasInFlashPeriod = inFlashPeriod;

    // Band debouncing: keep bands visible for a short grace period to prevent flicker
    uint8_t restingDebouncedBands = 0;
    const unsigned long BAND_GRACE_MS = 100;  // Reduced from 200ms for snappier response
    unsigned long now = millis();

    if (state.activeBands & BAND_LASER) cache.restingBandLastSeen[0] = now;
    if (state.activeBands & BAND_KA)    cache.restingBandLastSeen[1] = now;
    if (state.activeBands & BAND_K)     cache.restingBandLastSeen[2] = now;
    if (state.activeBands & BAND_X)     cache.restingBandLastSeen[3] = now;

    restingDebouncedBands = state.activeBands;
    if ((now - cache.restingBandLastSeen[0]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_LASER;
    if ((now - cache.restingBandLastSeen[1]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_KA;
    if ((now - cache.restingBandLastSeen[2]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_K;
    if ((now - cache.restingBandLastSeen[3]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_X;

    // In resting mode (no alerts), never show muted visual - just normal display
    // Apps commonly set main volume to 0 when idle, adjusting on new alerts
    // The muted state should only affect active alert display, not resting
    bool effectiveMuted = false;

    // Reset resting statics when change tracking reset is requested (on V1 disconnect)
    if (dirty.resetTracking) {
        cache.resetRestingTracking();
        // Don't clear the flag here - let the alert update() clear it
    }

    // Check if RSSI needs periodic refresh (every 2 seconds)
    bool rssiNeedsUpdate = shouldRefreshRssi(now);

    // Check if transitioning from a non-resting visual mode.
    bool leavingLiveMode = (currentScreen_ == ScreenMode::Live);

    // Separate full redraw triggers from incremental updates
    bool needsFullRedraw =
        cache.restingFirstUpdate ||
        flashJustExpired ||
        wasPersistedMode ||  // Force full redraw when leaving persisted mode
        leavingLiveMode ||   // Force full redraw when alerts end (clear cards/frequency)
        restingDebouncedBands != cache.lastRestingDebouncedBands ||
        effectiveMuted != lastState_.muted;

    bool arrowsChanged = (state.arrows != cache.lastRestingArrows);
    bool signalBarsChanged = (state.signalBars != cache.lastRestingSignalBars);
    bool volumeChanged = (state.mainVolume != cache.lastRestingMainVol ||
                          state.muteVolume != cache.lastRestingMuteVol);
    bool bogeyCounterChanged = (state.bogeyCounterByte != cache.lastRestingBogeyByte);

    // Check if volume zero warning needs a flashing redraw
    const bool bleContextFresh = hasFreshBleContext(now);
    bool currentProxyConnected = bleCtx_.proxyConnected;
    bool volZero = (state.mainVolume == 0 && state.hasVolumeData);
    if (!bleContextFresh) {
        volZeroWarn.reset();
    } else if (volZeroWarn.needsFlashRedraw(volZero, currentProxyConnected, speedVolZeroActive_)) {
        needsFullRedraw = true;
    }

    if (!needsFullRedraw && !arrowsChanged && !signalBarsChanged && !volumeChanged && !bogeyCounterChanged && !rssiNeedsUpdate) {
        return;
    }

    if (!needsFullRedraw && (arrowsChanged || signalBarsChanged || volumeChanged || bogeyCounterChanged || rssiNeedsUpdate)) {
        perfRecordDisplayRenderPath(PerfDisplayRenderPath::RestingIncremental);
        // Incremental update - only redraw what changed
        bool flushLeftStrip = false;
        bool flushRightStrip = false;

        if (arrowsChanged) {
            cache.lastRestingArrows = state.arrows;
            drawDirectionArrow(state.arrows, effectiveMuted, state.flashBits);
            flushRightStrip = true;
        }
        if (signalBarsChanged) {
            cache.lastRestingSignalBars = state.signalBars;
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
                                     cache.lastRestingMainVol,
                                     cache.lastRestingMuteVol,
                                     cache.lastRestingBogeyByte,
                                     now,
                                     flushLeftStrip,
                                     flushRightStrip);
        (void)flushLeftStrip;
        (void)flushRightStrip;
        DISPLAY_FLUSH();
        lastState_ = state;
        return;
    }

    perfRecordDisplayRenderPath(restingRenderPathForScenario());
    recordDisplayRedrawReasonIf(cache.restingFirstUpdate, PerfDisplayRedrawReason::FirstRun);
    recordDisplayRedrawReasonIf(wasPersistedMode, PerfDisplayRedrawReason::LeavePersisted);
    recordDisplayRedrawReasonIf(leavingLiveMode, PerfDisplayRedrawReason::LeaveLive);
    recordDisplayRedrawReasonIf(restingDebouncedBands != cache.lastRestingDebouncedBands,
                                PerfDisplayRedrawReason::BandSetChange);
    recordDisplayRedrawReasonIf(arrowsChanged, PerfDisplayRedrawReason::ArrowChange);
    recordDisplayRedrawReasonIf(signalBarsChanged, PerfDisplayRedrawReason::SignalBarChange);
    recordDisplayRedrawReasonIf(volumeChanged, PerfDisplayRedrawReason::VolumeChange);
    recordDisplayRedrawReasonIf(bogeyCounterChanged,
                                PerfDisplayRedrawReason::BogeyCounterChange);
    recordDisplayRedrawReasonIf(rssiNeedsUpdate, PerfDisplayRedrawReason::RssiRefresh);
    recordDisplayRedrawReasonIf(flashJustExpired, PerfDisplayRedrawReason::FlashTick);
    recordDisplayRedrawReasonIf(requestedTrackingReset || (effectiveMuted != lastState_.muted),
                                PerfDisplayRedrawReason::ForceRedraw);

    // Full redraw needed
    cache.restingFirstUpdate = false;
    cache.lastRestingDebouncedBands = restingDebouncedBands;
    cache.lastRestingArrows = state.arrows;
    cache.lastRestingSignalBars = state.signalBars;
    cache.lastRestingMainVol = state.mainVolume;
    cache.lastRestingMuteVol = state.muteVolume;
    cache.lastRestingBogeyByte = state.bogeyCounterByte;
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
    bool showVolumeWarning = false;
    if (!bleContextFresh) {
        volZeroWarn.reset();
    } else {
        showVolumeWarning = volZeroWarn.evaluate(
            volZero, proxyConnected, speedVolZeroActive_, play_vol0_beep);
    }

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
    drawGpsIndicator();
    drawObdIndicator();
    drawProfileIndicator(currentProfileSlot_);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::ArrowsIcons,
                                      micros() - stageStartUs);

    // Clear any persisted card slots when entering resting state
    AlertData emptyPriority;
    stageStartUs = micros();
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, effectiveMuted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Cards,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    DISPLAY_FLUSH();  // Push canvas to display
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Flush,
                                      micros() - stageStartUs);

    dirty.resetTracking = false;

    if (currentScreen_ != ScreenMode::Resting) {
        perfRecordDisplayScreenTransition(
            perfScreenForMode(currentScreen_),
            PerfDisplayScreen::Resting,
            millis());
    }
    currentScreen_ = ScreenMode::Resting;  // Set screen mode after redraw complete
    lastState_ = state;
}

void V1Display::refreshFrequencyOnly(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    DisplayRenderCache& cache = s_displayRenderCache;

    drawFrequency(freqMHz, band, muted, isPhotoRadar);

    const uint32_t nowMs = millis();
    syncTopIndicators(nowMs);

    const bool gpsShow = gpsSatEnabled_ && gpsSatHasFix_;
    const uint8_t gpsSats = gpsShow ? gpsSatCount_ : 0;
    const bool obdShow = obdEnabled_;
    const bool obdConnected = obdShow && obdConnected_;

    const bool forceBadgeFlush = dirty.gpsIndicator || dirty.obdIndicator;

    drawGpsIndicator();
    drawObdIndicator();

    const bool badgeStripChanged =
        !cache.badgeCacheValid ||
        forceBadgeFlush ||
        (gpsShow != cache.lastGpsShown) ||
        (gpsSats != cache.lastGpsSats) ||
        (obdShow != cache.lastObdShown) ||
        (obdConnected != cache.lastObdConnected);

    if (badgeStripChanged) {
        // Top status strip containing GPS / OBD badges.
        flushRegion(120, 0, 320, 36);
        cache.badgeCacheValid = true;
        cache.lastGpsShown = gpsShow;
        cache.lastGpsSats = gpsSats;
        cache.lastObdShown = obdShow;
        cache.lastObdConnected = obdConnected;
    }

    if (frequencyRenderDirty_) {
        if (frequencyDirtyValid_) {
            flushRegion(frequencyDirtyX_, frequencyDirtyY_, frequencyDirtyW_, frequencyDirtyH_);
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
        persistedMode_ = false;
        update(state);  // Fall back to normal resting display
        return;
    }

    // Enable persisted mode so draw functions use PALETTE_PERSISTED instead of PALETTE_MUTED
    persistedMode_ = true;

    if (currentScreen_ != ScreenMode::Persisted) {
        perfRecordDisplayScreenTransition(
            perfScreenForMode(currentScreen_),
            PerfDisplayScreen::Persisted,
            millis());
    }
    currentScreen_ = ScreenMode::Persisted;
    perfRecordDisplayRenderPath(persistedRenderPathForScenario());

    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    multiAlertMode_ = false;  // No cards to draw
    wasInMultiAlertMode_ = false;

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
    drawProfileIndicator(currentProfileSlot_);
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
    DisplayRenderCache& cache = s_displayRenderCache;

    // Check if we're transitioning FROM persisted mode (need full redraw to restore colors)
    bool wasPersistedMode = persistedMode_;
    persistedMode_ = false;  // Not in persisted mode
    const bool requestedTrackingReset = dirty.resetTracking;

    // Get settings reference for priorityArrowOnly
    const V1Settings& s = settingsManager.get();

    // If no valid priority alert, return (caller should use updatePersisted or update(state) instead)
    if (!priority.isValid || priority.band == BAND_NONE) {
        PERF_INC(displayLiveInvalidPrioritySkips);
        return;
    }

    // Track screen mode transitions - force redraw when entering live mode from resting/scanning
    bool enteringLiveMode = (currentScreen_ != ScreenMode::Live);
    if (enteringLiveMode) {
        DISPLAY_LOG("[DISP] Entering Live mode (was %d), alertCount=%d\n",
                    (int)currentScreen_, alertCount);
        perfRecordDisplayScreenTransition(
            perfScreenForMode(currentScreen_),
            PerfDisplayScreen::Live,
            millis());
    }
    currentScreen_ = ScreenMode::Live;

    // Always use multi-alert mode (raised layout for cards)
    dirty.multiAlert = true;
    multiAlertMode_ = true;

    // V1 is source of truth - use activeBands directly, no debouncing
    // This allows V1's native blinking to come through

    char liveTopCounterChar = state.bogeyCounterChar;
    bool liveTopCounterDot = state.bogeyCounterDot;

    // Check if reset was requested (e.g., on V1 disconnect)
    if (dirty.resetTracking) {
        cache.resetLiveTracking();
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
    if (cache.liveFirstRun) { needsRedraw = true; cache.liveFirstRun = false; }
    else if (enteringLiveMode) { needsRedraw = true; }
    else if (wasPersistedMode) { needsRedraw = true; }
    // V1 is source of truth - always redraw when priority alert changes
    // Frequency-only changes are handled via incremental path (freqOnlyChanged below)
    else if (priority.band != cache.liveLastPriority.band) { needsRedraw = true; }
    else if (state.muted != cache.liveLastMultiState.muted) { needsRedraw = true; }
    // Note: bogey counter changes are handled via incremental update (bogeyCounterChanged) for rapid response

    // Also check if any secondary alert changed (set-based, not order-based)
    // V1 may reorder alerts by signal strength - we only care if the SET of alerts changed
    bool secondarySetChanged = false;
    if (!needsRedraw) {
        // Compare counts first
        int lastAlertCount = 0;
        for (int i = 0; i < PacketParser::MAX_ALERTS; i++) {
            if (cache.liveLastSecondary[i].band != BAND_NONE) lastAlertCount++;
        }
        if (alertCount != lastAlertCount) {
            needsRedraw = true;
            secondarySetChanged = true;
        } else {
            // Check if any current alert is NOT in last set (set membership test)
            // Use frequency tolerance (±5 MHz) to handle V1 jitter
            for (int i = 0; i < alertCount && i < PacketParser::MAX_ALERTS && !needsRedraw; i++) {
                bool foundInLast = false;
                for (int j = 0; j < PacketParser::MAX_ALERTS; j++) {
                    if (allAlerts[i].band == cache.liveLastSecondary[j].band) {
                        if (allAlerts[i].band == BAND_LASER) {
                            foundInLast = true;
                        } else {
                            uint32_t diff =
                                (allAlerts[i].frequency > cache.liveLastSecondary[j].frequency)
                                    ? (allAlerts[i].frequency - cache.liveLastSecondary[j].frequency)
                                    : (cache.liveLastSecondary[j].frequency - allAlerts[i].frequency);
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
    bool arrowsChanged = (arrowsToShow != cache.liveLastArrows);
    bool signalBarsChanged = (state.signalBars != cache.liveLastSignalBars);
    bool bandsChanged = (state.activeBands != cache.liveLastActiveBands);
    bool bogeyCounterChanged = (state.bogeyCounterByte != cache.liveLastBogeyByte);

    // Volume tracking
    bool volumeChanged =
        (state.mainVolume != cache.liveLastMainVol || state.muteVolume != cache.liveLastMuteVol);

    // Check if RSSI needs periodic refresh (every 2 seconds)
    unsigned long now = millis();
    bool rssiNeedsUpdate = shouldRefreshRssi(now);

    // Force periodic redraw when something is flashing (for blink animation)
    // Check if any arrows or bands are marked as flashing
    bool hasFlashing = (state.flashBits != 0) || (state.bandFlashBits != 0);
    bool needsFlashUpdate = false;
    if (hasFlashing) {
        if (now - cache.liveLastFlashRedraw >= 75) {  // Redraw at ~13Hz for smoother blink
            needsFlashUpdate = true;
            cache.liveLastFlashRedraw = now;
        }
    }

    // Frequency-only change: no fillScreen or full subphase redraw needed.
    // drawFrequency() self-clears its bounding box so no ghost pixels.
    const bool freqOnlyChanged = !needsRedraw &&
        freqDifferent(priority.frequency, cache.liveLastPriority.frequency);

    if (!needsRedraw && !freqOnlyChanged && !arrowsChanged && !signalBarsChanged && !bandsChanged && !needsFlashUpdate && !volumeChanged && !bogeyCounterChanged && !rssiNeedsUpdate) {
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

    if (!needsRedraw && (freqOnlyChanged || arrowsChanged || signalBarsChanged || bandsChanged || needsFlashUpdate || volumeChanged || bogeyCounterChanged || rssiNeedsUpdate)) {
        perfRecordDisplayRenderPath(PerfDisplayRenderPath::Incremental);
        // Incremental update without full redraw — only redraw changed elements.
        bool flushLeftStrip = false;
        bool flushRightStrip = false;

        if (freqOnlyChanged) {
            cache.liveLastPriority.frequency = priority.frequency;
            const bool isPhotoRadar =
                (priority.photoType != 0) ||
                state.hasPhotoAlert ||
                (liveTopCounterChar == 'P');
            drawFrequency(priority.frequency, priority.band, state.muted, isPhotoRadar);
        }

        if (arrowsChanged || (needsFlashUpdate && state.flashBits != 0)) {
            cache.liveLastArrows = arrowsToShow;
            drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
            flushRightStrip = true;
        }
        if (signalBarsChanged) {
            cache.liveLastSignalBars = state.signalBars;
            drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
            flushRightStrip = true;
        }
        if (bandsChanged || (needsFlashUpdate && state.bandFlashBits != 0)) {
            cache.liveLastActiveBands = state.activeBands;
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
                                     cache.liveLastMainVol,
                                     cache.liveLastMuteVol,
                                     cache.liveLastBogeyByte,
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
    recordDisplayRedrawReasonIf(cache.liveFirstRun, PerfDisplayRedrawReason::FirstRun);
    recordDisplayRedrawReasonIf(enteringLiveMode, PerfDisplayRedrawReason::EnterLive);
    recordDisplayRedrawReasonIf(wasPersistedMode, PerfDisplayRedrawReason::LeavePersisted);
    recordDisplayRedrawReasonIf(priority.band != cache.liveLastPriority.band,
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
                                    (state.muted != cache.liveLastMultiState.muted),
                                PerfDisplayRedrawReason::ForceRedraw);

    // Full redraw needed - store current state for next comparison
    cache.liveLastPriority = priority;
    cache.liveLastBogeyByte = state.bogeyCounterByte;
    cache.liveLastMultiState = state;
    // Use same arrowsToShow logic as computed above for change detection
    cache.liveLastArrows = arrowsToShow;
    cache.liveLastSignalBars = state.signalBars;
    cache.liveLastActiveBands = state.activeBands;
    cache.liveLastMainVol = state.mainVolume;
    cache.liveLastMuteVol = state.muteVolume;
    markRssiRefreshed(now);  // Reset RSSI timer on full redraw
    // Store all alerts for change detection (V1 supports up to 15)
    // We only display primary + 2 cards, but track all for accurate change detection
    for (int i = 0; i < PacketParser::MAX_ALERTS; i++) {
        cache.liveLastSecondary[i] = (i < alertCount) ? allAlerts[i] : AlertData();
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
    drawGpsIndicator();
    drawObdIndicator();
    drawProfileIndicator(currentProfileSlot_);
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

    lastAlert_ = priority;
    lastState_ = state;
}

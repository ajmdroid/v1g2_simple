/**
 * Display update methods — three render functions, one cache layer.
 *
 * Contains update(DisplayState), update(AlertData, ...), updatePersisted.
 *
 * The element caches (g_elementCaches) are the sole caching layer. Each draw
 * function checks "did my inputs change?" and skips the draw if not. Mode
 * transitions invalidate all element caches via prepareFullRedrawNoClear().
 */

#include "display.h"
#include "../include/display_layout.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_element_caches.h"
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

}  // namespace

// ============================================================================
// drawStatusStrip — full status strip render
// ============================================================================

void V1Display::drawStatusStrip(const DisplayState& state,
                                char topChar,
                                bool topMuted,
                                bool topDot) {
    syncTopIndicators(millis());
    drawTopCounter(topChar, topMuted, topDot);
    const V1Settings& s = settingsManager.get();
    const bool showVolumeAndRssi = state.supportsVolume() && !s.hideVolumeIndicator;
    if (showVolumeAndRssi) {
        drawVolumeIndicator(state.mainVolume, state.muteVolume);
        drawRssiIndicator(bleCtx_.v1Rssi);
    }
    drawWiFiIndicator();
    drawBatteryIndicator();
    drawBLEProxyIndicator();
    drawObdIndicator();
    drawAlpIndicator();
    drawMuteIcon(topMuted);
    drawProfileIndicator(currentProfileSlot_);
}

// ============================================================================
// update(DisplayState) — Resting display (no active alerts)
// ============================================================================

void V1Display::update(const DisplayState& state) {
    // Not in persisted mode
    persistedMode_ = false;

    // Don't process resting update if we're in Scanning mode
    if (currentScreen_ == ScreenMode::Scanning) {
        return;
    }

    // Mode transition → full redraw via element cache invalidation
    if (currentScreen_ != ScreenMode::Resting) {
        perfRecordDisplayScreenTransition(
            perfScreenForMode(currentScreen_),
            PerfDisplayScreen::Resting,
            millis());
    }

    perfRecordDisplayRenderPath(restingRenderPathForScenario());

    // In resting mode, never show muted visual — apps commonly set volume to 0
    // when idle, adjusting on new alerts.
    const bool effectiveMuted = false;

    const bool bleContextFresh = hasFreshBleContext(millis());

    // Volume-zero warning state machine
    bool showVolumeWarning = false;
    if (!bleContextFresh) {
        volZeroWarn.reset();
    } else {
        const bool volZero = (state.mainVolume == 0 && state.hasVolumeData);
        const bool proxyConnected = bleCtx_.proxyConnected;
        showVolumeWarning = volZeroWarn.evaluate(
            volZero, proxyConnected, speedVolZeroActive_, play_vol0_beep);
    }

    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    multiAlertMode_ = false;

    uint32_t stageStartUs = micros();
    drawBaseFrame();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BaseFrame,
                                      micros() - stageStartUs);

    char topChar = state.bogeyCounterChar;
    stageStartUs = micros();
    drawStatusStrip(state, topChar, effectiveMuted, state.bogeyCounterDot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::StatusStrip,
                                      micros() - stageStartUs);

    drawBandIndicators(state.activeBands, effectiveMuted);

    // Volume-zero warning replaces frequency display
    if (showVolumeWarning) {
        drawVolumeZeroWarning();
    } else {
        stageStartUs = micros();
        drawFrequency(0, BAND_NONE, effectiveMuted);
        perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Frequency,
                                          micros() - stageStartUs);
    }

    stageStartUs = micros();
    drawVerticalSignalBars(state.signalBars, state.signalBars, BAND_KA, effectiveMuted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    drawDirectionArrow(DIR_NONE, effectiveMuted, 0);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::ArrowsIcons,
                                      micros() - stageStartUs);

    // Clear any persisted card slots
    AlertData emptyPriority;
    stageStartUs = micros();
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, effectiveMuted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Cards,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    DISPLAY_FLUSH();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Flush,
                                      micros() - stageStartUs);

    dirty.resetTracking = false;
    currentScreen_ = ScreenMode::Resting;
    lastState_ = state;
}

// ============================================================================
// updatePersisted — last alert held in dark grey
// ============================================================================

void V1Display::updatePersisted(const AlertData& alert, const DisplayState& state) {
    if (!alert.isValid) {
        persistedMode_ = false;
        update(state);
        return;
    }

    persistedMode_ = true;

    if (currentScreen_ != ScreenMode::Persisted) {
        perfRecordDisplayScreenTransition(
            perfScreenForMode(currentScreen_),
            PerfDisplayScreen::Persisted,
            millis());
    }

    perfRecordDisplayRenderPath(persistedRenderPathForScenario());

    dirty.multiAlert = true;
    multiAlertMode_ = false;

    uint32_t stageStartUs = micros();
    drawBaseFrame();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BaseFrame,
                                      micros() - stageStartUs);

    // Bogey counter shows V1's decoded display — NOT greyed, always visible
    char topChar = state.bogeyCounterChar;
    stageStartUs = micros();
    drawStatusStrip(state, topChar, false, state.bogeyCounterDot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::StatusStrip,
                                      micros() - stageStartUs);

    // Band indicator in persisted color
    stageStartUs = micros();
    drawBandIndicators(alert.band, true);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);

    // Frequency in persisted color
    const bool isPhotoRadar =
        (alert.photoType != 0) ||
        state.hasPhotoAlert ||
        (state.bogeyCounterChar == 'P');
    stageStartUs = micros();
    drawFrequency(alert.frequency, alert.band, true, isPhotoRadar);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Frequency,
                                      micros() - stageStartUs);

    // No signal bars — draw empty
    stageStartUs = micros();
    drawVerticalSignalBars(0, 0, alert.band, true);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);

    // Arrows in persisted grey
    stageStartUs = micros();
    drawDirectionArrow(alert.direction, true);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::ArrowsIcons,
                                      micros() - stageStartUs);

    // Clear card area
    AlertData emptyPriority;
    stageStartUs = micros();
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, true);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Cards,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    DISPLAY_FLUSH();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Flush,
                                      micros() - stageStartUs);

    currentScreen_ = ScreenMode::Persisted;
}

// ============================================================================
// update(priority, allAlerts, alertCount, state) — Live alert display
// ============================================================================

void V1Display::update(const AlertData& priority, const AlertData* allAlerts,
                       int alertCount, const DisplayState& state) {
    persistedMode_ = false;

    const V1Settings& s = settingsManager.get();

    if (!priority.isValid || priority.band == BAND_NONE) {
        PERF_INC(displayLiveInvalidPrioritySkips);
        return;
    }

    if (currentScreen_ != ScreenMode::Live) {
        DISPLAY_LOG("[DISP] Entering Live mode (was %d), alertCount=%d\n",
                    (int)currentScreen_, alertCount);
        perfRecordDisplayScreenTransition(
            perfScreenForMode(currentScreen_),
            PerfDisplayScreen::Live,
            millis());
    }

    perfRecordDisplayRenderPath(liveRenderPathForScenario());

    dirty.multiAlert = true;
    multiAlertMode_ = true;

    // Arrow display: priority arrow only if setting enabled, otherwise all V1 arrows
    Direction arrowsToShow;
    if (settingsManager.getSlotPriorityArrowOnly(s.activeSlot)) {
        arrowsToShow = static_cast<Direction>(state.priorityArrow & state.arrows);
    } else {
        arrowsToShow = state.arrows;
    }

    char liveTopCounterChar = state.bogeyCounterChar;
    bool liveTopCounterDot = state.bogeyCounterDot;

    uint32_t stageStartUs = micros();
    drawBaseFrame();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BaseFrame,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    drawStatusStrip(state, liveTopCounterChar, state.muted, liveTopCounterDot);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::StatusStrip,
                                      micros() - stageStartUs);

    const bool isPhotoRadar =
        (priority.photoType != 0) ||
        state.hasPhotoAlert ||
        (liveTopCounterChar == 'P');
    stageStartUs = micros();
    drawFrequency(priority.frequency, priority.band, state.muted, isPhotoRadar);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Frequency,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    drawBandIndicators(state.activeBands, state.muted, state.bandFlashBits);
    drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::BandsBars,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::ArrowsIcons,
                                      micros() - stageStartUs);

    // Force card redraw since drawBaseFrame cleared the screen
    dirty.cards = true;
    g_elementCaches.cards.invalidate();

    stageStartUs = micros();
    drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Cards,
                                      micros() - stageStartUs);

    stageStartUs = micros();
    DISPLAY_FLUSH();
    perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase::Flush,
                                      micros() - stageStartUs);

    currentScreen_ = ScreenMode::Live;
    lastAlert_ = priority;
    lastState_ = state;
}

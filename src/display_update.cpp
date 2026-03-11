/**
 * Update methods — extracted from display.cpp (Phase 3C/3D)
 *
 * Contains update(DisplayState), update(AlertData, ...), refreshFrequencyOnly,
 * refreshSecondaryAlertCards, updatePersisted, updateCameraAlert.
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
#include "modules/gps/gps_runtime_module.h"

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

void V1Display::drawStatusStrip(const DisplayState& state,
                                char topChar,
                                bool topMuted,
                                bool topDot) {
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
}

void V1Display::update(const DisplayState& state) {
    // Track if we're transitioning FROM persisted mode (need full redraw)
    bool wasPersistedMode = persistedMode;
    persistedMode = false;  // Not in persisted mode
    
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
    bool leavingCameraMode = (currentScreen == ScreenMode::Camera);
    
    // Separate full redraw triggers from incremental updates
    bool needsFullRedraw =
        firstUpdate ||
        flashJustExpired ||
        wasPersistedMode ||  // Force full redraw when leaving persisted mode
        leavingLiveMode ||   // Force full redraw when alerts end (clear cards/frequency)
        leavingCameraMode || // Force full redraw when camera owner releases frequency zone
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
            flushRegion(DisplayLayout::CONTENT_LEFT_MARGIN,
                        SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT,
                        DisplayLayout::CONTENT_AVAILABLE_WIDTH,
                        SECONDARY_ROW_HEIGHT);
        }
        return;
    }
    
    if (!needsFullRedraw && (arrowsChanged || signalBarsChanged || volumeChanged || bogeyCounterChanged || rssiNeedsUpdate)) {
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

    // Full redraw needed
    firstUpdate = false;
    lastRestingDebouncedBands = restingDebouncedBands;
    lastRestingArrows = state.arrows;
    lastRestingSignalBars = state.signalBars;
    lastRestingMainVol = state.mainVolume;
    lastRestingMuteVol = state.muteVolume;
    lastRestingBogeyByte = state.bogeyCounterByte;
    markRssiRefreshed(now);  // Reset RSSI timer on full redraw
    
    drawBaseFrame();
    // Use V1's decoded bogey counter byte - shows mode, volume, etc.
    char topChar = state.bogeyCounterChar;
    drawStatusStrip(state, topChar, effectiveMuted, state.bogeyCounterDot);
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
        drawFrequency(0, primaryBand, effectiveMuted);
    }
    
    drawVerticalSignalBars(state.signalBars, state.signalBars, primaryBand, effectiveMuted);
    // Never draw arrows in resting display - arrows should only appear in live mode
    // when we have actual alert data with frequency. If display packet has arrows but
    // no alert packet arrived, we shouldn't show arrows without frequency.
    drawDirectionArrow(DIR_NONE, effectiveMuted, 0);
    drawMuteIcon(effectiveMuted);
    drawLockoutIndicator();
    drawGpsIndicator();
    drawProfileIndicator(currentProfileSlot);
    
    // Clear any persisted card slots when entering resting state
    AlertData emptyPriority;
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, effectiveMuted);
    drawRestTelemetryCards(true);

    DISPLAY_FLUSH();  // Push canvas to display

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

    // Keep top-row badges responsive even when we're in lightweight refresh mode.
    // This path runs independently of full display.update() redraws.
    const uint32_t nowMs = millis();
    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    const bool gpsShow = gpsStatus.enabled && gpsStatus.stableHasFix;
    const uint8_t gpsSats = gpsShow ? gpsStatus.stableSatellites : 0;

    setGpsSatellites(gpsStatus.enabled, gpsStatus.stableHasFix, gpsStatus.stableSatellites);

    const bool forceBadgeFlush = dirty.lockout || dirty.gpsIndicator;

    drawLockoutIndicator();
    drawGpsIndicator();

    static bool badgeCacheValid = false;
    static bool lastLockoutShown = false;
    static bool lastGpsShown = false;
    static uint8_t lastGpsSats = 0;

    const bool badgeStripChanged =
        !badgeCacheValid ||
        forceBadgeFlush ||
        (lockoutIndicatorShown_ != lastLockoutShown) ||
        (gpsShow != lastGpsShown) ||
        (gpsSats != lastGpsSats);

    if (badgeStripChanged) {
        // Top status strip containing GPS / lockout badges.
        flushRegion(120, 0, 320, 36);
        badgeCacheValid = true;
        lastLockoutShown = lockoutIndicatorShown_;
        lastGpsShown = gpsShow;
        lastGpsSats = gpsSats;
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
    
    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    multiAlertMode = false;  // No cards to draw
    wasInMultiAlertMode = false;
    
    drawBaseFrame();
    
    // Bogey counter shows V1's decoded display - NOT greyed, always visible
    char topChar = state.bogeyCounterChar;
    drawStatusStrip(state, topChar, false, state.bogeyCounterDot);
    
    // Band indicator in persisted color
    uint8_t bandMask = alert.band;
    drawBandIndicators(bandMask, true);  // muted=true triggers PALETTE_MUTED_OR_PERSISTED
    
    // Frequency in persisted color (pass muted=true)
    const bool isPhotoRadar =
        (alert.photoType != 0) ||
        state.hasPhotoAlert ||
        (state.bogeyCounterChar == 'P');
    drawFrequency(alert.frequency, alert.band, true, isPhotoRadar);
    
    // No signal bars - just draw empty
    drawVerticalSignalBars(0, 0, alert.band, true);
    
    // Arrows in dark grey
    drawDirectionArrow(alert.direction, true);  // muted=true for grey
    
    // No mute badge
    // drawMuteIcon intentionally skipped
    
    // Profile indicator still shown
    drawProfileIndicator(currentProfileSlot);
    
    // Clear card area AND expire all tracked card slots (no cards during persisted state)
    // This prevents stale cards from reappearing when returning to live alerts
    AlertData emptyPriority;
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, true);

    DISPLAY_FLUSH();
}

void V1Display::updateCameraAlert(const CameraAlertDisplayPayload& payload,
                                  const DisplayState& state) {
    persistedMode = false;

    if (!payload.active || payload.type == CameraType::INVALID) {
        update(state);
        return;
    }

    const bool enteringCamera = (currentScreen != ScreenMode::Camera);
    if (enteringCamera) {
        perfRecordDisplayScreenTransition(
            static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
            PerfDisplayScreen::Camera,
            millis());
    }
    currentScreen = ScreenMode::Camera;

    static CameraType lastCameraType = CameraType::INVALID;
    static uint8_t lastMainVol = 0xFF;
    static uint8_t lastMuteVol = 0xFF;
    static uint8_t lastBogeyByte = 0;
    const unsigned long now = millis();
    const bool fullRedraw = enteringCamera || payload.type != lastCameraType;

    if (fullRedraw) {
        dirty.multiAlert = true;
        multiAlertMode = false;
        wasInMultiAlertMode = false;

        drawBaseFrame();

        const V1Settings& settings = settingsManager.get();
        drawStatusStrip(state, state.bogeyCounterChar, false, state.bogeyCounterDot);
        drawBandIndicators(0, false);
        drawCameraLabel(cameraTypeDisplayLabel(payload.type), settings.colorCameraText);
        drawVerticalSignalBars(0, 0, BAND_NONE, false);
        drawDirectionArrow(DIR_FRONT, false, 0, settings.colorCameraArrow);
        drawMuteIcon(false);
        drawLockoutIndicator();
        drawGpsIndicator();
        drawProfileIndicator(currentProfileSlot);

        AlertData emptyPriority;
        drawSecondaryAlertCards(nullptr, 0, emptyPriority, false);

        DISPLAY_FLUSH();
        markRssiRefreshed(now);
    } else {
        // Camera payload/owner are unchanged; keep camera mode cheap by updating
        // only dynamic strip/indicator elements instead of full-screen redraws.
        bool flushLeftStrip = false;
        bool flushRightStrip = false;
        const bool volumeChanged =
            (state.mainVolume != lastMainVol) || (state.muteVolume != lastMuteVol);
        const bool rssiNeedsUpdate = shouldRefreshRssi(now);
        const bool bogeyCounterChanged = (state.bogeyCounterByte != lastBogeyByte);
        updateStatusStripIncremental(state,
                                     state.bogeyCounterChar,
                                     false,
                                     state.bogeyCounterDot,
                                     volumeChanged,
                                     rssiNeedsUpdate,
                                     bogeyCounterChanged,
                                     lastMainVol,
                                     lastMuteVol,
                                     lastBogeyByte,
                                     now,
                                     flushLeftStrip,
                                     flushRightStrip);
        drawLockoutIndicator();
        drawGpsIndicator();
        drawProfileIndicator(currentProfileSlot);
        DISPLAY_FLUSH();
    }

    lastMainVol = state.mainVolume;
    lastMuteVol = state.muteVolume;
    lastBogeyByte = state.bogeyCounterByte;
    lastCameraType = payload.type;
    lastState = state;
}

// Multi-alert update: draws priority alert with secondary alert cards below
void V1Display::update(const AlertData& priority, const AlertData* allAlerts, int alertCount, const DisplayState& state) {
    // Check if we're transitioning FROM persisted mode (need full redraw to restore colors)
    bool wasPersistedMode = persistedMode;
    persistedMode = false;  // Not in persisted mode

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
    if (!needsRedraw) {
        // Compare counts first
        int lastAlertCount = 0;
        for (int i = 0; i < PacketParser::MAX_ALERTS; i++) {
            if (lastSecondary[i].band != BAND_NONE) lastAlertCount++;
        }
        if (alertCount != lastAlertCount) {
            needsRedraw = true;
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
            flushRegion(DisplayLayout::CONTENT_LEFT_MARGIN,
                        SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT,
                        DisplayLayout::CONTENT_AVAILABLE_WIDTH,
                        SECONDARY_ROW_HEIGHT);
        }
        return;
    }
    
    if (!needsRedraw && (arrowsChanged || signalBarsChanged || bandsChanged || needsFlashUpdate || volumeChanged || bogeyCounterChanged || rssiNeedsUpdate)) {
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
    drawBaseFrame();
    DISP_PERF_LOG("drawBaseFrame");

    // V1 is source of truth - use activeBands directly (allows blinking)
    uint8_t bandMask = state.activeBands;
    
    // Bogey counter - use V1's decoded byte (shows J=Junk, P=Photo, volume, etc.)
    drawStatusStrip(state, liveTopCounterChar, state.muted, liveTopCounterDot);
    DISP_PERF_LOG("counters+vol");
    
    // Main alert display (frequency, bands, arrows, signal bars)
    // Use state.signalBars which is the MAX across ALL alerts (calculated in packet_parser)
    const bool isPhotoRadar =
        (priority.photoType != 0) ||
        state.hasPhotoAlert ||
        (liveTopCounterChar == 'P');
    drawFrequency(priority.frequency, priority.band, state.muted, isPhotoRadar);
    DISP_PERF_LOG("drawFrequency");
    drawBandIndicators(bandMask, state.muted, state.bandFlashBits);
    drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
    DISP_PERF_LOG("bands+bars");
    
    // Arrow display: use priority arrow only if setting enabled, otherwise all V1 arrows
    // (arrowsToShow already computed above for change detection)
    drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
    drawMuteIcon(state.muted);
    drawLockoutIndicator();
    drawGpsIndicator();
    drawProfileIndicator(currentProfileSlot);
    DISP_PERF_LOG("arrows+icons");
    
    // Force card redraw since drawBaseFrame cleared the screen
    dirty.cards = true;
    
    // Draw secondary alert cards at bottom
    drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
    DISP_PERF_LOG("cards");
    
    // Keep dirty.multiAlert true while in multi-alert - only reset when going to single-alert mode

    DISPLAY_FLUSH();
    DISP_PERF_LOG("flush");

    lastAlert = priority;
    lastState = state;
}

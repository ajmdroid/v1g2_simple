/**
 * V1 Gen2 Simple Display - Main Application
 * Target: Waveshare ESP32-S3-Touch-LCD-3.49 with Valentine1 Gen2 BLE
 * 
 * Features:
 * - BLE client for V1 Gen2 radar detector
 * - BLE server proxy for JBV1 app compatibility
 * - 3.49" AMOLED display with touch support
 * - WiFi web interface for configuration
 * - 3-slot auto-push profile system
 * - Tap-to-mute functionality
 * - Alert logging and replay
 * - Multiple color themes
 * 
 * Architecture:
 * - FreeRTOS queue for BLE data handling
 * - Non-blocking display updates
 * - Persistent settings via Preferences
 * 
 * Author: Based on Valentine Research ESP protocol
 * License: MIT
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "wifi_manager.h"
#include "settings.h"
#include "touch_handler.h"
#include "v1_profiles.h"
#include "battery_manager.h"
#include "storage_manager.h"
#include "debug_logger.h"
#include "audio_beep.h"
#include "gps_handler.h"
#include "lockout_manager.h"
#include "auto_lockout_manager.h"
#include "obd_handler.h"
#include "camera_manager.h"
#include "perf_metrics.h"
#include "../include/config.h"
#define SerialLog Serial  // Alias: serial logger removed; use Serial directly
#include <FS.h>
#include <LittleFS.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Gate verbose logs behind debug switches (keep off in normal builds)
static constexpr bool DEBUG_LOGS = false;          // General debug logging (packet dumps, status)
static constexpr bool AUTOPUSH_DEBUG_LOGS = false;  // AutoPush-specific verbose logs
static constexpr bool PERF_TIMING_LOGS = false;    // Hot path timing measurements (enable for bench testing)
#define DEBUG_LOGF(...) do { if (DEBUG_LOGS) SerialLog.printf(__VA_ARGS__); } while (0)
#define DEBUG_LOGLN(msg) do { if (DEBUG_LOGS) SerialLog.println(msg); } while (0)
#define AUTO_PUSH_LOGF(...) do { if (AUTOPUSH_DEBUG_LOGS) SerialLog.printf(__VA_ARGS__); } while (0)
#define AUTO_PUSH_LOGLN(msg) do { if (AUTOPUSH_DEBUG_LOGS) SerialLog.println(msg); } while (0)

// Performance timing helpers - measure critical path durations
static unsigned long perfTimingAccum = 0;
static unsigned long perfTimingCount = 0;
static unsigned long perfTimingMax = 0;
static unsigned long perfLastReport = 0;

// Display latency tracking for SD logging
static unsigned long displayLatencySum = 0;
static unsigned long displayLatencyCount = 0;
static unsigned long displayLatencyMax = 0;
static unsigned long displayLatencyLastLog = 0;
static constexpr unsigned long DISPLAY_LOG_INTERVAL_MS = 10000;  // Log summary every 10s
static constexpr unsigned long DISPLAY_SLOW_THRESHOLD_US = 16000; // 16ms = 60fps budget

#define V1_PERF_START() unsigned long _perfStart = micros()
#define V1_PERF_END(label) do { \
    if (PERF_TIMING_LOGS) { \
        unsigned long _perfDur = micros() - _perfStart; \
        perfTimingAccum += _perfDur; \
        perfTimingCount++; \
        if (_perfDur > perfTimingMax) perfTimingMax = _perfDur; \
        if (millis() - perfLastReport > 5000) { \
            SerialLog.printf("[PERF] %s: avg=%luus max=%luus (n=%lu)\n", \
                label, perfTimingAccum/perfTimingCount, perfTimingMax, perfTimingCount); \
            perfTimingAccum = 0; perfTimingCount = 0; perfTimingMax = 0; \
            perfLastReport = millis(); \
        } \
    } \
} while(0)

// Display latency logging macro - tracks timing and logs to SD when enabled
#define V1_DISPLAY_END(label) do { \
    unsigned long _dur = micros() - _perfStart; \
    displayLatencySum += _dur; \
    displayLatencyCount++; \
    if (_dur > displayLatencyMax) displayLatencyMax = _dur; \
    if (_dur > DISPLAY_SLOW_THRESHOLD_US && debugLogger.isEnabledFor(DebugLogCategory::Display)) { \
        debugLogger.logf(DebugLogCategory::Display, "[SLOW] %s: %lums", label, _dur / 1000); \
    } \
    unsigned long _now = millis(); \
    if ((_now - displayLatencyLastLog) > DISPLAY_LOG_INTERVAL_MS && displayLatencyCount > 0) { \
        if (debugLogger.isEnabledFor(DebugLogCategory::Display)) { \
            debugLogger.logf(DebugLogCategory::Display, "Display: avg=%luus max=%luus n=%lu", \
                displayLatencySum / displayLatencyCount, displayLatencyMax, displayLatencyCount); \
        } \
        displayLatencySum = 0; displayLatencyCount = 0; displayLatencyMax = 0; \
        displayLatencyLastLog = _now; \
    } \
    if (PERF_TIMING_LOGS) { \
        perfTimingAccum += _dur; \
        perfTimingCount++; \
        if (_dur > perfTimingMax) perfTimingMax = _dur; \
        if (_now - perfLastReport > 5000) { \
            SerialLog.printf("[PERF] %s: avg=%luus max=%luus (n=%lu)\n", \
                label, perfTimingAccum/perfTimingCount, perfTimingMax, perfTimingCount); \
            perfTimingAccum = 0; perfTimingCount = 0; perfTimingMax = 0; \
            perfLastReport = _now; \
        } \
    } \
} while(0)

// Global objects
V1BLEClient bleClient;
PacketParser parser;
V1Display display;
TouchHandler touchHandler;

// GPS and Lockout managers (optional modules)
// GPS is static allocation to avoid heap fragmentation - use begin()/end() to enable/disable
GPSHandler gpsHandler;
LockoutManager lockouts;
AutoLockoutManager autoLockouts;

// OBD-II handler uses global obdHandler from obd_handler.cpp
// (included via obd_handler.h extern declaration)

// Queue for BLE data - decouples BLE callbacks from display updates
static QueueHandle_t bleDataQueue = nullptr;
struct BLEDataPacket {
    uint8_t data[128];  // V1 packets are typically <60 bytes; 128 gives headroom
    size_t length;
    uint16_t charUUID;  // Last 16-bit of characteristic UUID to identify source
    uint32_t tsMs;      // Timestamp for latency measurement
};

unsigned long lastDisplayUpdate = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastLvTick = 0;
unsigned long lastRxMillis = 0;
unsigned long lastDisplayDraw = 0;  // Throttle display updates
static constexpr unsigned long DISPLAY_DRAW_MIN_MS = 50;  // Min 50ms between draws (~20fps) - flush alone takes 26ms
static unsigned long lastAlertGapRecoverMs = 0;  // Throttle recovery when bands show but alerts are missing

// Color preview state machine to keep demo visible and cycle bands
static bool colorPreviewActive = false;
static unsigned long colorPreviewStartMs = 0;
static unsigned long colorPreviewDurationMs = 0;
static int colorPreviewStep = 0;
static bool colorPreviewEnded = false;

struct ColorPreviewStep {
    unsigned long offsetMs;
    Band band;
    uint8_t bars;
    Direction dir;
    uint32_t freqMHz;
    bool muted;
};

// Sequence: X, K, Ka, Laser, then Ka muted - with arrow cycle and cards
static const ColorPreviewStep COLOR_PREVIEW_STEPS[] = {
    {300, BAND_X, 3, DIR_FRONT, 10525, false},
    {1300, BAND_K, 5, DIR_SIDE, 24150, false},
    {2300, BAND_KA, 6, DIR_REAR, 35500, false},
    {3300, BAND_LASER, 8, static_cast<Direction>(DIR_FRONT | DIR_REAR), 0, false},
    {4300, BAND_KA, 5, DIR_FRONT, 34700, true}   // Muted Ka to show mute badge
};
static constexpr int COLOR_PREVIEW_STEP_COUNT = sizeof(COLOR_PREVIEW_STEPS) / sizeof(COLOR_PREVIEW_STEPS[0]);

enum class DisplayMode {
    IDLE,
    LIVE
};
static DisplayMode displayMode = DisplayMode::IDLE;

// Voice alerts tracking - announces priority alert when no app is connected
static Band lastVoiceAlertBand = BAND_NONE;
static Direction lastVoiceAlertDirection = DIR_NONE;
static uint16_t lastVoiceAlertFrequency = 0xFFFF;  // Track frequency to avoid re-announcing same alert (0xFFFF = none)
static uint8_t lastVoiceAlertBogeyCount = 0;  // Track bogey count to announce when it changes
static unsigned long lastVoiceAlertTime = 0;
static constexpr unsigned long VOICE_ALERT_COOLDOWN_MS = 5000;  // Min 5s between new alert announcements
static constexpr unsigned long BOGEY_COUNT_COOLDOWN_MS = 2000;  // Min 2s between bogey count-only updates

// Direction change throttling - stop announcing if arrow bounces too much
static uint8_t directionChangeCount = 0;        // Count of direction changes for current alert
static unsigned long directionChangeWindowStart = 0;  // When the throttle window started
static constexpr unsigned long DIRECTION_THROTTLE_WINDOW_MS = 10000;  // 10 second window
static constexpr uint8_t DIRECTION_CHANGE_LIMIT = 3;  // Max changes before throttling

// Secondary alert tracking - announce non-priority alerts once
// Use band<<16 | freq to create unique identifiers (handles Laser freq=0)
// NOTE: Bounds safety verified January 20, 2026 - all accesses check announcedAlertCount < 10
static uint32_t announcedAlertIds[10] = {0};  // All alerts announced this session (band<<16 | freq)
static uint8_t announcedAlertCount = 0;
static unsigned long lastPriorityAnnouncementTime = 0;  // When priority was last announced
static unsigned long priorityStableSince = 0;  // When priority alert became stable
static constexpr unsigned long PRIORITY_STABILITY_MS = 1000;  // Priority must be stable 1s before secondary
static constexpr unsigned long POST_PRIORITY_GAP_MS = 1500;   // Wait 1.5s after priority announcement

// Auto power-off timer - triggered when V1 disconnects and autoPowerOffMinutes > 0
static unsigned long autoPowerOffTimerStart = 0;  // 0 = timer not running
static bool autoPowerOffArmed = false;  // True once V1 data has been received (not just connected)

// OBD auto-connect delay - wait for V1 connection to settle before OBD
static unsigned long obdAutoConnectAt = 0;        // millis() when to attempt OBD connect (0 = disabled)
static constexpr unsigned long OBD_CONNECT_DELAY_MS = 12000;  // 12 second delay after V1 connects

// Deferred camera database loading - loads after V1 connects to not block boot
// Not static - accessed from wifi_manager for runtime GPS enable
bool cameraLoadPending = false;            // True if camera DB should be loaded
bool cameraLoadComplete = false;           // True once loading finished

// Volume fade tracking - reduce V1 volume after X seconds of continuous alert
static unsigned long volumeFadeAlertStartMs = 0;  // When current alert session started (0 = no active alert)
static uint8_t volumeFadeOriginalVol = 0xFF;      // Original main volume before fade (0xFF = not faded)
static uint8_t volumeFadeOriginalMuteVol = 0;     // Original muted volume (to pass to setVolume)
static bool volumeFadeActive = false;             // True if volume has been faded down
static bool volumeFadeCommandSent = false;        // One-shot flag to send fade command once
// Track seen frequencies during fade session - new frequencies restore volume, priority shuffling stays faded
static constexpr int MAX_FADE_SEEN_FREQS = 12;    // Max alerts V1 can track
static uint16_t volumeFadeSeenFreqs[MAX_FADE_SEEN_FREQS] = {0};
static int volumeFadeSeenCount = 0;

// Speed-based volume tracking - boost volume at highway speeds
// NOTE: Speed boost and volume fade can interact. Speed boost captures the pre-boost volume,
// and volume fade captures the volume at alert start. When both are active, we need to 
// restore to the correct "baseline" volume (before both adjustments).
static bool speedVolumeBoostActive = false;       // True if volume is currently boosted
static uint8_t speedVolumeOriginalVol = 0xFF;     // Original volume before speed boost

// Smart threat escalation tracking - detect signals ramping up over time
// Trigger: was weak + now strong + sustained + not too many bogeys
static constexpr int WEAK_THRESHOLD = 2;           // "Was weak" = 2 bars or less
static constexpr int STRONG_THRESHOLD = 4;         // "Now strong" = 4+ bars
static constexpr unsigned long SUSTAINED_MS = 500; // Must stay strong for 500ms (filter spikes)
static constexpr unsigned long HISTORY_STALE_MS = 5000;  // Clear history if no update in 5s
static constexpr int MAX_BOGEYS_FOR_ESCALATION = 4;      // Skip escalation in noisy environments

struct AlertHistory {
    uint32_t alertId;           // (band << 16) | freq
    uint8_t currentBars;        // Latest reading
    uint32_t lastUpdateMs;      // For staleness check
    uint32_t strongSinceMs;     // When first hit strong threshold (0 = not strong)
    bool wasWeak;               // True if ever seen at ≤2 bars (never cleared)
    bool escalationAnnounced;   // One-shot flag
};
// NOTE: Bounds safety verified January 20, 2026 - all accesses check alertHistoryCount < 10
static AlertHistory alertHistories[10];
static uint8_t alertHistoryCount = 0;

// Helper functions for secondary alert tracking
// Alert ID = (band << 16) | frequency - ensures Laser (freq=0) is unique per band
static uint32_t makeAlertId(Band band, uint16_t freq) {
    return ((uint32_t)band << 16) | freq;
}

static bool isAlertAnnounced(Band band, uint16_t freq) {
    uint32_t id = makeAlertId(band, freq);
    for (int i = 0; i < announcedAlertCount; i++) {
        if (announcedAlertIds[i] == id) return true;
    }
    return false;
}

static void markAlertAnnounced(Band band, uint16_t freq) {
    uint32_t id = makeAlertId(band, freq);
    if (announcedAlertCount < 10 && !isAlertAnnounced(band, freq)) {
        announcedAlertIds[announcedAlertCount++] = id;
    }
}

static void clearAnnouncedAlerts() {
    announcedAlertCount = 0;
    memset(announcedAlertIds, 0, sizeof(announcedAlertIds));
    alertHistoryCount = 0;
    memset(alertHistories, 0, sizeof(alertHistories));
}

// Alert history tracking for smart threat escalation
static AlertHistory* findAlertHistory(uint32_t alertId) {
    for (int i = 0; i < alertHistoryCount; i++) {
        if (alertHistories[i].alertId == alertId) {
            return &alertHistories[i];
        }
    }
    return nullptr;
}

static AlertHistory* getOrCreateAlertHistory(uint32_t alertId, unsigned long now) {
    // Find existing
    AlertHistory* h = findAlertHistory(alertId);
    if (h) return h;
    
    // Create new if space available
    if (alertHistoryCount < 10) {
        h = &alertHistories[alertHistoryCount++];
        h->alertId = alertId;
        h->currentBars = 0;
        h->lastUpdateMs = now;
        h->strongSinceMs = 0;  // Not strong yet
        h->wasWeak = false;    // Will be set if bars <= WEAK_THRESHOLD
        h->escalationAnnounced = false;
        return h;
    }
    
    // No space - find oldest stale entry to recycle
    unsigned long oldestTime = now;
    int oldestIdx = -1;
    for (int i = 0; i < alertHistoryCount; i++) {
        if (alertHistories[i].lastUpdateMs < oldestTime) {
            oldestTime = alertHistories[i].lastUpdateMs;
            oldestIdx = i;
        }
    }
    if (oldestIdx >= 0) {
        h = &alertHistories[oldestIdx];
        h->alertId = alertId;
        h->currentBars = 0;
        h->lastUpdateMs = now;
        h->strongSinceMs = 0;
        h->wasWeak = false;
        h->escalationAnnounced = false;
        return h;
    }
    
    return nullptr;
}

static void updateAlertHistory(Band band, uint16_t freq, uint8_t bars, unsigned long now) {
    if (band == BAND_LASER) return;  // Laser excluded from smart tracking
    
    uint32_t alertId = makeAlertId(band, freq);
    AlertHistory* h = getOrCreateAlertHistory(alertId, now);
    if (!h) return;
    
    // Track if ever weak (permanent flag - never cleared)
    if (bars <= WEAK_THRESHOLD) {
        h->wasWeak = true;
    }
    
    // Track sustained strong signal
    if (bars >= STRONG_THRESHOLD) {
        if (h->strongSinceMs == 0) {
            h->strongSinceMs = now;  // Just became strong
        }
    } else {
        h->strongSinceMs = 0;  // Dropped below strong, reset
    }
    
    h->currentBars = bars;
    h->lastUpdateMs = now;
}

static void cleanupStaleHistories(unsigned long now) {
    for (int i = alertHistoryCount - 1; i >= 0; i--) {
        if (now - alertHistories[i].lastUpdateMs > HISTORY_STALE_MS) {
            // Remove by shifting
            for (int j = i; j < alertHistoryCount - 1; j++) {
                alertHistories[j] = alertHistories[j + 1];
            }
            alertHistoryCount--;
        }
    }
}

// Check if alert should trigger threat escalation announcement
// Trigger: wasWeak + nowStrong + sustained 500ms + bogeys <= 4 + not muted + not announced
static bool shouldAnnounceThreatEscalation(Band band, uint16_t freq, uint8_t totalBogeys, unsigned long now) {
    if (band == BAND_LASER) return false;  // Laser excluded
    
    uint32_t alertId = makeAlertId(band, freq);
    AlertHistory* h = findAlertHistory(alertId);
    if (!h) return false;
    
    // Check all conditions
    bool wasWeak = h->wasWeak;
    bool nowStrong = (h->currentBars >= STRONG_THRESHOLD);
    bool sustained = (h->strongSinceMs > 0) && (now - h->strongSinceMs >= SUSTAINED_MS);
    bool notNoisy = (totalBogeys <= MAX_BOGEYS_FOR_ESCALATION);
    bool notAnnounced = !h->escalationAnnounced;
    
    if (wasWeak && nowStrong && sustained && notNoisy && notAnnounced) {
        DEBUG_LOGF("[ThreatEscalation] TRIGGERED: band=%d freq=%u wasWeak=%d cur=%d strongFor=%lums bogeys=%d\n",
                   (int)band, freq, h->wasWeak, h->currentBars, now - h->strongSinceMs, totalBogeys);
        return true;
    }
    return false;
}

static void markThreatEscalationAnnounced(Band band, uint16_t freq) {
    uint32_t alertId = makeAlertId(band, freq);
    AlertHistory* h = findAlertHistory(alertId);
    if (h) {
        h->escalationAnnounced = true;
    }
}

static bool isBandEnabledForSecondary(Band band, const V1Settings& settings) {
    switch (band) {
        case BAND_LASER: return settings.secondaryLaser;
        case BAND_KA:    return settings.secondaryKa;
        case BAND_K:     return settings.secondaryK;
        case BAND_X:     return settings.secondaryX;
        default:         return false;
    }
}

// Helper: get signal bars for an alert based on direction (0-8 scale from V1)
static uint8_t getAlertBars(const AlertData& a) {
    if (a.direction & DIR_FRONT) return a.frontStrength;
    if (a.direction & DIR_REAR) return a.rearStrength;
    return (a.frontStrength > a.rearStrength) ? a.frontStrength : a.rearStrength;
}

// Speed cache - avoids issues with OBD poll timing jitter
static float cachedSpeedMph = 0.0f;
static unsigned long cachedSpeedTimestamp = 0;
static constexpr unsigned long SPEED_CACHE_MAX_AGE_MS = 5000;  // 5s cache - conservative for safety

// Helper: get current speed in MPH from OBD (preferred) or GPS, with caching
static float getCurrentSpeedMph() {
    unsigned long now = millis();
    
    // Try OBD first (more accurate, works in tunnels/garages)
    if (obdHandler.isModuleDetected() && obdHandler.hasValidData()) {
        cachedSpeedMph = obdHandler.getSpeedMph();
        cachedSpeedTimestamp = now;
        return cachedSpeedMph;
    }
    
    // Try GPS
    if (gpsHandler.hasValidFix()) {
        cachedSpeedMph = gpsHandler.getSpeed() * 2.237f;  // m/s to mph
        cachedSpeedTimestamp = now;
        return cachedSpeedMph;
    }
    
    // Use cached speed if still reasonably fresh (handles OBD jitter)
    if (cachedSpeedTimestamp > 0 && (now - cachedSpeedTimestamp) < SPEED_CACHE_MAX_AGE_MS) {
        return cachedSpeedMph;
    }
    
    // Cache expired - return 0
    return 0.0f;
}

// Helper: check if we have any valid speed source (for low-speed mute logic)
static bool hasValidSpeedSource() {
    unsigned long now = millis();
    // Fresh OBD or GPS data, or valid cache
    return (obdHandler.isModuleDetected() && obdHandler.hasValidData()) ||
           gpsHandler.hasValidFix() ||
           (cachedSpeedTimestamp > 0 && (now - cachedSpeedTimestamp) < SPEED_CACHE_MAX_AGE_MS);
}

// Helper: check if voice should be muted due to low speed (parking lot mode)
// Skip low-speed muting if a phone app (JBV1/V1Driver) is connected - it handles its own alerts
static bool isLowSpeedMuted() {
    const V1Settings& s = settingsManager.get();
    if (!s.lowSpeedMuteEnabled) return false;
    
    // Don't mute if phone app is connected - let the app handle it
    if (bleClient.isProxyClientConnected()) return false;
    
    // Only mute if we have a valid speed source - don't mute just because no GPS/OBD
    if (!hasValidSpeedSource()) return false;
    
    float speedMph = getCurrentSpeedMph();
    bool muted = speedMph < s.lowSpeedMuteThresholdMph;
    if (muted) {
        static unsigned long lastLogTime = 0;
        if (millis() - lastLogTime > 5000) {  // Log every 5s max
            SerialLog.printf("[LowSpeedMute] Voice muted: %.1f mph < %d threshold\n", speedMph, s.lowSpeedMuteThresholdMph);
            lastLogTime = millis();
        }
    }
    return muted;
}

// WiFi manual startup - user must long-press BOOT to start AP

void requestColorPreviewHold(uint32_t durationMs) {
    colorPreviewActive = true;
    colorPreviewStartMs = millis();
    colorPreviewDurationMs = durationMs;
    colorPreviewStep = 0;
    colorPreviewEnded = false;
}

bool isColorPreviewRunning() {
    return colorPreviewActive;
}

void cancelColorPreview() {
    if (colorPreviewActive) {
        colorPreviewActive = false;
        colorPreviewEnded = true;  // Restore resting view on next loop
    }
}

static inline bool isColorPreviewActive() {
    if (!colorPreviewActive) return false;
    unsigned long now = millis();
    if (now - colorPreviewStartMs >= colorPreviewDurationMs) {
        colorPreviewActive = false;
        colorPreviewEnded = true;
        return false;
    }
    return true;
}

static void driveColorPreview() {
    if (!colorPreviewActive) return;

    unsigned long now = millis();
    unsigned long elapsed = now - colorPreviewStartMs;

    // Advance through band samples while within duration
    while (colorPreviewStep < COLOR_PREVIEW_STEP_COUNT && elapsed >= COLOR_PREVIEW_STEPS[colorPreviewStep].offsetMs) {
        const auto& step = COLOR_PREVIEW_STEPS[colorPreviewStep];
        AlertData previewAlert{};
        previewAlert.band = step.band;
        previewAlert.direction = step.dir;
        previewAlert.frontStrength = step.bars;
        previewAlert.rearStrength = 0;
        previewAlert.frequency = step.freqMHz;
        previewAlert.isValid = true;

        DisplayState previewState{};
        previewState.activeBands = step.band;
        previewState.arrows = step.dir;
        previewState.signalBars = step.bars;
        previewState.muted = step.muted;

        // Build array with secondary alerts to show cards during preview
        // Start adding secondary alerts from step 1 onwards
        AlertData allAlerts[3];
        int alertCount = 1;
        allAlerts[0] = previewAlert;
        
        if (colorPreviewStep >= 1) {
            // Add X band as secondary card
            allAlerts[alertCount].band = BAND_X;
            allAlerts[alertCount].direction = DIR_FRONT;
            allAlerts[alertCount].frontStrength = 3;
            allAlerts[alertCount].frequency = 10525;
            allAlerts[alertCount].isValid = true;
            previewState.activeBands = static_cast<Band>(previewState.activeBands | BAND_X);
            alertCount++;
        }
        if (colorPreviewStep >= 2) {
            // Add K band as secondary card
            allAlerts[alertCount].band = BAND_K;
            allAlerts[alertCount].direction = DIR_REAR;
            allAlerts[alertCount].frontStrength = 0;
            allAlerts[alertCount].rearStrength = 4;
            allAlerts[alertCount].frequency = 24150;
            allAlerts[alertCount].isValid = true;
            previewState.activeBands = static_cast<Band>(previewState.activeBands | BAND_K);
            alertCount++;
        }

        display.update(previewAlert, allAlerts, alertCount, previewState);
        colorPreviewStep++;
    }

    if (elapsed >= colorPreviewDurationMs) {
        colorPreviewActive = false;
        colorPreviewEnded = true;
    }
}

// Mute debounce - prevent flicker from rapid state changes  
static bool debouncedMuteState = false;
static unsigned long lastMuteChangeMs = 0;
static constexpr unsigned long MUTE_DEBOUNCE_MS = 150;  // Ignore mute changes within 150ms

// Alert persistence - show last alert in grey after V1 clears it
static AlertData persistedAlert;
static unsigned long alertClearedTime = 0;
static bool alertPersistenceActive = false;

// Camera alert tracking
static bool cameraAlertActive = false;
static unsigned long cameraAlertStartMs = 0;
static unsigned long lastCameraCheckMs = 0;
static NearbyCameraResult currentCameraAlert;
static float lastCameraAlertLat = 0.0f;
static float lastCameraAlertLon = 0.0f;
static constexpr unsigned long CAMERA_CHECK_INTERVAL_MS = 500;  // Check every 500ms
static constexpr float CAMERA_ALERT_COOLDOWN_M = 200.0f;  // Min distance before re-alerting same area

// Camera test alert mode (from web UI)
static bool cameraTestActive = false;
static unsigned long cameraTestEndMs = 0;
static char cameraTestTypeName[16] = {0};
static float cameraTestDistance = 500.0f;

// Regional camera cache - rebuilds every 30min or when moved 50+ miles
static unsigned long lastCacheCheckMs = 0;
static constexpr unsigned long CACHE_CHECK_INTERVAL_MS = 30000;   // Check every 30 seconds
static constexpr unsigned long CACHE_REFRESH_INTERVAL_MS = 1800000; // Force refresh every 30 minutes
static constexpr float CACHE_RADIUS_MILES = 100.0f;    // Cache cameras within 100 miles
static constexpr float CACHE_REFRESH_DIST_MILES = 50.0f; // Refresh if moved 50+ miles from cache center

// Triple-tap detection for profile cycling
static unsigned long lastTapTime = 0;
static int tapCount = 0;
static constexpr unsigned long TAP_WINDOW_MS = 600;  // Window for 3 taps
static constexpr unsigned long TAP_DEBOUNCE_MS = 150; // Minimum time between taps

// Brightness adjustment mode (BOOT button triggered)
static bool brightnessAdjustMode = false;
static uint8_t brightnessAdjustValue = 200;  // Current brightness slider value
static uint8_t volumeAdjustValue = 75;       // Current voice volume slider value
static int activeSlider = 0;                 // 0=brightness, 1=volume
static unsigned long lastVolumeChangeMs = 0; // Debounce for test voice playback
static unsigned long bootPressStart = 0;     // For long-press detection
static bool bootWasPressed = false;
static bool wifiToggleTriggered = false;     // Track if WiFi toggle already fired during this press
static constexpr unsigned long BOOT_DEBOUNCE_MS = 300;  // Debounce for BOOT button
static constexpr unsigned long AP_TOGGLE_LONG_PRESS_MS = 4000;  // Long press duration for WiFi toggle
static constexpr unsigned long VOLUME_TEST_DEBOUNCE_MS = 1000;  // Debounce for test voice after volume change


// Buffer for accumulating BLE data in main loop context
static std::vector<uint8_t> rxBuffer;

enum AutoPushStep {
    AUTO_PUSH_STEP_IDLE = 0,
    AUTO_PUSH_STEP_WAIT_READY,
    AUTO_PUSH_STEP_PROFILE,
    AUTO_PUSH_STEP_PROFILE_READBACK,  // Wait before requesting user bytes read-back
    AUTO_PUSH_STEP_DISPLAY,
    AUTO_PUSH_STEP_MODE,
    AUTO_PUSH_STEP_VOLUME,
};

struct AutoPushState {
    AutoPushStep step = AUTO_PUSH_STEP_IDLE;
    unsigned long nextStepAtMs = 0;
    int slotIndex = 0;
    AutoPushSlot slot;
    V1Profile profile;
    bool profileLoaded = false;
};

static AutoPushState autoPushState;


// Callback for BLE data reception - just queues data, doesn't process
// This runs in BLE task context, so we avoid SPI operations here
void onV1Data(const uint8_t* data, size_t length, uint16_t charUUID) {
    if (bleDataQueue && length > 0 && length <= sizeof(BLEDataPacket::data)) {
        PERF_INC(rxPackets);  // Count every packet received from V1
        BLEDataPacket pkt;
        memcpy(pkt.data, data, length);
        pkt.length = length;
        pkt.charUUID = charUUID;
        pkt.tsMs = millis();
        // Non-blocking send to queue - if queue is full, drop the packet
        BaseType_t result = xQueueSend(bleDataQueue, &pkt, 0);
        if (result != pdTRUE) {
            PERF_INC(queueDrops);  // Count queue overflows
            BLEDataPacket dropped;
            xQueueReceive(bleDataQueue, &dropped, 0);  // Drop oldest to make room
            xQueueSend(bleDataQueue, &pkt, 0);
        }
        // Track queue high-water mark for perf monitoring
        UBaseType_t queueDepth = uxQueueMessagesWaiting(bleDataQueue);
        PERF_MAX(queueHighWater, queueDepth);
    }
}

static void startAutoPush(int slotIndex) {
    static const char* slotNames[] = {"Default", "Highway", "Passenger Comfort"};
    int clampedIndex = std::max(0, std::min(2, slotIndex));
    autoPushState.slotIndex = clampedIndex;
    autoPushState.slot = settingsManager.getSlot(clampedIndex);
    autoPushState.profileLoaded = false;
    autoPushState.profile = V1Profile();
    autoPushState.step = AUTO_PUSH_STEP_WAIT_READY;
    autoPushState.nextStepAtMs = millis() + 500;
    AUTO_PUSH_LOGF("[AutoPush] V1 connected - applying '%s' profile (slot %d)...\n",
                   slotNames[clampedIndex], clampedIndex);
    
    // Show the profile indicator on display when auto-push starts
    display.drawProfileIndicator(clampedIndex);
}

static void processAutoPush() {
    if (autoPushState.step == AUTO_PUSH_STEP_IDLE) {
        return;
    }

    if (!bleClient.isConnected()) {
        autoPushState.step = AUTO_PUSH_STEP_IDLE;
        return;
    }

    unsigned long now = millis();
    if (now < autoPushState.nextStepAtMs) {
        return;
    }

    switch (autoPushState.step) {
        case AUTO_PUSH_STEP_WAIT_READY:
            autoPushState.step = AUTO_PUSH_STEP_PROFILE;
            autoPushState.nextStepAtMs = now;
            return;

        case AUTO_PUSH_STEP_PROFILE: {
            const AutoPushSlot& slot = autoPushState.slot;
            if (slot.profileName.length() > 0) {
                AUTO_PUSH_LOGF("[AutoPush] Loading profile: %s\n", slot.profileName.c_str());
                V1Profile profile;
                if (v1ProfileManager.loadProfile(slot.profileName, profile)) {
                    autoPushState.profile = profile;
                    autoPushState.profileLoaded = true;
                    
                    // Apply slot-level Mute to Zero setting to user bytes before pushing
                    bool slotMuteToZero = settingsManager.getSlotMuteToZero(autoPushState.slotIndex);
                    AUTO_PUSH_LOGF("[AutoPush] Slot %d MZ setting: %s\n", 
                                    autoPushState.slotIndex, slotMuteToZero ? "ON" : "OFF");
                    AUTO_PUSH_LOGF("[AutoPush] Profile byte0 before: 0x%02X\n", profile.settings.bytes[0]);
                    
                    V1UserSettings modifiedSettings = profile.settings;
                    if (slotMuteToZero) {
                        // MZ enabled: clear bit 4 (inverted logic)
                        modifiedSettings.bytes[0] &= ~0x10;
                    } else {
                        // MZ disabled: set bit 4
                        modifiedSettings.bytes[0] |= 0x10;
                    }
                    AUTO_PUSH_LOGF("[AutoPush] Modified byte0: 0x%02X (bit4=%d means MZ=%s)\n",
                                    modifiedSettings.bytes[0], 
                                    (modifiedSettings.bytes[0] & 0x10) ? 1 : 0,
                                    (modifiedSettings.bytes[0] & 0x10) ? "OFF" : "ON");
                    
                    if (bleClient.writeUserBytes(modifiedSettings.bytes)) {
                        AUTO_PUSH_LOGF("[AutoPush] Profile settings pushed (MZ=%s)\n", 
                                        slotMuteToZero ? "ON" : "OFF");
                        // Schedule read-back request after 100ms (non-blocking)
                        autoPushState.step = AUTO_PUSH_STEP_PROFILE_READBACK;
                        autoPushState.nextStepAtMs = now + 100;
                        return;
                    } else {
                        AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to push profile settings");
                    }
                } else {
                    AUTO_PUSH_LOGF("[AutoPush] ERROR: Failed to load profile '%s'\n", slot.profileName.c_str());
                    autoPushState.profileLoaded = false;
                }
            } else {
                AUTO_PUSH_LOGLN("[AutoPush] No profile configured for active slot");
                autoPushState.profileLoaded = false;
            }

            // Always proceed to display step to apply slot's dark mode setting
            autoPushState.step = AUTO_PUSH_STEP_DISPLAY;
            autoPushState.nextStepAtMs = now + 100;
            return;
        }

        case AUTO_PUSH_STEP_PROFILE_READBACK:
            // Request user bytes read-back to verify V1 accepted the settings
            bleClient.requestUserBytes();
            AUTO_PUSH_LOGLN("[AutoPush] Requested user bytes read-back for verification");
            autoPushState.step = AUTO_PUSH_STEP_DISPLAY;
            autoPushState.nextStepAtMs = now + 100;
            return;

        case AUTO_PUSH_STEP_DISPLAY: {
            // Use slot-level dark mode setting (inverted: darkMode=true means display OFF)
            bool slotDarkMode = settingsManager.getSlotDarkMode(autoPushState.slotIndex);
            bool displayOn = !slotDarkMode;  // Dark mode = display off
            bleClient.setDisplayOn(displayOn);
            AUTO_PUSH_LOGF("[AutoPush] Display set to: %s (darkMode=%s)\n",
                           displayOn ? "ON" : "OFF", slotDarkMode ? "true" : "false");
            autoPushState.step = AUTO_PUSH_STEP_MODE;
            autoPushState.nextStepAtMs = now + (autoPushState.slot.mode != V1_MODE_UNKNOWN ? 100 : 0);
            return;
        }

        case AUTO_PUSH_STEP_MODE: {
            if (autoPushState.slot.mode != V1_MODE_UNKNOWN) {
                const char* modeName = "Unknown";
                if (autoPushState.slot.mode == V1_MODE_ALL_BOGEYS) modeName = "All Bogeys";
                else if (autoPushState.slot.mode == V1_MODE_LOGIC) modeName = "Logic";
                else if (autoPushState.slot.mode == V1_MODE_ADVANCED_LOGIC) modeName = "Advanced Logic";

                if (bleClient.setMode(static_cast<uint8_t>(autoPushState.slot.mode))) {
                    AUTO_PUSH_LOGF("[AutoPush] Mode set to: %s\n", modeName);
                } else {
                    AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to set mode");
                }
            }

            bool volumeChangeNeeded =
                (settingsManager.getSlotVolume(autoPushState.slotIndex) != 0xFF ||
                 settingsManager.getSlotMuteVolume(autoPushState.slotIndex) != 0xFF);
            autoPushState.step = AUTO_PUSH_STEP_VOLUME;
            autoPushState.nextStepAtMs = now + (volumeChangeNeeded ? 100 : 0);
            return;
        }

        case AUTO_PUSH_STEP_VOLUME: {
            uint8_t mainVol = settingsManager.getSlotVolume(autoPushState.slotIndex);
            uint8_t muteVol = settingsManager.getSlotMuteVolume(autoPushState.slotIndex);
            if (mainVol != 0xFF || muteVol != 0xFF) {
                if (bleClient.setVolume(mainVol, muteVol)) {
                    AUTO_PUSH_LOGF("[AutoPush] Volume set - main: %d, muted: %d\n", mainVol, muteVol);
                } else {
                    AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to set volume");
                }
            }

            AUTO_PUSH_LOGLN("[AutoPush] Complete");
            autoPushState.step = AUTO_PUSH_STEP_IDLE;
            autoPushState.nextStepAtMs = 0;
            return;
        }

        default:
            autoPushState.step = AUTO_PUSH_STEP_IDLE;
            return;
    }
}

// Start WiFi after BLE connects to avoid radio contention during connection
void startWifi() {
    if (wifiManager.isSetupModeActive()) return;
    
    if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
        debugLogger.log(DebugLogCategory::Wifi, "startWifi() requested");
    }

    SerialLog.println("[WiFi] Starting WiFi (manual start)...");
    wifiManager.begin();
    
    // Reduce WiFi TX power to minimize interference with BLE
    // WIFI_POWER_11dBm is a good balance - enough for local AP, less BLE interference
    WiFi.setTxPower(WIFI_POWER_11dBm);
    SerialLog.println("[WiFi] TX power reduced to 11dBm for BLE coexistence");
    
    // Set up callbacks for web interface
    wifiManager.setStatusCallback([]() {
        return "\"v1_connected\":" + String(bleClient.isConnected() ? "true" : "false");
    });
    
    wifiManager.setAlertCallback([]() {
        JsonDocument doc;
        if (parser.hasAlerts()) {
            AlertData alert = parser.getPriorityAlert();
            doc["active"] = true;
            const char* bandStr = "None";
            if (alert.band == BAND_KA) bandStr = "Ka";
            else if (alert.band == BAND_K) bandStr = "K";
            else if (alert.band == BAND_X) bandStr = "X";
            else if (alert.band == BAND_LASER) bandStr = "LASER";
            doc["band"] = bandStr;
            doc["strength"] = alert.frontStrength;
            doc["frequency"] = alert.frequency;
            doc["direction"] = alert.direction;
        } else {
            doc["active"] = false;
        }
        String json;
        serializeJson(doc, json);
        return json;
    });
    
    // Set up command callback for dark mode and mute
    wifiManager.setCommandCallback([](const char* cmd, bool state) {
        if (strcmp(cmd, "display") == 0) {
            return bleClient.setDisplayOn(state);
        } else if (strcmp(cmd, "mute") == 0) {
            return bleClient.setMute(state);
        }
        return false;
    });

    // Provide filesystem access for web profile/device APIs
    wifiManager.setFilesystemCallback([]() -> fs::FS* {
        return storageManager.isReady() ? storageManager.getFilesystem() : nullptr;
    });
    
    // Manual profile push trigger from /api/profile/push
    wifiManager.setProfilePushCallback([]() {
        const V1Settings& s = settingsManager.get();
        startAutoPush(s.activeSlot);
        return true;
    });
    
    // GPS status callback for web API
    wifiManager.setGpsStatusCallback([]() {
        JsonDocument doc;
        
        doc["enabled"] = gpsHandler.isEnabled();
        doc["moduleDetected"] = gpsHandler.isModuleDetected();
        doc["detectionComplete"] = gpsHandler.isDetectionComplete();
        doc["hasValidFix"] = gpsHandler.hasValidFix();
        
        if (gpsHandler.isEnabled() && gpsHandler.isModuleDetected()) {
            GPSFix fix = gpsHandler.getFix();
            doc["latitude"] = fix.latitude;
            doc["longitude"] = fix.longitude;
            doc["satellites"] = fix.satellites;
            doc["hdop"] = fix.hdop;
            doc["speed_mph"] = fix.speed_mps * 2.237f;
            doc["heading"] = fix.heading_deg;
            doc["fixValid"] = fix.valid;
            doc["fixStale"] = gpsHandler.isFixStale();
            doc["moving"] = gpsHandler.isMoving();
            
            // GPS time if available
            if (fix.unixTime > 0) {
                doc["gpsTime"] = fix.unixTime;
                char timeStr[32];
                snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d UTC", fix.hour, fix.minute, fix.seconds);
                doc["gpsTimeStr"] = timeStr;
            }
        }
        
        String json;
        serializeJson(doc, json);
        return json;
    });
    
    // GPS reset callback for web API
    wifiManager.setGpsResetCallback([]() {
        gpsHandler.reset();
    });
    
    // Camera status callback for web API
    wifiManager.setCameraStatusCallback([]() {
        JsonDocument doc;
        
        doc["loaded"] = cameraManager.isLoaded();
        doc["count"] = cameraManager.getCameraCount();
        
        if (cameraManager.isLoaded()) {
            doc["name"] = cameraManager.getDatabaseName();
            doc["date"] = cameraManager.getDatabaseDate();
            doc["redLightCount"] = cameraManager.getRedLightCount();
            doc["speedCount"] = cameraManager.getSpeedCameraCount();
            doc["alprCount"] = cameraManager.getALPRCount();
            
            // Regional cache info
            if (cameraManager.hasRegionalCache()) {
                JsonObject cache = doc["cache"].to<JsonObject>();
                float cacheLat, cacheLon;
                cameraManager.getCacheCenter(cacheLat, cacheLon);
                cache["count"] = cameraManager.getRegionalCacheCount();
                cache["centerLat"] = cacheLat;
                cache["centerLon"] = cacheLon;
                cache["radiusMiles"] = cameraManager.getCacheRadius();
            }
        }
        
        String json;
        serializeJson(doc, json);
        return json;
    });
    
    // Camera reload callback for web API
    wifiManager.setCameraReloadCallback([]() {
        if (!storageManager.isReady() || !storageManager.isSDCard()) {
            return false;
        }
        fs::FS* fs = storageManager.getFilesystem();
        if (!fs) return false;
        
        return cameraManager.begin(fs);
    });
    
    // Camera test callback for web API - triggers display + voice
    wifiManager.setCameraTestCallback([](int type) {
        // type: 0=red light, 1=speed, 2=ALPR, 3=red light+speed
        
        // Get camera type name for display
        const char* typeName = "CAM";
        CameraAlertType voiceType = CameraAlertType::SPEED;
        switch (type) {
            case 0:
                typeName = "REDLIGHT";
                voiceType = CameraAlertType::RED_LIGHT;
                break;
            case 1:
                typeName = "SPEED";
                voiceType = CameraAlertType::SPEED;
                break;
            case 2:
                typeName = "ALPR";
                voiceType = CameraAlertType::ALPR;
                break;
            case 3:
                typeName = "RLS";
                voiceType = CameraAlertType::RED_LIGHT_SPEED;
                break;
        }
        
        // Set up test alert mode - persists for 5 seconds
        cameraTestActive = true;
        cameraTestEndMs = millis() + 5000;
        strncpy(cameraTestTypeName, typeName, sizeof(cameraTestTypeName) - 1);
        cameraTestDistance = 500.0f;
        
        // Play voice alert
        play_camera_voice(voiceType);
        
        Serial.printf("[Camera] Test alert: %s (type %d) - showing for 5s\n", typeName, type);
    });
    
    SerialLog.println("[WiFi] Initialized");
}

// Callback when V1 connection is fully established
// Handles auto-push of default profile and mode
void onV1Connected() {
    const V1Settings& s = settingsManager.get();
    int activeSlotIndex = std::max(0, std::min(2, s.activeSlot));
    if (activeSlotIndex != s.activeSlot) {
        AUTO_PUSH_LOGF("[AutoPush] WARNING: activeSlot out of range (%d). Using slot %d instead.\n",
                        s.activeSlot, activeSlotIndex);
    }
    
    // Schedule delayed OBD auto-connect if OBD is enabled
    if (s.obdEnabled) {
        obdAutoConnectAt = millis() + OBD_CONNECT_DELAY_MS;
        SerialLog.printf("[OBD] V1 connected - will attempt OBD connect in %lums\n", OBD_CONNECT_DELAY_MS);
    }
    
    // Trigger deferred camera database loading now that V1 is connected
    // This happens in loop() to avoid blocking the callback
    if (cameraLoadPending && !cameraLoadComplete) {
        SerialLog.println("[Camera] V1 connected - camera database will load shortly");
    }
    
    if (!s.autoPushEnabled) {
        AUTO_PUSH_LOGLN("[AutoPush] Disabled, skipping");
        return;
    }

    // Use global activeSlot
    AUTO_PUSH_LOGF("[AutoPush] Using global activeSlot: %d\n", activeSlotIndex);

    startAutoPush(activeSlotIndex);
}

#ifdef REPLAY_MODE
// ============================
// Packet Replay for UI Testing
// ============================

// Sample V1 packets for testing (captured from real device)
// Format: 0xAA <dest> <origin> <packetID> <len> <payload...> <checksum> 0xAB

// Alert packet: Ka 33.800 GHz, front, strength 5
const uint8_t REPLAY_PACKET_KA_ALERT[] = {
    0xAA, 0x04, 0x0A, 0x43, 0x0C,  // Header: alert data, 12 bytes payload
    0x04, 0x01, 0x05, 0x00,        // Band=Ka(4), direction=front(1), frontStrength=5, rearStrength=0
    0x00, 0xD0, 0x2F, 0x01,        // Frequency: 33.800 GHz (0x012FD000 = 19980288 in 100kHz units)
    0x00, 0x00, 0x00, 0x01,        // Count=1, flags
    0xE8, 0xAB                     // Checksum, end
};

// Display data packet: Ka active, 3 signal bars, muted
const uint8_t REPLAY_PACKET_DISPLAY_MUTED[] = {
    0xAA, 0x04, 0x0A, 0x31, 0x08,  // Header: display data, 8 bytes payload
    0x04, 0x01, 0x03, 0x01,        // activeBands=Ka(4), arrows=front(1), signalBars=3, muted=1
    0x00, 0x00, 0x00, 0x00,        // Padding
    0x00, 0x00, 0x00, 0x00,
    0x8A, 0xAB                     // Checksum, end
};

// Display data: X band active, not muted
const uint8_t REPLAY_PACKET_DISPLAY_X[] = {
    0xAA, 0x04, 0x0A, 0x31, 0x08,
    0x01, 0x01, 0x04, 0x00,        // X band, front, 4 bars, not muted
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x7E, 0xAB
};

// Alert: K band 24.150 GHz, rear, strength 3
const uint8_t REPLAY_PACKET_K_ALERT[] = {
    0xAA, 0x04, 0x0A, 0x43, 0x0C,
    0x02, 0x02, 0x00, 0x03,        // Band=K(2), direction=rear(2), frontStrength=0, rearStrength=3
    0x00, 0x6C, 0xBE, 0x03,        // Frequency: 24.150 GHz
    0x00, 0x00, 0x00, 0x02,        // Count=2
    0xD9, 0xAB
};

// Laser alert
const uint8_t REPLAY_PACKET_LASER[] = {
    0xAA, 0x04, 0x0A, 0x43, 0x0C,
    0x08, 0x01, 0x08, 0x00,        // Band=Laser(8), direction=front, strength=8
    0x00, 0x00, 0x00, 0x00,        // No frequency for laser
    0x00, 0x00, 0x00, 0x01,
    0xA8, 0xAB
};

// Clear/no alert
const uint8_t REPLAY_PACKET_CLEAR[] = {
    0xAA, 0x04, 0x0A, 0x31, 0x08,
    0x00, 0x00, 0x00, 0x00,        // No bands, no arrows, no bars, not muted
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x73, 0xAB
};

struct ReplayPacket {
    const uint8_t* data;
    size_t length;
    unsigned long delayMs;  // Delay before next packet
};

// Replay sequence: simulate realistic alert scenarios
const ReplayPacket replaySequence[] = {
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 2000},           // Start clear
    {REPLAY_PACKET_KA_ALERT, sizeof(REPLAY_PACKET_KA_ALERT), 100},     // Ka alert appears
    {REPLAY_PACKET_DISPLAY_MUTED, sizeof(REPLAY_PACKET_DISPLAY_MUTED), 1000},  // Show muted
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 1500},          // Clear
    {REPLAY_PACKET_DISPLAY_X, sizeof(REPLAY_PACKET_DISPLAY_X), 100},   // X band
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 2000},          // Clear
    {REPLAY_PACKET_K_ALERT, sizeof(REPLAY_PACKET_K_ALERT), 100},       // K rear
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 1500},          // Clear
    {REPLAY_PACKET_LASER, sizeof(REPLAY_PACKET_LASER), 100},           // Laser!
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 3000},          // Clear and loop
};

const size_t REPLAY_SEQUENCE_LENGTH = sizeof(replaySequence) / sizeof(replaySequence[0]);

unsigned long lastReplayTime = 0;
size_t replayIndex = 0;

void processReplayData() {
    unsigned long now = millis();
    
    // Check if it's time for the next packet
    if (now - lastReplayTime < replaySequence[replayIndex].delayMs) {
        return;
    }
    
    // Inject packet into rxBuffer (same as BLE would)
    const ReplayPacket& pkt = replaySequence[replayIndex];
    rxBuffer.insert(rxBuffer.end(), pkt.data, pkt.data + pkt.length);
    
    SerialLog.printf("[REPLAY] Injected packet %d/%d (%d bytes)\n", 
                     replayIndex + 1, REPLAY_SEQUENCE_LENGTH, pkt.length);
    
    // Advance to next packet
    lastReplayTime = now;
    replayIndex = (replayIndex + 1) % REPLAY_SEQUENCE_LENGTH;
}
#endif // REPLAY_MODE

// Process queued BLE data - called from main loop (safe for SPI)
void processBLEData() {
#ifdef REPLAY_MODE
    // In replay mode, inject test packets instead of reading from BLE
    processReplayData();
#else
    // Normal BLE mode
    if (colorPreviewActive) {
        return;  // Hold preview on-screen; skip live data
    }
    BLEDataPacket pkt;
    uint32_t latestPktTs = 0;
    
    // Process all queued packets (with safety cap to prevent unbounded growth)
    constexpr size_t RX_BUFFER_MAX = 512;  // Hard cap on buffer size
    // Pre-reserve buffer once to avoid repeated heap growth/fragmentation on ESP32
    if (rxBuffer.capacity() < RX_BUFFER_MAX) {
        rxBuffer.reserve(RX_BUFFER_MAX);
    }
    while (xQueueReceive(bleDataQueue, &pkt, 0) == pdTRUE) {
        // NOTE: Proxy forwarding is done immediately in the BLE callback (forwardToProxyImmediate)
        // for minimal latency. We don't forward again here to avoid duplicate packets.
        
        // Accumulate and frame on 0xAA ... 0xAB so we don't choke on chunked notifications
        // Skip accumulation if buffer is at capacity (will be trimmed below)
        if (rxBuffer.size() < RX_BUFFER_MAX) {
            rxBuffer.insert(rxBuffer.end(), pkt.data, pkt.data + pkt.length);
        }
        latestPktTs = pkt.tsMs;
    }
    
    // Process the proxy queue to actually send data to connected apps (JBV1/V1 Companion)
    bleClient.processProxyQueue();
#endif
    
    // If no data accumulated, return
    if (rxBuffer.empty()) {
        return;
    }

    // Trim runaway buffers - more aggressive to prevent memory pressure
    // Normal V1 packets are <100 bytes, so 256 is plenty for recovery
    if (rxBuffer.size() > 256) {
        // Keep last 128 bytes (enough for one complete packet with framing)
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.end() - 128);
    }

    const size_t MIN_HEADER_SIZE = 6;
    const size_t MAX_PACKET_SIZE = 512;

    while (true) {
        auto dumpPacket = [&](const uint8_t* p, size_t len) {
            if (!DEBUG_LOGS) return;  // Skip packet dumps unless debug enabled
            SerialLog.printf("PKT id=0x%02X len=%u: ", p[3], static_cast<unsigned>(len));
            for (size_t i = 0; i < len; i++) {
                SerialLog.printf("%02X ", p[i]);
            }
            SerialLog.println();
        };

        auto startIt = std::find(rxBuffer.begin(), rxBuffer.end(), ESP_PACKET_START);
        if (startIt == rxBuffer.end()) {
            rxBuffer.clear();
            break;
        }
        if (startIt != rxBuffer.begin()) {
            rxBuffer.erase(rxBuffer.begin(), startIt);
            continue;
        }
        if (rxBuffer.size() < MIN_HEADER_SIZE) {
            break;
        }

        uint8_t lenField = rxBuffer[4];
        if (lenField == 0) {
            rxBuffer.erase(rxBuffer.begin());
            continue;
        }

        size_t packetSize = 6 + lenField;
        if (packetSize > MAX_PACKET_SIZE) {
            SerialLog.printf("WARNING: BLE packet too large (%u bytes) - resyncing\n", (unsigned)packetSize);
            rxBuffer.erase(rxBuffer.begin());
            continue;
        }
        if (rxBuffer.size() < packetSize) {
            break;
        }
        if (rxBuffer[packetSize - 1] != ESP_PACKET_END) {
            SerialLog.println("WARNING: Packet missing end marker - resyncing");
            rxBuffer.erase(rxBuffer.begin());
            continue;
        }

        // Parse directly from rxBuffer - no heap allocation
        const uint8_t* packetPtr = rxBuffer.data();
        
        lastRxMillis = millis();
        if (latestPktTs == 0) {
            latestPktTs = lastRxMillis;
        }

        // Check for user bytes response (0x12) - V1 settings pull
        if (packetSize >= 12 && packetPtr[3] == PACKET_ID_RESP_USER_BYTES) {
            // Payload starts at byte 5, length is 6 bytes
            uint8_t userBytes[6];
            memcpy(userBytes, &packetPtr[5], 6);
            DEBUG_LOGF("V1 user bytes raw: %02X %02X %02X %02X %02X %02X\n",
                userBytes[0], userBytes[1], userBytes[2], userBytes[3], userBytes[4], userBytes[5]);
            DEBUG_LOGF("  xBand=%d, kBand=%d, kaBand=%d, laser=%d\n",
                userBytes[0] & 0x01, (userBytes[0] >> 1) & 0x01, 
                (userBytes[0] >> 2) & 0x01, (userBytes[0] >> 3) & 0x01);
            bleClient.onUserBytesReceived(userBytes);
            v1ProfileManager.setCurrentSettings(userBytes);
            DEBUG_LOGLN("Received V1 user bytes!");
            rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + packetSize);
            continue;  // Don't pass to parser
        }

        // ALWAYS erase packet from buffer after attempting to parse
        // This prevents stale packets from accumulating when display updates are throttled
        uint8_t packetId = packetPtr[3];
        bool parseOk = parser.parse(packetPtr, packetSize);
        
        // Only count display/alert packets for success/failure metrics
        // Other packet types (heartbeats, ACKs, etc.) are expected to return false
        if (packetId == PACKET_ID_DISPLAY_DATA || packetId == PACKET_ID_ALERT_DATA) {
            if (parseOk) {
                PERF_INC(parseSuccesses);
            } else {
                PERF_INC(parseFailures);  // Actual parse error on a packet we care about
            }
        }

        // Debug: dump display/alert packets for capture/replay (throttled to 1 Hz per ID)
        if (packetPtr[3] == PACKET_ID_DISPLAY_DATA || packetPtr[3] == PACKET_ID_ALERT_DATA) {
            static unsigned long lastDump31 = 0;
            static unsigned long lastDump43 = 0;
            unsigned long dumpNow = millis();
            if ((packetPtr[3] == PACKET_ID_DISPLAY_DATA && dumpNow - lastDump31 > 1000) ||
                (packetPtr[3] == PACKET_ID_ALERT_DATA && dumpNow - lastDump43 > 1000)) {
                dumpPacket(packetPtr, packetSize);
                if (packetPtr[3] == PACKET_ID_DISPLAY_DATA) lastDump31 = dumpNow;
                else lastDump43 = dumpNow;
            }
        }
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + packetSize);
        
        if (parseOk) {
            if (isColorPreviewActive()) {
                continue;  // Keep preview on-screen briefly
            }
            DisplayState state = parser.getDisplayState();

            // Cache alert status to avoid repeated calls
            bool hasAlerts = parser.hasAlerts();

            // Recovery: display shows bands but no alert table arrived
            if (!hasAlerts && state.activeBands != BAND_NONE) {
                unsigned long gapNow = millis();
                if (gapNow - lastAlertGapRecoverMs > 50) {  // 50ms - quick recovery for lost alert packets
                    DEBUG_LOGLN("Alert gap: bands active but no alerts; re-requesting alert data");
                    parser.resetAlertAssembly();
                    bleClient.requestAlertData();
                    lastAlertGapRecoverMs = gapNow;
                }
            }

            // V1 is source of truth for mute state
            // Mute debounce: prevent flicker from rapid V1 state changes
            unsigned long muteNow = millis();
            if (state.muted != debouncedMuteState) {
                if (muteNow - lastMuteChangeMs > MUTE_DEBOUNCE_MS) {
                    debouncedMuteState = state.muted;
                    lastMuteChangeMs = muteNow;
                } else {
                    // Within debounce window - keep previous state
                    state.muted = debouncedMuteState;
                }
            }
            
            // Throttle display updates to prevent crashes
            unsigned long now = millis();
            if (now - lastDisplayDraw < DISPLAY_DRAW_MIN_MS) {
                continue; // Skip this draw
            }
            lastDisplayDraw = now;
            
            // Cache settings once for all volume/voice operations in this cycle
            const V1Settings& alertSettings = settingsManager.get();
            
            if (hasAlerts) {
                AlertData priority = parser.getPriorityAlert();
                int alertCount = parser.getAlertCount();
                const auto& currentAlerts = parser.getAllAlerts();
                
                displayMode = DisplayMode::LIVE;
                
                // Arm auto power-off once we've received actual V1 data
                if (!autoPowerOffArmed) {
                    autoPowerOffArmed = true;
                    SerialLog.println("[AutoPowerOff] Armed - V1 data received");
                }
                
                // GPS Lockout Check: Mute V1 and suppress voice for lockout zones
                // Display still shows alert (V1 is source of truth), but we auto-mute
                bool priorityInLockout = false;
                static bool lockoutMuteSent = false;  // Track if we've sent mute for current alert
                static uint32_t lastLockoutAlertId = 0xFFFFFFFF;  // Track which alert we muted
                
                if (gpsHandler.hasValidFix()) {
                    GPSFix fix = gpsHandler.getFix();
                    
                    // Check if priority alert is in a lockout zone
                    if (priority.isValid && priority.band != BAND_NONE) {
                        priorityInLockout = lockouts.shouldMuteAlert(fix.latitude, fix.longitude, priority.band);
                        uint32_t currentAlertId = makeAlertId(priority.band, (uint16_t)priority.frequency);
                        
                        // Auto-mute V1 if in lockout zone and not already muted
                        if (priorityInLockout && !state.muted) {
                            // Only send mute once per alert (avoid spamming)
                            if (!lockoutMuteSent || currentAlertId != lastLockoutAlertId) {
                                DEBUG_LOGF("[Lockout] Auto-muting V1 for lockout zone alert\n");
                                bleClient.setMute(true);
                                lockoutMuteSent = true;
                                lastLockoutAlertId = currentAlertId;
                                // Tell display this is a lockout mute (shows "LOCKOUT" instead of "MUTED")
                                display.setLockoutMuted(true);
                            }
                        } else if (!priorityInLockout) {
                            // Reset tracking when not in lockout
                            lockoutMuteSent = false;
                            display.setLockoutMuted(false);
                        }
                        
                        // Record alert for auto-learning (even if locked out)
                        bool isMoving = gpsHandler.isMoving();
                        uint8_t strength = getAlertBars(priority);
                        autoLockouts.recordAlert(fix.latitude, fix.longitude, priority.band,
                                                  (uint32_t)priority.frequency, strength, 0, isMoving);
                    }
                    
                    // Record all secondary alerts for auto-learning too
                    for (int i = 0; i < alertCount; i++) {
                        const AlertData& alert = currentAlerts[i];
                        if (!alert.isValid || alert.band == BAND_NONE) continue;
                        // Skip priority (already recorded above)
                        if (alert.band == priority.band && alert.frequency == priority.frequency) continue;
                        
                        uint8_t strength = getAlertBars(alert);
                        autoLockouts.recordAlert(fix.latitude, fix.longitude, alert.band,
                                                  (uint32_t)alert.frequency, strength, 0, gpsHandler.isMoving());
                    }
                }

                // Volume Fade: reduce V1 volume after X seconds of continuous unmuted alert
                // Only applies when not muted - muted alerts should not trigger volume changes
                if (alertSettings.alertVolumeFadeEnabled && !state.muted && !priorityInLockout) {
                    unsigned long now = millis();
                    uint16_t currentFreq = (uint16_t)priority.frequency;
                    
                    // Check if this is a new frequency we haven't seen during this fade session
                    bool isNewFrequency = true;
                    for (int i = 0; i < volumeFadeSeenCount; i++) {
                        if (volumeFadeSeenFreqs[i] == currentFreq) {
                            isNewFrequency = false;
                            break;
                        }
                    }
                    
                    // New frequency while faded -> restore volume, restart fade timer
                    if (volumeFadeActive && isNewFrequency) {
                        DEBUG_LOGF("[VolumeFade] New frequency %d! Restoring volume to %d\n", currentFreq, volumeFadeOriginalVol);
                        if (volumeFadeOriginalVol != 0xFF) {
                            bleClient.setVolume(volumeFadeOriginalVol, volumeFadeOriginalMuteVol);
                        }
                        // Restart fade timer but keep the same original volume baseline
                        volumeFadeAlertStartMs = now;
                        volumeFadeActive = false;
                        volumeFadeCommandSent = false;
                        // Add this new frequency to our seen list
                        if (volumeFadeSeenCount < MAX_FADE_SEEN_FREQS) {
                            volumeFadeSeenFreqs[volumeFadeSeenCount++] = currentFreq;
                        }
                    }
                    
                    // Start tracking on first alert of a session
                    if (volumeFadeAlertStartMs == 0) {
                        volumeFadeAlertStartMs = now;
                        // Capture the "true" original volume - if speed boost is active,
                        // use the pre-boost volume instead of current (boosted) volume
                        if (speedVolumeBoostActive && speedVolumeOriginalVol != 0xFF) {
                            volumeFadeOriginalVol = speedVolumeOriginalVol;
                            DEBUG_LOGF("[VolumeFade] Alert started, using pre-speed-boost volume: %d\n", volumeFadeOriginalVol);
                        } else {
                            volumeFadeOriginalVol = state.mainVolume;  // Capture current volume
                            DEBUG_LOGF("[VolumeFade] Alert started, original volume: %d\n", volumeFadeOriginalVol);
                        }
                        volumeFadeOriginalMuteVol = state.muteVolume;  // Capture muted volume too
                        volumeFadeActive = false;
                        volumeFadeCommandSent = false;
                        // Track this frequency as the first one in this session
                        volumeFadeSeenFreqs[0] = currentFreq;
                        volumeFadeSeenCount = 1;
                    }
                    
                    // Check if fade delay has elapsed
                    unsigned long fadeDelayMs = alertSettings.alertVolumeFadeDelaySec * 1000UL;
                    if (!volumeFadeCommandSent && (now - volumeFadeAlertStartMs) >= fadeDelayMs) {
                        // Time to fade - send reduced volume to V1
                        uint8_t fadeVol = alertSettings.alertVolumeFadeVolume;
                        // Don't fade if already at or below target volume
                        if (state.mainVolume > fadeVol) {
                            DEBUG_LOGF("[VolumeFade] Fading volume from %d to %d after %lums\n", 
                                       state.mainVolume, fadeVol, now - volumeFadeAlertStartMs);
                            if (bleClient.setVolume(fadeVol, volumeFadeOriginalMuteVol)) {
                                volumeFadeActive = true;
                            }
                        }
                        volumeFadeCommandSent = true;  // Don't retry even if failed
                    }
                } else if (alertSettings.alertVolumeFadeEnabled && (state.muted || priorityInLockout)) {
                    // Alert was muted or entered lockout - restore volume if we faded
                    if (volumeFadeActive && volumeFadeOriginalVol != 0xFF) {
                        DEBUG_LOGF("[VolumeFade] Alert muted/lockout - restoring volume to %d\n", volumeFadeOriginalVol);
                        bleClient.setVolume(volumeFadeOriginalVol, volumeFadeOriginalMuteVol);
                    }
                    // Reset tracking - will re-arm if alert unmutes
                    volumeFadeAlertStartMs = 0;
                    volumeFadeOriginalVol = 0xFF;
                    volumeFadeOriginalMuteVol = 0;
                    volumeFadeActive = false;
                    volumeFadeCommandSent = false;
                    volumeFadeSeenCount = 0;
                }

                // Voice alerts: announce new priority alert when no phone app connected
                // Skip if alert is muted on V1 - user has already acknowledged/dismissed it
                // Skip if alert is in a GPS lockout zone
                // Skip if at low speed (parking lot mode)
                bool muteForVolZero = alertSettings.muteVoiceIfVolZero && state.mainVolume == 0;
                bool muteForLowSpeed = isLowSpeedMuted();
                if (alertSettings.voiceAlertMode != VOICE_MODE_DISABLED && 
                    !muteForVolZero &&
                    !muteForLowSpeed &&
                    !state.muted &&  // Don't announce muted alerts
                    !priorityInLockout &&  // Don't announce lockout zone alerts
                    !bleClient.isProxyClientConnected() &&
                    priority.isValid &&
                    priority.band != BAND_NONE) {
                    
                    unsigned long now = millis();
                    uint16_t currentFreq = (uint16_t)priority.frequency;
                    // Check if this is a different alert (band OR frequency changed)
                    // This handles Laser correctly - even though freq=0, band change triggers new announcement
                    bool bandChanged = (priority.band != lastVoiceAlertBand);
                    bool frequencyChanged = (currentFreq != lastVoiceAlertFrequency);
                    bool alertChanged = bandChanged || frequencyChanged;
                    bool directionChanged = (priority.direction != lastVoiceAlertDirection);
                    bool cooldownPassed = (now - lastVoiceAlertTime >= VOICE_ALERT_COOLDOWN_MS);
                    
                    // Track priority stability for secondary alerts (use band+freq combo)
                    uint32_t currentAlertId = makeAlertId(priority.band, currentFreq);
                    static uint32_t lastPriorityAlertId = 0xFFFFFFFF;
                    if (currentAlertId != lastPriorityAlertId) {
                        lastPriorityAlertId = currentAlertId;
                        priorityStableSince = now;
                    }
                    
                    // Convert V1 direction to audio direction
                    // V1 uses bitmask (FRONT=1, SIDE=2, REAR=4), we simplify to primary direction
                    AlertDirection audioDir;
                    if (priority.direction & DIR_FRONT) {
                        audioDir = AlertDirection::AHEAD;
                    } else if (priority.direction & DIR_REAR) {
                        audioDir = AlertDirection::BEHIND;
                    } else {
                        audioDir = AlertDirection::SIDE;
                    }
                    
                    // Track bogey count changes
                    bool bogeyCountChanged = ((uint8_t)alertCount != lastVoiceAlertBogeyCount);
                    bool bogeyCountCooldownPassed = (now - lastVoiceAlertTime >= BOGEY_COUNT_COOLDOWN_MS);
                    
                    // PRIORITY ALERT ANNOUNCEMENTS
                    // Logic:
                    // - New alert (band or freq changed): full announcement based on voice mode setting
                    // - Same alert but direction changed: direction + bogey count if changed
                    // - Same alert, same direction, but bogey count changed: direction + new count (shorter cooldown)
                    bool priorityAnnounced = false;
                    if (alertChanged && cooldownPassed) {
                        // New alert - full announcement based on mode
                        // Reset direction change throttle for new alert
                        directionChangeCount = 0;
                        directionChangeWindowStart = now;
                        
                        AlertBand audioBand;
                        bool validBand = true;
                        switch (priority.band) {
                            case BAND_LASER: audioBand = AlertBand::LASER; break;
                            case BAND_KA:    audioBand = AlertBand::KA;    break;
                            case BAND_K:     audioBand = AlertBand::K;     break;
                            case BAND_X:     audioBand = AlertBand::X;     break;
                            default:         validBand = false;            break;
                        }
                        
                        if (validBand) {
                            DEBUG_LOGF("[VoiceAlert] New priority: band=%d freq=%u dir=%d mode=%d dirEnabled=%d alerts=%d\n", 
                                       (int)audioBand, currentFreq, (int)audioDir,
                                       (int)alertSettings.voiceAlertMode, alertSettings.voiceDirectionEnabled, alertCount);
                            play_frequency_voice(audioBand, currentFreq, audioDir,
                                                 alertSettings.voiceAlertMode, alertSettings.voiceDirectionEnabled,
                                                 alertSettings.announceBogeyCount ? (uint8_t)alertCount : 1);
                            lastVoiceAlertBand = priority.band;
                            lastVoiceAlertDirection = priority.direction;
                            lastVoiceAlertFrequency = currentFreq;
                            lastVoiceAlertBogeyCount = (uint8_t)alertCount;
                            lastVoiceAlertTime = now;
                            lastPriorityAnnouncementTime = now;
                            markAlertAnnounced(priority.band, currentFreq);
                            priorityAnnounced = true;
                        }
                    } else if (!alertChanged && directionChanged && cooldownPassed && 
                               alertSettings.voiceDirectionEnabled) {
                        // Same alert changed direction - announce direction + bogey count if changed
                        // But throttle if direction has been bouncing too much
                        
                        // Check if throttle window expired - reset counter
                        if (now - directionChangeWindowStart > DIRECTION_THROTTLE_WINDOW_MS) {
                            directionChangeCount = 0;
                            directionChangeWindowStart = now;
                        }
                        
                        // Increment change count
                        directionChangeCount++;
                        
                        // Only announce if under throttle limit
                        if (directionChangeCount <= DIRECTION_CHANGE_LIMIT) {
                            uint8_t bogeyCountToAnnounce = (alertSettings.announceBogeyCount && bogeyCountChanged) ? (uint8_t)alertCount : 0;
                            DEBUG_LOGF("[VoiceAlert] Direction change: freq=%u dir=%d bogeys=%d (was %d) [change %d/%d]\n", 
                                       currentFreq, (int)audioDir, alertCount, lastVoiceAlertBogeyCount,
                                       directionChangeCount, DIRECTION_CHANGE_LIMIT);
                            play_direction_only(audioDir, bogeyCountToAnnounce);
                            lastVoiceAlertTime = now;
                            lastPriorityAnnouncementTime = now;
                            priorityAnnounced = true;
                        } else {
                            DEBUG_LOGF("[VoiceAlert] Direction change THROTTLED: freq=%u changes=%d in window\n",
                                       currentFreq, directionChangeCount);
                        }
                        // Always update tracking even if throttled
                        lastVoiceAlertDirection = priority.direction;
                        lastVoiceAlertBogeyCount = (uint8_t)alertCount;
                    } else if (!alertChanged && !directionChanged && bogeyCountChanged && 
                               bogeyCountCooldownPassed && alertSettings.announceBogeyCount) {
                        // Same alert, same direction, but bogey count changed - announce direction + new count
                        // Uses shorter cooldown (2s) to be more responsive to count changes
                        DEBUG_LOGF("[VoiceAlert] Bogey count change: freq=%u dir=%d bogeys=%d (was %d)\n", 
                                   currentFreq, (int)audioDir, alertCount, lastVoiceAlertBogeyCount);
                        play_direction_only(audioDir, (uint8_t)alertCount);
                        lastVoiceAlertBogeyCount = (uint8_t)alertCount;
                        lastVoiceAlertTime = now;
                        lastPriorityAnnouncementTime = now;
                        priorityAnnounced = true;
                    }
                    
                    // SECONDARY ALERT ANNOUNCEMENTS
                    // Only if: master toggle enabled, priority stable, gap after priority announcement
                    // Secondary alerts are announced once when they appear - no direction/bogey updates
                    if (!priorityAnnounced && 
                        alertSettings.announceSecondaryAlerts &&
                        alertCount > 1 &&
                        (now - priorityStableSince >= PRIORITY_STABILITY_MS) &&
                        (now - lastPriorityAnnouncementTime >= POST_PRIORITY_GAP_MS)) {
                        
                        // Check non-priority alerts for unannounced frequencies
                        for (int i = 0; i < alertCount; i++) {
                            const AlertData& alert = currentAlerts[i];
                            if (!alert.isValid || alert.band == BAND_NONE) continue;
                            
                            uint16_t alertFreq = (uint16_t)alert.frequency;
                            
                            // Skip priority alert (compare band+freq combo, not just freq)
                            if (alert.band == priority.band && alertFreq == currentFreq) continue;
                            
                            // Skip if already announced
                            if (isAlertAnnounced(alert.band, alertFreq)) continue;
                            
                            // Check band filter
                            if (!isBandEnabledForSecondary(alert.band, alertSettings)) continue;
                            
                            // Announce this secondary alert
                            AlertBand audioBand;
                            bool validBand = true;
                            switch (alert.band) {
                                case BAND_LASER: audioBand = AlertBand::LASER; break;
                                case BAND_KA:    audioBand = AlertBand::KA;    break;
                                case BAND_K:     audioBand = AlertBand::K;     break;
                                case BAND_X:     audioBand = AlertBand::X;     break;
                                default:         validBand = false;            break;
                            }
                            
                            if (validBand) {
                                AlertDirection secDir;
                                if (alert.direction & DIR_FRONT) {
                                    secDir = AlertDirection::AHEAD;
                                } else if (alert.direction & DIR_REAR) {
                                    secDir = AlertDirection::BEHIND;
                                } else {
                                    secDir = AlertDirection::SIDE;
                                }
                                
                                DEBUG_LOGF("[VoiceAlert] Secondary: band=%d freq=%u dir=%d\n", 
                                           (int)audioBand, alertFreq, (int)secDir);
                                // Secondary alerts: same voice mode, but no bogey count (keep it brief)
                                play_frequency_voice(audioBand, alertFreq, secDir,
                                                     alertSettings.voiceAlertMode, alertSettings.voiceDirectionEnabled, 1);
                                markAlertAnnounced(alert.band, alertFreq);
                                lastVoiceAlertTime = now;  // Use same cooldown
                                break;  // Only announce one secondary per cycle
                            }
                        }
                    }
                    
                    // SMART THREAT ESCALATION: Track signal history and announce when weak signals ramp up
                    // Update all secondary alert histories and check for escalation triggers
                    if (alertSettings.announceSecondaryAlerts && alertCount > 1) {
                        // First pass: update all alert histories with current signal strengths
                        for (int i = 0; i < alertCount; i++) {
                            const AlertData& alert = currentAlerts[i];
                            if (!alert.isValid || alert.band == BAND_NONE) continue;
                            if (alert.band == BAND_LASER) continue;  // Laser excluded from smart tracking
                            
                            uint16_t alertFreq = (uint16_t)alert.frequency;
                            uint8_t bars = getAlertBars(alert);
                            updateAlertHistory(alert.band, alertFreq, bars, now);
                        }
                        
                        // Cleanup stale histories (alerts that disappeared)
                        cleanupStaleHistories(now);
                        
                        // Second pass: check for any alert triggering smart escalation
                        // Only announce if cooldown has passed
                        if (now - lastVoiceAlertTime >= BOGEY_COUNT_COOLDOWN_MS) {
                            for (int i = 0; i < alertCount; i++) {
                                const AlertData& alert = currentAlerts[i];
                                if (!alert.isValid || alert.band == BAND_NONE) continue;
                                if (alert.band == BAND_LASER) continue;
                                
                                uint16_t alertFreq = (uint16_t)alert.frequency;
                                
                                // Skip priority alert
                                if (alert.band == priority.band && alertFreq == currentFreq) continue;
                                
                                // Skip if muted
                                if (state.muted) continue;
                                
                                // Skip if band not enabled for secondary
                                if (!isBandEnabledForSecondary(alert.band, alertSettings)) continue;
                                
                                // Check smart escalation criteria (pass alertCount for bogey filter)
                                if (shouldAnnounceThreatEscalation(alert.band, alertFreq, (uint8_t)alertCount, now)) {
                                    // Mark as announced before building the message
                                    markThreatEscalationAnnounced(alert.band, alertFreq);
                                    
                                    // Convert to audio types
                                    AlertBand audioBand;
                                    switch (alert.band) {
                                        case BAND_KA: audioBand = AlertBand::KA; break;
                                        case BAND_K:  audioBand = AlertBand::K; break;
                                        case BAND_X:  audioBand = AlertBand::X; break;
                                        default: continue;  // Skip unknown bands
                                    }
                                    
                                    AlertDirection alertDir;
                                    if (alert.direction & DIR_FRONT) {
                                        alertDir = AlertDirection::AHEAD;
                                    } else if (alert.direction & DIR_REAR) {
                                        alertDir = AlertDirection::BEHIND;
                                    } else {
                                        alertDir = AlertDirection::SIDE;
                                    }
                                    
                                    // Count all alerts by direction for breakdown
                                    uint8_t aheadCount = 0, behindCount = 0, sideCount = 0;
                                    for (int j = 0; j < alertCount; j++) {
                                        const AlertData& a = currentAlerts[j];
                                        if (!a.isValid || a.band == BAND_NONE) continue;
                                        
                                        if (a.direction & DIR_FRONT) {
                                            aheadCount++;
                                        } else if (a.direction & DIR_REAR) {
                                            behindCount++;
                                        } else {
                                            sideCount++;
                                        }
                                    }
                                    
                                    uint8_t total = aheadCount + behindCount + sideCount;
                                    DEBUG_LOGF("[VoiceAlert] Smart threat escalation: %s %u %s - %d bogeys (%d ahead, %d behind, %d side)\n",
                                               alert.band == BAND_KA ? "Ka" : (alert.band == BAND_K ? "K" : "X"),
                                               alertFreq, 
                                               (alertDir == AlertDirection::AHEAD) ? "ahead" : 
                                                   ((alertDir == AlertDirection::BEHIND) ? "behind" : "side"),
                                               total, aheadCount, behindCount, sideCount);
                                    
                                    // Play full announcement: "[Band] [freq] [direction] [N] bogeys, [X] ahead, [Y] behind"
                                    play_threat_escalation(audioBand, alertFreq, alertDir, 
                                                          total, aheadCount, behindCount, sideCount);
                                    lastVoiceAlertTime = now;
                                    break;  // Only one escalation announcement per cycle
                                }
                            }
                        }
                    }
                }

                // V1 is source of truth for mute state - no auto-unmute logic
                // Display just shows what V1 reports

                // Update display FIRST for lowest latency
                // Pass all alerts for multi-alert card display
                V1_PERF_START();
                display.update(priority, currentAlerts.data(), alertCount, state);
                V1_DISPLAY_END("display.update(alerts)");
                
                // Save priority alert for potential persistence when alert clears
                persistedAlert = priority;
                alertPersistenceActive = false;  // Cancel any active persistence
                alertClearedTime = 0;
                
            } else {
                // No alerts from V1
                displayMode = DisplayMode::IDLE;
                
                // GPS Passthrough Recording: Track when we pass through lockout zones without alerts
                // This helps demote false lockouts over time
                static unsigned long lastPassthroughRecordMs = 0;
                if (gpsHandler.hasValidFix() && gpsHandler.isMoving()) {
                    unsigned long now = millis();
                    // Only record passthrough every 5 seconds to avoid spamming
                    if (now - lastPassthroughRecordMs > 5000) {
                        GPSFix fix = gpsHandler.getFix();
                        autoLockouts.recordPassthrough(fix.latitude, fix.longitude);
                        lastPassthroughRecordMs = now;
                    }
                }
                
                // Reset voice alert tracking when alerts clear
                lastVoiceAlertBand = BAND_NONE;
                lastVoiceAlertDirection = DIR_NONE;
                lastVoiceAlertFrequency = 0xFFFF;
                lastVoiceAlertBogeyCount = 0;
                clearAnnouncedAlerts();
                priorityStableSince = 0;
                
                // Volume Fade: restore original volume when alerts clear
                // Restore if we either:
                // 1. Actually faded (volumeFadeActive), OR
                // 2. Had captured an original but fade was pending (volumeFadeAlertStartMs != 0)
                //    This handles cases where alert cleared before fade delay elapsed
                if (alertSettings.alertVolumeFadeEnabled && volumeFadeOriginalVol != 0xFF) {
                    // Only send restore command if volume actually changed
                    DisplayState restoreState = parser.getDisplayState();
                    if (restoreState.mainVolume != volumeFadeOriginalVol) {
                        DEBUG_LOGF("[VolumeFade] Alerts cleared - restoring volume from %d to %d\n", 
                                   restoreState.mainVolume, volumeFadeOriginalVol);
                        bleClient.setVolume(volumeFadeOriginalVol, volumeFadeOriginalMuteVol);
                    }
                }
                // Reset fade tracking regardless of whether fade was active
                volumeFadeAlertStartMs = 0;
                volumeFadeOriginalVol = 0xFF;
                volumeFadeOriginalMuteVol = 0;
                volumeFadeActive = false;
                volumeFadeCommandSent = false;
                volumeFadeSeenCount = 0;  // Clear seen frequencies for next session
                
                // Reset speed boost state - let it re-evaluate on next check
                // This ensures speed boost re-engages properly after fade restores volume
                speedVolumeBoostActive = false;
                speedVolumeOriginalVol = 0xFF;
                
                // Alert persistence: show last alert in grey for configured duration
                const V1Settings& s = settingsManager.get();
                uint8_t persistSec = settingsManager.getSlotAlertPersistSec(s.activeSlot);
                unsigned long now = millis();
                
                // Clear persistence on profile slot change (handles web UI profile switches)
                static int lastPersistenceSlot = -1;
                if (s.activeSlot != lastPersistenceSlot) {
                    lastPersistenceSlot = s.activeSlot;
                    persistedAlert = AlertData();
                    alertPersistenceActive = false;
                    alertClearedTime = 0;
                }
                
                if (persistSec > 0 && persistedAlert.isValid) {
                    // Start persistence timer on transition from alerts to no-alerts
                    if (alertClearedTime == 0) {
                        alertClearedTime = now;
                        alertPersistenceActive = true;
                    }
                    
                    // Check if persistence timer still active
                    unsigned long persistMs = persistSec * 1000UL;
                    if (alertPersistenceActive && (now - alertClearedTime) < persistMs) {
                        // Show persisted alert in dark grey
                        V1_PERF_START();
                        display.updatePersisted(persistedAlert, state);
                        V1_DISPLAY_END("display.persisted");
                    } else {
                        // Persistence expired - show normal resting
                        if (alertPersistenceActive) {
                            alertPersistenceActive = false;
                        }
                        V1_PERF_START();
                        display.update(state);
                        V1_DISPLAY_END("display.resting");
                    }
                } else {
                    // Persistence disabled or no valid persisted alert
                    alertPersistenceActive = false;
                    alertClearedTime = 0;
                    V1_PERF_START();
                    display.update(state);
                    V1_DISPLAY_END("display.resting");
                }
            }
        }
    }
}

void setup() {
    // Wait for USB to stabilize after upload
    delay(100);
    
    // Create BLE data queue early - before any BLE operations
    bleDataQueue = xQueueCreate(64, sizeof(BLEDataPacket));  // Increased from 32 to handle web server blocking
    rxBuffer.reserve(256);  // Trimmed at 256 bytes anyway; normal packets <100 bytes
    
// Backlight is handled in display.begin() (inverted PWM for Waveshare)

#if defined(PIN_POWER_ON) && PIN_POWER_ON >= 0
    // Cut panel power until we intentionally bring it up
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, LOW);
#endif

    Serial.begin(115200);
    delay(200);  // Reduced from 500ms - brief delay for serial init
    
    SerialLog.println("\n===================================");
    SerialLog.println("V1 Gen2 Simple Display");
    SerialLog.println("Firmware: " FIRMWARE_VERSION);
    SerialLog.print("Board: ");
    SerialLog.println(DISPLAY_NAME);
    
    // Check reset reason - if firmware flash, clear BLE bonds
    esp_reset_reason_t resetReason = esp_reset_reason();
    SerialLog.printf("Reset reason: %d ", resetReason);
    if (resetReason == ESP_RST_SW || resetReason == ESP_RST_UNKNOWN) {
        SerialLog.println("(SW/Upload - will clear BLE bonds for clean reconnect)");
    } else if (resetReason == ESP_RST_POWERON) {
        SerialLog.println("(Power-on)");
    } else {
        SerialLog.printf("(Other: %d)\n", resetReason);
    }
    SerialLog.println("===================================\n");
    
    // Initialize battery manager EARLY - needs to latch power on if running on battery
    // This must happen before any long-running init to prevent shutdown
#if defined(DISPLAY_WAVESHARE_349)
    batteryManager.begin();
    
    // DEBUG: Simulate battery for testing UI (uncomment to test)
    // batteryManager.simulateBattery(3800);  // 60% battery
#endif
    
    // Initialize display
    if (!display.begin()) {
        SerialLog.println("Display initialization failed!");
        while (1) delay(1000);
    }
    
    // Display battery status after display is initialized
#if defined(DISPLAY_WAVESHARE_349)
    SerialLog.printf("[Battery] Power source: %s\n", 
                     batteryManager.isOnBattery() ? "BATTERY" : "USB");
    SerialLog.printf("[Battery] Icon display: %s\n",
                     batteryManager.hasBattery() ? "YES" : "NO");
    if (batteryManager.hasBattery()) {
        SerialLog.printf("[Battery] Voltage: %dmV (%d%%)\n", 
                      batteryManager.getVoltageMillivolts(), 
                      batteryManager.getPercentage());
    }
#endif

    // Brief delay to ensure panel is fully cleared before enabling backlight
    delay(100);

    // Initialize settings BEFORE showing any styled screens (need displayStyle setting)
    settingsManager.begin();

    // Show boot splash only on true power-on (not crash reboots or firmware uploads)
    if (resetReason == ESP_RST_POWERON) {
        // True cold boot - show splash
        display.showBootSplash();
        delay(2500);  // Show logo for 2.5 seconds
    }
    // After splash (or skipping it), show scanning screen until connected
    display.showScanning();
    
    // Show the current profile indicator
    display.drawProfileIndicator(settingsManager.get().activeSlot);
    
    // If you want to show the demo, call display.showDemo() manually elsewhere (e.g., via a button or menu)
    
    lastLvTick = millis();

    // Mount storage (SD if available, else LittleFS) for profiles and settings
    SerialLog.println("[Setup] Mounting storage...");
    if (storageManager.begin()) {
        SerialLog.printf("[Setup] Storage ready: %s\n", storageManager.statusText().c_str());
        v1ProfileManager.begin(storageManager.getFilesystem());
        audio_init_sd();  // Initialize SD-based frequency voice audio
        
        // Validate profile references in auto-push slots
        // Clear references to profiles that don't exist
        settingsManager.validateProfileReferences(v1ProfileManager);
        
        // Retry settings restore now that SD is mounted
        // (settings.begin() runs before storage, so restore may have failed)
        if (settingsManager.checkAndRestoreFromSD()) {
            // Settings were restored from SD - update display with restored brightness
            display.setBrightness(settingsManager.get().brightness);
        }
        
        // Initialize lockout managers (requires storage to be ready)
        autoLockouts.setLockoutManager(&lockouts);
        lockouts.loadFromJSON("/v1profiles/lockouts.json");
        autoLockouts.loadFromJSON("/v1profiles/auto_lockouts.json");
        SerialLog.printf("[Setup] Loaded %d lockout zones, %d learning clusters\n",
                        lockouts.getLockoutCount(), autoLockouts.getClusterCount());
        
        // Initialize GPS if enabled in settings (static allocation - just call begin())
        if (settingsManager.isGpsEnabled()) {
            SerialLog.println("[Setup] GPS enabled - initializing...");
            gpsHandler.begin();
            
            // Defer camera database loading until after V1 connects
            // This keeps boot fast - camera loading can take several seconds for large DBs
            if (storageManager.isSDCard()) {
                cameraLoadPending = true;
                SerialLog.println("[Setup] Camera database will load after V1 connects");
            }
        } else {
            SerialLog.println("[Setup] GPS disabled in settings");
        }
        
        // Initialize OBD if enabled in settings
        if (settingsManager.isObdEnabled()) {
            SerialLog.println("[Setup] OBD enabled - initializing...");
            obdHandler.begin();
        } else {
            SerialLog.println("[Setup] OBD disabled in settings");
        }
    } else {
        SerialLog.println("[Setup] Storage unavailable - profiles will be disabled");
    }

    // Initialize debug logger after storage is mounted
    debugLogger.begin();
    {
        DebugLogConfig cfg = settingsManager.getDebugLogConfig();
        DebugLogFilter filter{cfg.alerts, cfg.wifi, cfg.ble, cfg.gps, cfg.obd, cfg.system, cfg.display, cfg.perfMetrics};
        debugLogger.setFilter(filter);
    }
    debugLogger.setEnabled(settingsManager.get().enableDebugLogging);
    if (debugLogger.isEnabledFor(DebugLogCategory::System)) {
        debugLogger.logf(DebugLogCategory::System, "Debug logging enabled (storage=%s)", storageManager.statusText().c_str());
    }

    SerialLog.println("==============================");
    SerialLog.println("WiFi Configuration:");
    SerialLog.printf("  enableWifi: %s\n", settingsManager.get().enableWifi ? "YES" : "NO");
    SerialLog.printf("  wifiMode: %d\n", settingsManager.get().wifiMode);
    SerialLog.printf("  apSSID: %s\n", settingsManager.get().apSSID.c_str());
    SerialLog.println("==============================");
    
    // WiFi startup behavior - either auto-start or wait for BOOT button
    if (settingsManager.get().enableWifiAtBoot) {
        SerialLog.println("[WiFi] Auto-start enabled (dev setting)");
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "WiFi auto-start enabled (dev setting)");
        }
    } else {
        SerialLog.println("[WiFi] Off by default - start with BOOT long-press");
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "WiFi auto-start disabled (manual BOOT press required)");
        }
    }
    
    // Initialize touch handler early - before BLE to avoid interleaved logs
    SerialLog.println("Initializing touch handler...");
    if (touchHandler.begin(17, 18, AXS_TOUCH_ADDR, -1)) {
        SerialLog.println("Touch handler initialized successfully");
    } else {
        SerialLog.println("WARNING: Touch handler failed to initialize - continuing anyway");
    }
    
    // Initialize BOOT button (GPIO 0) for brightness adjustment
#if defined(DISPLAY_WAVESHARE_349)
    pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
    brightnessAdjustValue = settingsManager.get().brightness;
    volumeAdjustValue = settingsManager.get().voiceVolume;
    display.setBrightness(brightnessAdjustValue);  // Apply saved brightness
    audio_set_volume(volumeAdjustValue);           // Apply saved voice volume
    SerialLog.printf("[Settings] Applied saved brightness: %d, voice volume: %d\n", 
                    brightnessAdjustValue, volumeAdjustValue);
#endif

#ifndef REPLAY_MODE
    // Initialize BLE client with proxy settings from preferences
    const V1Settings& bleSettings = settingsManager.get();
    SerialLog.printf("Starting BLE (proxy: %s, name: %s)\n", 
                  bleSettings.proxyBLE ? "enabled" : "disabled",
                  bleSettings.proxyName.c_str());

    // Initialize BLE stack first (required before any BLE operations)
    if (!bleClient.initBLE(bleSettings.proxyBLE, bleSettings.proxyName.c_str())) {
        SerialLog.println("BLE initialization failed!");
        display.showDisconnected();
        while (1) delay(1000);
    }
    
    // Start normal scanning
    SerialLog.println("Starting BLE scan for V1...");
    if (!bleClient.begin(bleSettings.proxyBLE, bleSettings.proxyName.c_str())) {
        SerialLog.println("BLE scan failed to start!");
        display.showDisconnected();
        while (1) delay(1000);
    }
    
    // Register data callback
    bleClient.onDataReceived(onV1Data);
    
    // Register V1 connection callback for auto-push
    bleClient.onV1Connected(onV1Connected);
#else
    SerialLog.println("[REPLAY_MODE] BLE disabled - using packet replay for UI testing");
#endif
    
    // Auto-start WiFi if enabled in dev settings
    if (settingsManager.get().enableWifiAtBoot) {
        SerialLog.println("[WiFi] Auto-start enabled - starting AP now...");
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "WiFi auto-start: starting AP");
        }
        startWifi();
        SerialLog.println("Setup complete - BLE scanning, WiFi auto-started");
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "Setup complete (WiFi auto-started)");
        }
    } else {
        SerialLog.println("Setup complete - BLE scanning, WiFi off until BOOT long-press");
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "Setup complete (WiFi idle until BOOT long-press)");
        }
    }
}

void loop() {
    // Periodic perf metrics logging (every 60s if enabled)
    static unsigned long lastPerfLogMs = 0;
    if (settingsManager.get().logPerfMetrics && debugLogger.isEnabledFor(DebugLogCategory::PerfMetrics)) {
        unsigned long now = millis();
        if (now - lastPerfLogMs >= 60000) {
            lastPerfLogMs = now;
            debugLogger.logf(DebugLogCategory::PerfMetrics, 
                "PerfMetrics: rx=%lu qDrop=%lu parseOK=%lu parseFail=%lu disc=%lu reconn=%lu",
                perfCounters.rxPackets, perfCounters.queueDrops, 
                perfCounters.parseSuccesses, perfCounters.parseFailures,
                perfCounters.disconnects, perfCounters.reconnects);
        }
    }

    // Update BLE indicator: show when V1 is connected; color reflects JBV1 connection
    // Third param is "receiving" - true if we got V1 packets in last 2s (heartbeat visual)
    bool bleReceiving = (millis() - lastRxMillis) < 2000;
    display.setBLEProxyStatus(bleClient.isConnected(), bleClient.isProxyClientConnected(), bleReceiving);
    
    // Process audio amp timeout (disables amp after 3s of inactivity)
    audio_process_amp_timeout();

    // Drive color preview (band cycle) first; skip other updates if active
    if (colorPreviewActive) {
        driveColorPreview();
    } else if (colorPreviewEnded) {
        // Preview finished - restore normal display with fresh V1 data
        colorPreviewEnded = false;
        // Force full redraw and immediately update with current parser state
        display.forceNextRedraw();
        if (bleClient.isConnected()) {
            // Immediately refresh with current V1 state (don't wait for next packet)
            DisplayState state = parser.getDisplayState();
            if (parser.hasAlerts()) {
                AlertData priority = parser.getPriorityAlert();
                const auto& alerts = parser.getAllAlerts();
                display.update(priority, alerts.data(), parser.getAlertCount(), state);
            } else {
                display.update(state);
            }
        } else {
            display.showResting();
        }
    }

    // Process battery manager (updates cached readings at 1Hz, handles power button)
#if defined(DISPLAY_WAVESHARE_349)
    batteryManager.update();
    batteryManager.processPowerButton();
    
    // Check for critical battery - auto shutdown to prevent damage
    static bool lowBatteryWarningShown = false;
    static unsigned long criticalBatteryTime = 0;
    
    if (batteryManager.isOnBattery() && batteryManager.hasBattery()) {
        if (batteryManager.isCritical()) {
            // Show warning once, then shutdown after 5 seconds
            if (!lowBatteryWarningShown) {
                Serial.println("[Battery] CRITICAL - showing low battery warning");
                display.showLowBattery();
                lowBatteryWarningShown = true;
                criticalBatteryTime = millis();
            } else if (millis() - criticalBatteryTime > 5000) {
                Serial.println("[Battery] CRITICAL - auto shutdown to protect battery");
                batteryManager.powerOff();
            }
        } else {
            lowBatteryWarningShown = false;  // Reset if voltage recovers
        }
    }
    
    // BOOT button handling: short press = brightness adjust, long press = AP toggle
    bool bootPressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);  // Active low
    unsigned long now = millis();

    if (bootPressed && !bootWasPressed) {
        bootPressStart = now;
        wifiToggleTriggered = false;  // Reset for new press
    }

    // Check for long press while button is still held (immediate feedback)
    if (bootPressed && !wifiToggleTriggered && !brightnessAdjustMode) {
        unsigned long pressDuration = now - bootPressStart;
        if (pressDuration >= AP_TOGGLE_LONG_PRESS_MS) {
            wifiToggleTriggered = true;  // Prevent re-triggering
            bool wasActive = wifiManager.isSetupModeActive();
            if (wasActive) {
                wifiManager.stopSetupMode(true);
                SerialLog.println("[WiFi] AP stopped (button long press)");
            } else {
                startWifi();
                SerialLog.println("[WiFi] AP started (button long press)");
            }
            display.drawWiFiIndicator();
            display.flush();
        }
    }

    if (!bootPressed && bootWasPressed) {
        unsigned long pressDuration = now - bootPressStart;
        if (pressDuration >= BOOT_DEBOUNCE_MS && !wifiToggleTriggered) {
            // Only handle short press actions (not already handled by long press)
            if (brightnessAdjustMode) {
                // Exit settings adjustment mode and save both values
                brightnessAdjustMode = false;
                settingsManager.updateBrightness(brightnessAdjustValue);
                settingsManager.updateVoiceVolume(volumeAdjustValue);
                settingsManager.save();
                audio_set_volume(volumeAdjustValue);  // Apply new volume
                display.hideBrightnessSlider();
                // Restore display with fresh V1 data
                display.forceNextRedraw();
                if (bleClient.isConnected()) {
                    DisplayState state = parser.getDisplayState();
                    if (parser.hasAlerts()) {
                        AlertData priority = parser.getPriorityAlert();
                        const auto& alerts = parser.getAllAlerts();
                        display.update(priority, alerts.data(), parser.getAlertCount(), state);
                    } else {
                        display.update(state);
                    }
                } else {
                    display.showResting();
                }
                SerialLog.printf("[Settings] Saved brightness: %d, volume: %d\n", brightnessAdjustValue, volumeAdjustValue);
            } else {
                // Enter settings adjustment mode (short press)
                brightnessAdjustMode = true;
                brightnessAdjustValue = settingsManager.get().brightness;
                volumeAdjustValue = settingsManager.get().voiceVolume;
                activeSlider = 0;  // Start with brightness selected
                display.showSettingsSliders(brightnessAdjustValue, volumeAdjustValue);
                SerialLog.printf("[Settings] Entering adjustment mode (brightness: %d, volume: %d)\n", 
                                brightnessAdjustValue, volumeAdjustValue);
            }
        }
    }

    bootWasPressed = bootPressed;
    
    // If in settings adjustment mode, handle touch for both sliders
    if (brightnessAdjustMode) {
        int16_t touchX, touchY;
        if (touchHandler.getTouchPoint(touchX, touchY)) {
            // Map touch X to slider value (40 to 600 pixels = slider area)
            // Note: Touch coordinates are inverted relative to display
            const int sliderX = 40;
            const int sliderWidth = SCREEN_WIDTH - 80;  // 560 pixels
            
            // Determine which slider was touched based on Y coordinate
            int touchedSlider = display.getActiveSliderFromTouch(touchY);
            
            if (touchedSlider >= 0 && touchX >= sliderX && touchX <= sliderX + sliderWidth) {
                activeSlider = touchedSlider;
                
                if (activeSlider == 0) {
                    // Brightness slider (range 80-255)
                    // Touch X is inverted: swipe right = lower X, swipe left = higher X
                    int newLevel = 255 - (((touchX - sliderX) * 175) / sliderWidth);
                    if (newLevel < 80) newLevel = 80;
                    if (newLevel > 255) newLevel = 255;
                    
                    if (newLevel != brightnessAdjustValue) {
                        brightnessAdjustValue = newLevel;
                        display.updateSettingsSliders(brightnessAdjustValue, volumeAdjustValue, activeSlider);
                    }
                } else if (activeSlider == 1) {
                    // Voice volume slider (range 0-100)
                    int newVolume = 100 - (((touchX - sliderX) * 100) / sliderWidth);
                    if (newVolume < 0) newVolume = 0;
                    if (newVolume > 100) newVolume = 100;
                    
                    if (newVolume != volumeAdjustValue) {
                        volumeAdjustValue = newVolume;
                        audio_set_volume(volumeAdjustValue);  // Apply immediately for feedback
                        display.updateSettingsSliders(brightnessAdjustValue, volumeAdjustValue, activeSlider);
                        lastVolumeChangeMs = now;  // Track for test playback
                    }
                }
            }
        } else {
            // No touch - check if volume was recently changed and play test voice
            if (lastVolumeChangeMs > 0 && (now - lastVolumeChangeMs) >= VOLUME_TEST_DEBOUNCE_MS) {
                play_test_voice();
                lastVolumeChangeMs = 0;  // Reset so we don't play again
            }
        }
        return;  // Skip normal loop processing while in settings mode
    }
#endif

    // Check for touch - single tap for mute (only with active alert), triple-tap for profile cycle (only without alert)
    int16_t touchX, touchY;
    bool hasActiveAlert = parser.hasAlerts();

    // Helper to toggle mute state - sends command to V1
    // V1 is source of truth - display updates when V1 reports new state
    auto performMuteToggle = [&](const char* reason) {
        if (!hasActiveAlert) {
            SerialLog.println("MUTE BLOCKED: No active alert to mute");
            return;
        }

        DisplayState state = parser.getDisplayState();
        bool currentMuted = state.muted;
        bool newMuted = !currentMuted;

        SerialLog.printf("Mute: %s -> Sending: %s (%s)\n", 
                      currentMuted ? "MUTED" : "UNMUTED",
                      newMuted ? "MUTE_ON" : "MUTE_OFF",
                      reason);

        // Send mute command to V1
        bool cmdSent = bleClient.setMute(newMuted);
        SerialLog.printf("Mute command sent: %s\n", cmdSent ? "OK" : "FAIL");
    };
    
    if (touchHandler.getTouchPoint(touchX, touchY)) {
        unsigned long now = millis();
        
        // Debounce check
        if (now - lastTapTime >= TAP_DEBOUNCE_MS) {
            // Check if this tap is within the window of previous taps
            if (now - lastTapTime <= TAP_WINDOW_MS) {
                tapCount++;
            } else {
                // Window expired, start new count
                tapCount = 1;
            }
            lastTapTime = now;
            
            SerialLog.printf("Tap detected: count=%d, x=%d, y=%d, hasAlert=%d\n", tapCount, touchX, touchY, hasActiveAlert);

            // With an active alert, toggle mute immediately on first tap for responsiveness
            if (hasActiveAlert && tapCount == 1) {
                performMuteToggle("immediate tap");
                tapCount = 0;  // reset so follow-up taps start fresh
                return;        // skip triple-tap handling
            }
            
            // Check for triple-tap to cycle profiles (ONLY when no active alert)
            if (tapCount >= 3) {
                tapCount = 0;  // Reset tap count
                
                if (hasActiveAlert) {
                    SerialLog.println("PROFILE CHANGE BLOCKED: Active alert present - tap to mute instead");
                } else {
                    // Cycle to next profile slot: 0 -> 1 -> 2 -> 0
                    const V1Settings& s = settingsManager.get();
                    int newSlot = (s.activeSlot + 1) % 3;
                    settingsManager.setActiveSlot(newSlot);
                    displayMode = DisplayMode::IDLE;
                    
                    // Clear persisted alert state on profile change
                    persistedAlert = AlertData();
                    alertPersistenceActive = false;
                    
                    const char* slotNames[] = {"Default", "Highway", "Comfort"};
                    SerialLog.printf("PROFILE CHANGE: Switched to '%s' (slot %d)\n", slotNames[newSlot], newSlot);
                    
                    // Update display to show new profile
                    display.drawProfileIndicator(newSlot);
                    
                    // If connected to V1 and auto-push is enabled, push the new profile
                    // Note: Use startAutoPush directly to avoid device-specific override
                    if (bleClient.isConnected() && s.autoPushEnabled) {
                        SerialLog.println("Pushing new profile to V1...");
                        startAutoPush(newSlot);
                    }
                }
            }
        }
    } else {
        // No touch - check if we have a pending single/double tap to process as mute toggle
        unsigned long now = millis();
        if (tapCount > 0 && tapCount < 3 && (now - lastTapTime > TAP_WINDOW_MS)) {
            // Window expired with 1-2 taps - treat as mute toggle (ONLY with active alert)
            SerialLog.printf("Processing %d tap(s) as mute toggle\n", tapCount);
            tapCount = 0;
            performMuteToggle("deferred tap");
        }
    }
    
#ifndef REPLAY_MODE
    // Process BLE events
    bleClient.process();
#endif
    
    // Process queued BLE data (safe for SPI - runs in main loop context)
    // In REPLAY_MODE, this injects test packets; otherwise processes BLE queue
    processBLEData();

    // Drive auto-push state machine (non-blocking)
    processAutoPush();

    // Process WiFi/web server
    wifiManager.process();
    
    // Process GPS updates (if enabled - static allocation uses isEnabled())
    if (gpsHandler.isEnabled()) {
        gpsHandler.update();
        
        // Auto-disable GPS if module not detected after timeout
        if (gpsHandler.isDetectionComplete() && !gpsHandler.isModuleDetected()) {
            SerialLog.println("[GPS] Module not detected - disabling GPS");
            gpsHandler.end();  // Static allocation - use end() instead of delete
            settingsManager.setGpsEnabled(false);
        }
    }
    
    // Camera alert checking (requires GPS with valid fix)
    const V1Settings& camSettings = settingsManager.get();
    if (camSettings.cameraAlertsEnabled && cameraManager.isLoaded() && gpsHandler.isEnabled()) {
        unsigned long now = millis();
        
        // Regional cache refresh check - only when GPS has valid fix AND background load is complete
        // Skip cache rebuild while background loading is in progress
        if (now - lastCacheCheckMs >= CACHE_CHECK_INTERVAL_MS && gpsHandler.hasValidFix() && !cameraManager.isBackgroundLoading()) {
            lastCacheCheckMs = now;
            
            GPSFix cacheFix = gpsHandler.getFix();
            bool needsRefresh = cameraManager.needsCacheRefresh(cacheFix.latitude, cacheFix.longitude, CACHE_REFRESH_DIST_MILES);
            
            // Also refresh if cache is older than 30 minutes
            if (!needsRefresh && cameraManager.hasRegionalCache()) {
                // Force refresh every 30 minutes while driving
                static unsigned long lastCacheRefreshMs = 0;
                if (lastCacheRefreshMs == 0) lastCacheRefreshMs = now;
                if (now - lastCacheRefreshMs >= CACHE_REFRESH_INTERVAL_MS) {
                    needsRefresh = true;
                    lastCacheRefreshMs = now;
                }
            }
            
            if (needsRefresh) {
                Serial.printf("[Camera] Building regional cache at %.4f, %.4f\n", cacheFix.latitude, cacheFix.longitude);
                if (cameraManager.buildRegionalCache(cacheFix.latitude, cacheFix.longitude, CACHE_RADIUS_MILES)) {
                    // Save to LittleFS for fast boot next time
                    cameraManager.saveRegionalCache(&LittleFS, "/cameras_cache.json");
                }
            }
        }
        
        if (now - lastCameraCheckMs >= CAMERA_CHECK_INTERVAL_MS) {
            lastCameraCheckMs = now;
            
            // Only check when we have a valid GPS fix
            if (gpsHandler.hasValidFix()) {
                GPSFix fix = gpsHandler.getFix();
                float lat = fix.latitude;
                float lon = fix.longitude;
                float heading = fix.heading_deg;
                float alertRadius = static_cast<float>(camSettings.cameraAlertDistanceM);
                
                // Update camera type filters from settings
                cameraManager.setEnabledTypes(
                    camSettings.cameraAlertRedLight,
                    camSettings.cameraAlertSpeed,
                    camSettings.cameraAlertALPR
                );
                
                NearbyCameraResult nearestCamera;
                bool cameraFound = cameraManager.getClosestCamera(lat, lon, heading, alertRadius, nearestCamera);
                
                // Only alert when approaching the camera (forward-looking)
                if (cameraFound && nearestCamera.isApproaching) {
                    // Check if this is a new alert (not already alerting this camera)
                    float distFromLastAlert = CameraManager::haversineDistance(
                        lat, lon, lastCameraAlertLat, lastCameraAlertLon);
                    
                    bool isNewAlert = !cameraAlertActive || 
                                      (distFromLastAlert > CAMERA_ALERT_COOLDOWN_M);
                    
                    if (isNewAlert && !cameraAlertActive) {
                        // New camera alert - start alert
                        cameraAlertActive = true;
                        cameraAlertStartMs = now;
                        currentCameraAlert = nearestCamera;
                        lastCameraAlertLat = nearestCamera.camera.latitude;
                        lastCameraAlertLon = nearestCamera.camera.longitude;
                        
                        Serial.printf("[Camera] ALERT: %s at %.0fm AHEAD\n",
                            nearestCamera.camera.getTypeName(),
                            nearestCamera.distance_m);
                        
                        // Play voice alert if enabled
                        if (camSettings.cameraAudioEnabled) {
                            // Convert camera type to voice type
                            CameraAlertType voiceType = CameraAlertType::SPEED;  // Default
                            CameraType camType = nearestCamera.camera.getCameraType();
                            switch (camType) {
                                case CameraType::RedLightCamera:
                                    voiceType = CameraAlertType::RED_LIGHT;
                                    break;
                                case CameraType::RedLightAndSpeed:
                                    voiceType = CameraAlertType::RED_LIGHT_SPEED;
                                    break;
                                case CameraType::SpeedCamera:
                                    voiceType = CameraAlertType::SPEED;
                                    break;
                                case CameraType::ALPR:
                                    voiceType = CameraAlertType::ALPR;
                                    break;
                                default:
                                    voiceType = CameraAlertType::SPEED;
                                    break;
                            }
                            play_camera_voice(voiceType);
                        }
                    }
                    
                    // Update distance while alert is active
                    currentCameraAlert.distance_m = nearestCamera.distance_m;
                    currentCameraAlert.isApproaching = nearestCamera.isApproaching;
                    
                } else if (cameraAlertActive) {
                    // No approaching camera in range - clear alert
                    Serial.println("[Camera] Alert cleared - camera out of range or passed");
                    cameraAlertActive = false;
                }
            }
        }
    }
    
    // Process OBD updates (always runs - state machine handles enabled/disabled)
    // Check for delayed auto-connect trigger
    if (obdAutoConnectAt != 0 && millis() >= obdAutoConnectAt) {
        obdAutoConnectAt = 0;  // Clear trigger
        SerialLog.println("[OBD] V1 settle delay complete - attempting OBD auto-connect");
        obdHandler.tryAutoConnect();
    }
    obdHandler.update();
    
    // Deferred camera database loading - runs once after V1 connects
    // Strategy: Load regional cache first (instant, ~100ms), then background load full DB
    // This allows camera alerts to work immediately while the 70K+ record DB loads in background
    if (cameraLoadPending && !cameraLoadComplete && bleClient.isConnected()) {
        cameraLoadPending = false;
        cameraLoadComplete = true;  // Mark complete to prevent re-entry
        
        SerialLog.println("[Camera] Initializing camera alerts...");
        fs::FS* sdFs = storageManager.getFilesystem();
        
        // Step 1: Try to load regional cache from LittleFS first (fast path ~100ms)
        // This gives us immediate camera alerts without waiting for full DB
        bool hasCachedData = cameraManager.loadRegionalCache(&LittleFS, "/cameras_cache.json");
        if (hasCachedData) {
            SerialLog.printf("[Camera] Regional cache loaded: %d cameras (instant alerts ready)\n", 
                            cameraManager.getRegionalCacheCount());
        }
        
        // Step 2: Start background loading of full database from SD card
        // The background task runs at low priority and yields frequently to not block BLE/display
        if (sdFs && (sdFs->exists("/alpr.json") || sdFs->exists("/redlight_cam.json") || sdFs->exists("/speed_cam.json"))) {
            // Set filesystem for background loader
            cameraManager.setFilesystem(sdFs);
            
            if (cameraManager.startBackgroundLoad()) {
                SerialLog.println("[Camera] Background database load started (won't block V1)");
            } else {
                // Background load failed to start - fall back to synchronous
                SerialLog.println("[Camera] Background load failed, using synchronous load...");
                if (cameraManager.begin(sdFs)) {
                    SerialLog.printf("[Camera] Database loaded: %d cameras\n", 
                                    cameraManager.getCameraCount());
                }
            }
        } else if (!hasCachedData) {
            SerialLog.println("[Camera] No camera database or cache found");
        }
    }
    
    // Monitor background camera load progress (optional: could show on display)
    static bool bgLoadLoggedComplete = false;
    static bool bgLoadCacheBuilt = false;  // Track if we've rebuilt cache after background load
    if (cameraManager.isBackgroundLoading()) {
        // Optionally log progress periodically
        static unsigned long lastProgressLog = 0;
        if (millis() - lastProgressLog > 10000) {  // Every 10 seconds
            SerialLog.printf("[Camera] Background load: %d%% (%d cameras)\n",
                            cameraManager.getLoadProgress(), cameraManager.getLoadedCount());
            lastProgressLog = millis();
        }
        bgLoadLoggedComplete = false;
        bgLoadCacheBuilt = false;
    } else if (!bgLoadLoggedComplete && cameraManager.getCameraCount() > 0) {
        // Log once when background load completes
        bgLoadLoggedComplete = true;
        SerialLog.printf("[Camera] Background load complete: %d cameras ready\n",
                        cameraManager.getCameraCount());
        
        // Build/refresh regional cache now that full database is loaded
        // This ensures we have the most up-to-date cache for the current location
        if (!bgLoadCacheBuilt && gpsHandler.hasValidFix()) {
            bgLoadCacheBuilt = true;
            GPSFix fix = gpsHandler.getFix();
            SerialLog.printf("[Camera] Rebuilding regional cache with full database at %.4f, %.4f\n",
                            fix.latitude, fix.longitude);
            if (cameraManager.buildRegionalCache(fix.latitude, fix.longitude, CACHE_RADIUS_MILES)) {
                cameraManager.saveRegionalCache(&LittleFS, "/cameras_cache.json");
                SerialLog.printf("[Camera] Regional cache updated: %d cameras\n",
                                cameraManager.getRegionalCacheCount());
            }
        }
    }

    // Check if V1 has active alerts - determines if camera shows as main display or card
    bool v1HasActiveAlerts = parser.hasAlerts();
    
    // Update camera alert display (if active)
    // Test mode takes priority
    if (cameraTestActive) {
        if (millis() < cameraTestEndMs) {
            // Simulate countdown
            cameraTestDistance -= 5.0f;  // Decrease distance to simulate approach
            if (cameraTestDistance < 50.0f) cameraTestDistance = 50.0f;
            
            const V1Settings& dispSettings = settingsManager.get();
            display.updateCameraAlert(
                true,
                cameraTestTypeName,
                cameraTestDistance,
                true,
                dispSettings.colorCameraAlert,
                v1HasActiveAlerts  // Show as card if V1 has alerts
            );
        } else {
            // Test complete
            cameraTestActive = false;
            display.clearCameraAlert();
            Serial.println("[Camera] Test alert ended");
        }
    } else if (cameraAlertActive) {
        const V1Settings& dispSettings = settingsManager.get();
        display.updateCameraAlert(
            true,
            currentCameraAlert.camera.getShortTypeName(),
            currentCameraAlert.distance_m,
            currentCameraAlert.isApproaching,
            dispSettings.colorCameraAlert,
            v1HasActiveAlerts  // Show as card if V1 has alerts
        );
    } else {
        display.clearCameraAlert();
    }
    
    // Speed-based volume: boost V1 volume at highway speeds
    // Check periodically (not every loop) to avoid spamming V1
    // NOTE: If volume fade is active/tracking, let fade handle volume control
    static unsigned long lastSpeedVolumeCheck = 0;
    static bool loggedSpeedVolumeSettings = false;
    
    if (bleClient.isConnected() && now - lastSpeedVolumeCheck > 2000) {  // Check every 2s
        lastSpeedVolumeCheck = now;
        const V1Settings& spdSettings = settingsManager.get();
        
        // One-time log of settings for debugging
        if (!loggedSpeedVolumeSettings) {
            SerialLog.printf("[SpeedVolume] Settings: enabled=%d threshold=%d boost=%d\n",
                spdSettings.speedVolumeEnabled, spdSettings.speedVolumeThresholdMph, spdSettings.speedVolumeBoost);
            SerialLog.printf("[LowSpeedMute] Settings: enabled=%d threshold=%d\n",
                spdSettings.lowSpeedMuteEnabled, spdSettings.lowSpeedMuteThresholdMph);
            loggedSpeedVolumeSettings = true;
        }
        
        // Skip speed volume adjustments if volume fade has captured the volume
        // (either during active fade or while tracking for potential fade)
        bool fadeTakingControl = volumeFadeOriginalVol != 0xFF;
        
        if (spdSettings.speedVolumeEnabled && !fadeTakingControl) {
            float speedMph = getCurrentSpeedMph();
            
            // Periodic debug: log speed check every 30s when at highway speeds
            static unsigned long lastSpeedDebugLog = 0;
            if (speedMph >= 40 && now - lastSpeedDebugLog > 30000) {
                SerialLog.printf("[SpeedVolume] Check: speed=%.0f threshold=%d shouldBoost=%d boostActive=%d\n",
                    speedMph, spdSettings.speedVolumeThresholdMph, 
                    speedMph >= spdSettings.speedVolumeThresholdMph, speedVolumeBoostActive);
                lastSpeedDebugLog = now;
            }
            
            bool shouldBoost = speedMph >= spdSettings.speedVolumeThresholdMph;
            
            if (shouldBoost && !speedVolumeBoostActive) {
                // Speed crossed threshold - boost volume
                DisplayState state = parser.getDisplayState();
                speedVolumeOriginalVol = state.mainVolume;
                uint8_t boostedVol = std::min((int)state.mainVolume + spdSettings.speedVolumeBoost, 9);
                if (boostedVol > state.mainVolume) {
                    SerialLog.printf("[SpeedVolume] Speed %.0f mph >= %d threshold - boosting volume %d -> %d\n",
                               speedMph, spdSettings.speedVolumeThresholdMph, state.mainVolume, boostedVol);
                    bleClient.setVolume(boostedVol, state.muteVolume);
                    speedVolumeBoostActive = true;
                }
            } else if (!shouldBoost && speedVolumeBoostActive) {
                // Speed dropped below threshold - restore original volume
                DisplayState state = parser.getDisplayState();
                if (speedVolumeOriginalVol != 0xFF && state.mainVolume != speedVolumeOriginalVol) {
                    SerialLog.printf("[SpeedVolume] Speed %.0f mph < %d threshold - restoring volume to %d\n",
                               speedMph, spdSettings.speedVolumeThresholdMph, speedVolumeOriginalVol);
                    bleClient.setVolume(speedVolumeOriginalVol, state.muteVolume);
                }
                speedVolumeBoostActive = false;
                speedVolumeOriginalVol = 0xFF;
            }
        }
    }
    
    // Periodic auto-lockout maintenance (promotion/demotion checks)
    // Only run every 30 seconds to minimize overhead
    static unsigned long lastAutoLockoutUpdate = 0;
    if (now - lastAutoLockoutUpdate > 30000) {
        autoLockouts.update();
        lastAutoLockoutUpdate = now;
    }
    
    // Periodic save of auto-lockout learning data (every 5 minutes)
    // Prevents losing learning progress on unexpected power loss
    static unsigned long lastAutoLockoutSave = 0;
    if (now - lastAutoLockoutSave > 300000) {  // 5 minutes
        if (autoLockouts.getClusterCount() > 0) {
            autoLockouts.saveToJSON("/v1profiles/auto_lockouts.json");
        }
        lastAutoLockoutSave = now;
    }
    
    // Update display periodically
    now = millis();
    
    if (now - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
        lastDisplayUpdate = now;
        if (!isColorPreviewActive()) {
            // Check connection status
            static bool wasConnected = false;
            bool isConnected = bleClient.isConnected();
            
            // Only trigger state changes on actual transitions
            if (isConnected != wasConnected) {
                if (isConnected) {
                    display.showResting(); // stay on resting view until data arrives
                    SerialLog.println("V1 connected!");
                    // Cancel any pending auto power-off timer
                    if (autoPowerOffTimerStart != 0) {
                        SerialLog.println("[AutoPowerOff] Timer cancelled - V1 reconnected");
                        autoPowerOffTimerStart = 0;
                    }
                    // Note: autoPowerOffArmed is set when we receive actual V1 data, not just on connect
                } else {
                    // Reset stale state from previous connection
                    PacketParser::resetPriorityState();
                    PacketParser::resetAlertCountTracker();
                    parser.resetAlertAssembly();
                    V1Display::resetChangeTracking();;
                    display.showScanning();
                    SerialLog.println("V1 disconnected - Scanning...");
                    displayMode = DisplayMode::IDLE;
                    
                    // Start auto power-off timer if enabled and was previously connected
                    const V1Settings& s = settingsManager.get();
                    if (autoPowerOffArmed && s.autoPowerOffMinutes > 0) {
                        autoPowerOffTimerStart = millis();
                        SerialLog.printf("[AutoPowerOff] Timer started: %d minutes\n", s.autoPowerOffMinutes);
                    }
                }
                wasConnected = isConnected;
            }

            // If connected but not seeing traffic, re-request alert data periodically
            static unsigned long lastReq = 0;
            if (isConnected && (now - lastRxMillis) > 2000 && (now - lastReq) > 1000) {
                DEBUG_LOGLN("No data recently; re-requesting alert data...");
                bleClient.requestAlertData();
                lastReq = now;
            }

            // Periodically refresh indicators (WiFi/battery) even when scanning
            if (!isConnected) {
                display.drawWiFiIndicator();
                display.drawBatteryIndicator();
                display.flush();  // Push canvas changes to physical display
            }
        }
    }
    
    // Status update (print to serial) - only when DEBUG_LOGS enabled
    if (DEBUG_LOGS && now - lastStatusUpdate >= STATUS_UPDATE_MS) {
        lastStatusUpdate = now;
        
        if (bleClient.isConnected()) {
            DisplayState state = parser.getDisplayState();
            if (parser.hasAlerts()) {
                SerialLog.printf("Active alerts: %d\n", parser.getAlertCount());
            }
        }
    }
    
    // Auto power-off timer check
    if (autoPowerOffTimerStart != 0) {
        const V1Settings& s = settingsManager.get();
        unsigned long currentMs = millis();
        unsigned long elapsedMs = currentMs - autoPowerOffTimerStart;
        unsigned long timeoutMs = (unsigned long)s.autoPowerOffMinutes * 60UL * 1000UL;
        
        if (elapsedMs >= timeoutMs) {
            SerialLog.printf("[AutoPowerOff] Timer expired after %d minutes - powering off\n", s.autoPowerOffMinutes);
            autoPowerOffTimerStart = 0;  // Clear timer
            batteryManager.powerOff();
        }
    }

    // Short FreeRTOS delay to yield CPU without capping loop at ~200 Hz
    vTaskDelay(pdMS_TO_TICKS(1));
}

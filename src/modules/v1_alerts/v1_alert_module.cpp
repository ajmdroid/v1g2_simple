// V1 Alert Module - Implementation
// Handles V1 radar alert detection, display, audio, and clearing

#include "v1_alert_module.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "settings.h"

V1AlertModule::V1AlertModule() {
    // Constructor - dependencies set in begin()
}

void V1AlertModule::begin(V1BLEClient* ble, PacketParser* pParser, V1Display* disp, SettingsManager* sett) {
    bleClient = ble;
    parser = pParser;
    display = disp;
    settings = sett;
    initialized = true;
    
    Serial.println("[V1AlertModule] Initialized with dependencies");
}

void V1AlertModule::update() {
    // Main update loop - will be populated during migration
    // For now, does nothing (main.cpp still handles everything)
    if (!initialized) return;
}

void V1AlertModule::end() {
    // Cleanup - will be populated during migration
    initialized = false;
}

// Static utility: Get signal bars for alert based on direction
uint8_t V1AlertModule::getAlertBars(const AlertData& a) {
    if (a.direction & DIR_FRONT) return a.frontStrength;
    if (a.direction & DIR_REAR) return a.rearStrength;
    return (a.frontStrength > a.rearStrength) ? a.frontStrength : a.rearStrength;
}

// Static utility: Create unique alert ID from band and frequency
// Alert ID = (band << 16) | frequency - ensures Laser (freq=0) is unique per band
uint32_t V1AlertModule::makeAlertId(Band band, uint16_t freq) {
    return ((uint32_t)band << 16) | freq;
}

// Static utility: Check if band is enabled for secondary alert announcements
bool V1AlertModule::isBandEnabledForSecondary(Band band, const V1Settings& settings) {
    switch (band) {
        case BAND_LASER: return settings.secondaryLaser;
        case BAND_KA:    return settings.secondaryKa;
        case BAND_K:     return settings.secondaryK;
        case BAND_X:     return settings.secondaryX;
        default:         return false;
    }
}

// Announced alert tracking - check if alert has been announced
bool V1AlertModule::isAlertAnnounced(Band band, uint16_t freq) {
    uint32_t id = makeAlertId(band, freq);
    for (int i = 0; i < announcedAlertCount; i++) {
        if (announcedAlertIds[i] == id) return true;
    }
    return false;
}

// Announced alert tracking - mark alert as announced
void V1AlertModule::markAlertAnnounced(Band band, uint16_t freq) {
    uint32_t id = makeAlertId(band, freq);
    if (announcedAlertCount < MAX_ANNOUNCED_ALERTS && !isAlertAnnounced(band, freq)) {
        announcedAlertIds[announcedAlertCount++] = id;
    }
}

// Announced alert tracking - clear all announced alerts
void V1AlertModule::clearAnnouncedAlerts() {
    announcedAlertCount = 0;
    memset(announcedAlertIds, 0, sizeof(announcedAlertIds));
}

// Combined clear - resets all tracking when alerts clear
void V1AlertModule::clearAllAlertState() {
    clearAnnouncedAlerts();
    clearAlertHistories();
    resetLastAnnounced();
    resetPriorityStability();
}

// ============================================================================
// Alert History Tracking - Smart Threat Escalation
// ============================================================================

// Find existing alert history by ID
V1AlertModule::AlertHistory* V1AlertModule::findAlertHistory(uint32_t alertId) {
    for (int i = 0; i < alertHistoryCount; i++) {
        if (alertHistories[i].alertId == alertId) {
            return &alertHistories[i];
        }
    }
    return nullptr;
}

// Get or create alert history entry
V1AlertModule::AlertHistory* V1AlertModule::getOrCreateAlertHistory(uint32_t alertId, unsigned long now) {
    // Find existing
    AlertHistory* h = findAlertHistory(alertId);
    if (h) return h;
    
    // Create new if space available
    if (alertHistoryCount < MAX_ALERT_HISTORIES) {
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

// Update alert history with current signal strength
void V1AlertModule::updateAlertHistory(Band band, uint16_t freq, uint8_t bars, unsigned long now) {
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

// Remove stale histories (alerts that disappeared)
void V1AlertModule::cleanupStaleHistories(unsigned long now) {
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
bool V1AlertModule::shouldAnnounceThreatEscalation(Band band, uint16_t freq, uint8_t totalBogeys, unsigned long now) {
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
        Serial.printf("[ThreatEscalation] TRIGGERED: band=%d freq=%u wasWeak=%d cur=%d strongFor=%lums bogeys=%d\n",
                   (int)band, freq, h->wasWeak, h->currentBars, now - h->strongSinceMs, totalBogeys);
        return true;
    }
    return false;
}

// Mark threat escalation as announced
void V1AlertModule::markThreatEscalationAnnounced(Band band, uint16_t freq) {
    uint32_t alertId = makeAlertId(band, freq);
    AlertHistory* h = findAlertHistory(alertId);
    if (h) {
        h->escalationAnnounced = true;
    }
}

// Clear all alert histories
void V1AlertModule::clearAlertHistories() {
    alertHistoryCount = 0;
    memset(alertHistories, 0, sizeof(alertHistories));
}

// Reset direction change throttle - call when priority alert changes
void V1AlertModule::resetDirectionThrottle(unsigned long now) {
    directionChangeCount = 0;
    directionChangeWindowStart = now;
}

// Check if direction change should be throttled
// Returns true if announcement should be skipped (too many changes in window)
// Also handles window expiry reset and incrementing the counter
bool V1AlertModule::shouldThrottleDirectionChange(unsigned long now) {
    // Check if throttle window expired - reset counter
    if (now - directionChangeWindowStart > DIRECTION_THROTTLE_WINDOW_MS) {
        directionChangeCount = 0;
        directionChangeWindowStart = now;
    }
    
    // Increment change count
    directionChangeCount++;
    
    // Return true if over limit (should throttle)
    return directionChangeCount > DIRECTION_CHANGE_LIMIT;
}

// Update priority stability tracking - call when checking priority alert
void V1AlertModule::updatePriorityStability(uint32_t currentAlertId, unsigned long now) {
    if (currentAlertId != lastPriorityAlertId) {
        // Priority changed - reset stability timer
        lastPriorityAlertId = currentAlertId;
        priorityStableSince = now;
    }
}

// Mark that priority was just announced
void V1AlertModule::markPriorityAnnounced(unsigned long now) {
    lastPriorityAnnouncementTime = now;
}

// Reset priority stability (call when all alerts clear)
void V1AlertModule::resetPriorityStability() {
    priorityStableSince = 0;
    lastPriorityAlertId = 0xFFFFFFFF;
}

// Check if secondary alert can be announced
// Requires: priority stable for PRIORITY_STABILITY_MS and gap of POST_PRIORITY_GAP_MS since announcement
bool V1AlertModule::canAnnounceSecondary(unsigned long now) const {
    return (priorityStableSince > 0) &&
           (now - priorityStableSince >= PRIORITY_STABILITY_MS) &&
           (now - lastPriorityAnnouncementTime >= POST_PRIORITY_GAP_MS);
}

// ============================================================================
// Voice Alert "Last Announced" Tracking
// ============================================================================

bool V1AlertModule::hasAlertChanged(Band band, uint16_t freq) const {
    return (band != lastVoiceAlertBand) || (freq != lastVoiceAlertFrequency);
}

bool V1AlertModule::hasDirectionChanged(Direction dir) const {
    return dir != lastVoiceAlertDirection;
}

bool V1AlertModule::hasCooldownPassed(unsigned long now) const {
    return (now - lastVoiceAlertTime >= VOICE_ALERT_COOLDOWN_MS);
}

bool V1AlertModule::hasBogeyCountCooldownPassed(unsigned long now) const {
    return (now - lastVoiceAlertTime >= BOGEY_COUNT_COOLDOWN_MS);
}

bool V1AlertModule::hasBogeyCountChanged(uint8_t count) const {
    return count != lastVoiceAlertBogeyCount;
}

void V1AlertModule::updateLastAnnounced(Band band, Direction dir, uint16_t freq, uint8_t bogeyCount, unsigned long now) {
    lastVoiceAlertBand = band;
    lastVoiceAlertDirection = dir;
    lastVoiceAlertFrequency = freq;
    lastVoiceAlertBogeyCount = bogeyCount;
    lastVoiceAlertTime = now;
}

void V1AlertModule::updateLastAnnouncedDirection(Direction dir, uint8_t bogeyCount) {
    lastVoiceAlertDirection = dir;
    lastVoiceAlertBogeyCount = bogeyCount;
}

void V1AlertModule::updateLastAnnouncedTime(unsigned long now) {
    lastVoiceAlertTime = now;
}

void V1AlertModule::resetLastAnnounced() {
    lastVoiceAlertBand = BAND_NONE;
    lastVoiceAlertDirection = DIR_NONE;
    lastVoiceAlertFrequency = 0xFFFF;
    lastVoiceAlertBogeyCount = 0;
}

// ============================================================================
// Alert Persistence - shows last alert briefly after V1 clears it
// ============================================================================

void V1AlertModule::setPersistedAlert(const AlertData& alert) {
    persistedAlert = alert;
    alertPersistenceActive = false;  // Cancel any active persistence
    alertClearedTime = 0;
}

void V1AlertModule::startPersistence(unsigned long now) {
    // Only start timer on first call (transition from alerts to no-alerts)
    if (persistedAlert.isValid && alertClearedTime == 0) {
        alertClearedTime = now;
        alertPersistenceActive = true;
    }
}

void V1AlertModule::clearPersistence() {
    persistedAlert = AlertData();
    alertPersistenceActive = false;
    alertClearedTime = 0;
}

bool V1AlertModule::shouldShowPersisted(unsigned long now, unsigned long persistMs) const {
    return alertPersistenceActive && (now - alertClearedTime) < persistMs;
}

// Voice Module - Voice announcement decision logic

#include "voice_module.h"
#include "settings.h"
#ifndef UNIT_TEST
#include "perf_metrics.h"
#define VOICE_PERF_INC(counter) PERF_INC(counter)
#else
#define VOICE_PERF_INC(counter) do { } while (0)
#endif

#ifdef UNIT_TEST
#include "../../test/mocks/ble_client.h"
#else
#include "ble_client.h"
#endif

// ============================================================================
// Constructor and Initialization
// ============================================================================

VoiceModule::VoiceModule() {
    // Dependencies set in begin()
}

void VoiceModule::begin(SettingsManager* sett, V1BLEClient* ble) {
    settings = sett;
    bleClient = ble;
    
    Serial.println("[VoiceModule] Initialized");
}

// ============================================================================
// Static Utilities
// ============================================================================

uint8_t VoiceModule::getAlertBars(const AlertData& a) {
    if (a.direction & DIR_FRONT) return a.frontStrength;
    if (a.direction & DIR_REAR) return a.rearStrength;
    return (a.frontStrength > a.rearStrength) ? a.frontStrength : a.rearStrength;
}

uint32_t VoiceModule::makeAlertId(Band band, uint16_t freq) {
    return ((uint32_t)band << 16) | freq;
}

bool VoiceModule::isBandEnabledForSecondary(Band band, const V1Settings& settings) {
    switch (band) {
        case BAND_LASER: return settings.secondaryLaser;
        case BAND_KA:    return settings.secondaryKa;
        case BAND_K:     return settings.secondaryK;
        case BAND_X:     return settings.secondaryX;
        default:         return false;
    }
}

AlertDirection VoiceModule::toAudioDirection(Direction dir) {
    if (dir & DIR_FRONT) {
        return AlertDirection::AHEAD;
    } else if (dir & DIR_REAR) {
        return AlertDirection::BEHIND;
    } else {
        return AlertDirection::SIDE;
    }
}

// ============================================================================
// Local Helpers
// ============================================================================

static AlertBand toAudioBand(Band band) {
    switch (band) {
        case BAND_LASER: return AlertBand::LASER;
        case BAND_KA:    return AlertBand::KA;
        case BAND_K:     return AlertBand::K;
        case BAND_X:     return AlertBand::X;
        default:         return AlertBand::KA;
    }
}

static bool isValidAnnounceBand(Band band) {
    return band == BAND_LASER || band == BAND_KA || band == BAND_K || band == BAND_X;
}

// ============================================================================
// Main Decision Method
// ============================================================================

VoiceAction VoiceModule::process(const VoiceContext& ctx) {
    VoiceAction action;  // Default = NONE
    
    // -------------------------------------------------------------------------
    // Early Exit Checks
    // -------------------------------------------------------------------------
    
    if (!settings) return action;
    const V1Settings& s = settings->get();
    
    // Voice alerts disabled
    if (s.voiceAlertMode == VOICE_MODE_DISABLED) return action;
    
    // Mute voice if V1 volume is zero (optional setting)
    if (s.muteVoiceIfVolZero && ctx.mainVolume == 0) return action;
    
    // V1 is muted - user has acknowledged/dismissed the alert
    if (ctx.isMuted) return action;
    
    // Alert is in a suppression zone
    if (ctx.isSuppressed) return action;
    
    // Phone app is connected - let app handle voice
    if (ctx.isProxyConnected) return action;
    
    // No valid priority alert
    if (!ctx.priority || ctx.priority->band == BAND_NONE) return action;
    
    // -------------------------------------------------------------------------
    // Priority Alert Logic
    // -------------------------------------------------------------------------
    
    const AlertData& priority = *ctx.priority;
    uint16_t currentFreq = (uint16_t)priority.frequency;
    
    // Query current state
    bool alertChanged = hasAlertChanged(priority.band, currentFreq);
    bool directionChanged = hasDirectionChanged(priority.direction);
    bool directionKnown = (priority.direction != DIR_NONE);
    bool cooldownPassed = hasCooldownPassed(ctx.now);
    bool bogeyCountChanged = hasBogeyCountChanged((uint8_t)ctx.alertCount);
    bool bogeyCountCooldownPassed = hasBogeyCountCooldownPassed(ctx.now);
    
    // Track priority stability for secondary alerts
    uint32_t currentAlertId = makeAlertId(priority.band, currentFreq);
    updatePriorityStability(currentAlertId, ctx.now);
    
    // Convert direction for audio
    AlertDirection audioDir = toAudioDirection(priority.direction);
    
    // Case 1: New Alert (band or frequency changed)
    if (alertChanged && cooldownPassed) {
        if (!isValidAnnounceBand(priority.band)) return action;
        
        resetDirectionThrottle(ctx.now);
        
        action.type = VoiceAction::Type::ANNOUNCE_PRIORITY;
        action.band = toAudioBand(priority.band);
        action.freq = currentFreq;
        action.dir = audioDir;
        action.bogeyCount = s.announceBogeyCount ? (uint8_t)ctx.alertCount : 1;
        
        updateLastAnnounced(priority.band, priority.direction, currentFreq, (uint8_t)ctx.alertCount, ctx.now);
        markPriorityAnnounced(ctx.now);
        markAlertAnnounced(priority.band, currentFreq);
        VOICE_PERF_INC(voiceAnnouncePriority);
        
        return action;
    }
    
    // Case 2: Direction Changed (same alert)
    // Ignore transient DIR_NONE direction drops to avoid noisy "side" chatter.
    if (!alertChanged && directionChanged && cooldownPassed && s.voiceDirectionEnabled && directionKnown) {
        bool throttled = shouldThrottleDirectionChange(ctx.now);
        updateLastAnnouncedDirection(priority.direction, (uint8_t)ctx.alertCount);
        
        if (throttled) {
            VOICE_PERF_INC(voiceDirectionThrottled);
            return action;
        }
        
        action.type = VoiceAction::Type::ANNOUNCE_DIRECTION;
        action.dir = audioDir;
        action.bogeyCount = (s.announceBogeyCount && bogeyCountChanged) ? (uint8_t)ctx.alertCount : 0;
        
        updateLastAnnouncedTime(ctx.now);
        markPriorityAnnounced(ctx.now);
        VOICE_PERF_INC(voiceAnnounceDirection);
        
        return action;
    }
    
    // Case 3: Bogey Count Changed (same alert, same direction)
    if (!alertChanged && !directionChanged && bogeyCountChanged && 
        bogeyCountCooldownPassed && s.announceBogeyCount) {
        uint8_t previousBogeyCount = getLastBogeyCount();
        
        action.type = VoiceAction::Type::ANNOUNCE_DIRECTION;
        action.dir = audioDir;
        action.bogeyCount = (uint8_t)ctx.alertCount;
        
        updateLastAnnouncedDirection(priority.direction, (uint8_t)ctx.alertCount);
        updateLastAnnouncedTime(ctx.now);
        markPriorityAnnounced(ctx.now);
        VOICE_PERF_INC(voiceAnnounceDirection);
        
        return action;
    }
    
    // -------------------------------------------------------------------------
    // Secondary Alert Logic
    // -------------------------------------------------------------------------
    
    if (s.announceSecondaryAlerts && ctx.alertCount > 1 && canAnnounceSecondary(ctx.now)) {
        for (int i = 0; i < ctx.alertCount; i++) {
            const AlertData& alert = ctx.alerts[i];
            if (!alert.isValid || alert.band == BAND_NONE) continue;
            
            uint16_t alertFreq = (uint16_t)alert.frequency;
            
            // Skip priority alert
            if (alert.band == priority.band && alertFreq == currentFreq) continue;
            
            // Skip if already announced
            if (isAlertAnnounced(alert.band, alertFreq)) continue;
            
            // Check band filter
            if (!isBandEnabledForSecondary(alert.band, s)) continue;
            
            if (!isValidAnnounceBand(alert.band)) continue;
            
            action.type = VoiceAction::Type::ANNOUNCE_SECONDARY;
            action.band = toAudioBand(alert.band);
            action.freq = alertFreq;
            action.dir = toAudioDirection(alert.direction);
            action.bogeyCount = 1;
            
            markAlertAnnounced(alert.band, alertFreq);
            updateLastAnnouncedTime(ctx.now);
            VOICE_PERF_INC(voiceAnnounceSecondary);
            
            return action;
        }
    }
    
    // -------------------------------------------------------------------------
    // Smart Threat Escalation
    // -------------------------------------------------------------------------
    
    if (s.announceSecondaryAlerts && ctx.alertCount > 1) {
        // Update all alert histories
        for (int i = 0; i < ctx.alertCount; i++) {
            const AlertData& alert = ctx.alerts[i];
            if (!alert.isValid || alert.band == BAND_NONE) continue;
            if (alert.band == BAND_LASER) continue;
            
            uint16_t alertFreq = (uint16_t)alert.frequency;
            uint8_t bars = getAlertBars(alert);
            updateAlertHistory(alert.band, alertFreq, bars, ctx.now);
        }
        
        cleanupStaleHistories(ctx.now);
        
        // Check for escalation triggers
        if (hasBogeyCountCooldownPassed(ctx.now)) {
            for (int i = 0; i < ctx.alertCount; i++) {
                const AlertData& alert = ctx.alerts[i];
                if (!alert.isValid || alert.band == BAND_NONE) continue;
                if (alert.band == BAND_LASER) continue;
                
                uint16_t alertFreq = (uint16_t)alert.frequency;
                
                // Skip priority alert
                if (alert.band == priority.band && alertFreq == currentFreq) continue;
                if (ctx.isMuted) continue;
                if (!isBandEnabledForSecondary(alert.band, s)) continue;
                
                if (shouldAnnounceThreatEscalation(alert.band, alertFreq, (uint8_t)ctx.alertCount, ctx.now)) {
                    markThreatEscalationAnnounced(alert.band, alertFreq);
                    
                    if (!isValidAnnounceBand(alert.band)) continue;
                    
                    // Count direction breakdown
                    uint8_t aheadCount = 0, behindCount = 0, sideCount = 0;
                    for (int j = 0; j < ctx.alertCount; j++) {
                        const AlertData& a = ctx.alerts[j];
                        if (!a.isValid || a.band == BAND_NONE) continue;
                        
                        if (a.direction & DIR_FRONT) aheadCount++;
                        else if (a.direction & DIR_REAR) behindCount++;
                        else sideCount++;
                    }
                    
                    uint8_t total = aheadCount + behindCount + sideCount;
                    
                    action.type = VoiceAction::Type::ANNOUNCE_ESCALATION;
                    action.band = toAudioBand(alert.band);
                    action.freq = alertFreq;
                    action.dir = toAudioDirection(alert.direction);
                    action.bogeyCount = total;
                    action.aheadCount = aheadCount;
                    action.behindCount = behindCount;
                    action.sideCount = sideCount;
                    
                    updateLastAnnouncedTime(ctx.now);
                    VOICE_PERF_INC(voiceAnnounceEscalation);
                    
                    return action;
                }
            }
        }
    }
    
    return action;  // NONE
}

// ============================================================================
// State Management
// ============================================================================

void VoiceModule::clearAllState() {
    clearAnnouncedAlerts();
    clearAlertHistories();
    resetLastAnnounced();
    resetPriorityStability();
}

// ============================================================================
// Announced Alert Tracking
// ============================================================================

bool VoiceModule::isAlertAnnounced(Band band, uint16_t freq) {
    uint32_t id = makeAlertId(band, freq);
    for (int i = 0; i < announcedAlertCount; i++) {
        if (announcedAlertIds[i] == id) return true;
    }
    return false;
}

void VoiceModule::markAlertAnnounced(Band band, uint16_t freq) {
    uint32_t id = makeAlertId(band, freq);
    if (announcedAlertCount < MAX_ANNOUNCED_ALERTS && !isAlertAnnounced(band, freq)) {
        announcedAlertIds[announcedAlertCount++] = id;
    }
}

void VoiceModule::clearAnnouncedAlerts() {
    announcedAlertCount = 0;
    memset(announcedAlertIds, 0, sizeof(announcedAlertIds));
}

// ============================================================================
// Alert History Tracking - Smart Threat Escalation
// ============================================================================

VoiceModule::AlertHistory* VoiceModule::findAlertHistory(uint32_t alertId) {
    for (int i = 0; i < alertHistoryCount; i++) {
        if (alertHistories[i].alertId == alertId) {
            return &alertHistories[i];
        }
    }
    return nullptr;
}

VoiceModule::AlertHistory* VoiceModule::getOrCreateAlertHistory(uint32_t alertId, unsigned long now) {
    AlertHistory* h = findAlertHistory(alertId);
    if (h) return h;
    
    if (alertHistoryCount < MAX_ALERT_HISTORIES) {
        h = &alertHistories[alertHistoryCount++];
        h->alertId = alertId;
        h->currentBars = 0;
        h->lastUpdateMs = now;
        h->strongSinceMs = 0;
        h->wasWeak = false;
        h->escalationAnnounced = false;
        return h;
    }
    
    // Recycle oldest (by elapsed time, handles millis() wraparound)
    unsigned long oldestElapsed = 0;
    int oldestIdx = -1;
    for (int i = 0; i < alertHistoryCount; i++) {
        unsigned long elapsed = now - alertHistories[i].lastUpdateMs;
        if (elapsed > oldestElapsed) {
            oldestElapsed = elapsed;
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

void VoiceModule::updateAlertHistory(Band band, uint16_t freq, uint8_t bars, unsigned long now) {
    if (band == BAND_LASER) return;
    
    uint32_t alertId = makeAlertId(band, freq);
    AlertHistory* h = getOrCreateAlertHistory(alertId, now);
    if (!h) return;
    
    if (bars <= WEAK_THRESHOLD) h->wasWeak = true;
    
    if (bars >= STRONG_THRESHOLD) {
        if (h->strongSinceMs == 0) h->strongSinceMs = now;
    } else {
        h->strongSinceMs = 0;
    }
    
    h->currentBars = bars;
    h->lastUpdateMs = now;
}

void VoiceModule::cleanupStaleHistories(unsigned long now) {
    for (int i = alertHistoryCount - 1; i >= 0; i--) {
        if (now - alertHistories[i].lastUpdateMs > HISTORY_STALE_MS) {
            for (int j = i; j < alertHistoryCount - 1; j++) {
                alertHistories[j] = alertHistories[j + 1];
            }
            alertHistoryCount--;
        }
    }
}

bool VoiceModule::shouldAnnounceThreatEscalation(Band band, uint16_t freq, uint8_t totalBogeys, unsigned long now) {
    if (band == BAND_LASER) return false;
    
    uint32_t alertId = makeAlertId(band, freq);
    AlertHistory* h = findAlertHistory(alertId);
    if (!h) return false;
    
    bool wasWeak = h->wasWeak;
    bool nowStrong = (h->currentBars >= STRONG_THRESHOLD);
    bool sustained = (h->strongSinceMs > 0) && (now - h->strongSinceMs >= SUSTAINED_MS);
    bool notNoisy = (totalBogeys <= MAX_BOGEYS_FOR_ESCALATION);
    bool notAnnounced = !h->escalationAnnounced;
    
    if (wasWeak && nowStrong && sustained && notNoisy && notAnnounced) {
        return true;
    }
    return false;
}

void VoiceModule::markThreatEscalationAnnounced(Band band, uint16_t freq) {
    uint32_t alertId = makeAlertId(band, freq);
    AlertHistory* h = findAlertHistory(alertId);
    if (h) h->escalationAnnounced = true;
}

void VoiceModule::clearAlertHistories() {
    alertHistoryCount = 0;
    memset(alertHistories, 0, sizeof(alertHistories));
}

// ============================================================================
// Direction Change Throttling
// ============================================================================

void VoiceModule::resetDirectionThrottle(unsigned long now) {
    directionChangeCount = 0;
    directionChangeWindowStart = now;
}

bool VoiceModule::shouldThrottleDirectionChange(unsigned long now) {
    if (now - directionChangeWindowStart > DIRECTION_THROTTLE_WINDOW_MS) {
        directionChangeCount = 0;
        directionChangeWindowStart = now;
    }
    directionChangeCount++;
    return directionChangeCount > DIRECTION_CHANGE_LIMIT;
}

// ============================================================================
// Priority Stability Tracking
// ============================================================================

void VoiceModule::updatePriorityStability(uint32_t currentAlertId, unsigned long now) {
    if (currentAlertId != lastPriorityAlertId) {
        lastPriorityAlertId = currentAlertId;
        priorityStableSince = now;
    }
}

void VoiceModule::markPriorityAnnounced(unsigned long now) {
    lastPriorityAnnouncementTime = now;
}

void VoiceModule::resetPriorityStability() {
    priorityStableSince = 0;
    lastPriorityAlertId = 0xFFFFFFFF;
}

bool VoiceModule::canAnnounceSecondary(unsigned long now) const {
    return (priorityStableSince > 0) &&
           (now - priorityStableSince >= PRIORITY_STABILITY_MS) &&
           (now - lastPriorityAnnouncementTime >= POST_PRIORITY_GAP_MS);
}

// ============================================================================
// Last Announced Tracking
// ============================================================================

bool VoiceModule::hasAlertChanged(Band band, uint16_t freq) const {
    return (band != lastVoiceAlertBand) || (freq != lastVoiceAlertFrequency);
}

bool VoiceModule::hasDirectionChanged(Direction dir) const {
    return dir != lastVoiceAlertDirection;
}

bool VoiceModule::hasCooldownPassed(unsigned long now) const {
    return (now - lastVoiceAlertTime >= VOICE_ALERT_COOLDOWN_MS);
}

bool VoiceModule::hasBogeyCountCooldownPassed(unsigned long now) const {
    return (now - lastVoiceAlertTime >= BOGEY_COUNT_COOLDOWN_MS);
}

bool VoiceModule::hasBogeyCountChanged(uint8_t count) const {
    return count != lastVoiceAlertBogeyCount;
}

void VoiceModule::updateLastAnnounced(Band band, Direction dir, uint16_t freq, uint8_t bogeyCount, unsigned long now) {
    lastVoiceAlertBand = band;
    lastVoiceAlertDirection = dir;
    lastVoiceAlertFrequency = freq;
    lastVoiceAlertBogeyCount = bogeyCount;
    lastVoiceAlertTime = now;
}

void VoiceModule::updateLastAnnouncedDirection(Direction dir, uint8_t bogeyCount) {
    lastVoiceAlertDirection = dir;
    lastVoiceAlertBogeyCount = bogeyCount;
}

void VoiceModule::updateLastAnnouncedTime(unsigned long now) {
    lastVoiceAlertTime = now;
}

void VoiceModule::resetLastAnnounced() {
    lastVoiceAlertBand = BAND_NONE;
    lastVoiceAlertDirection = DIR_NONE;
    lastVoiceAlertFrequency = 0xFFFF;
    lastVoiceAlertBogeyCount = 0;
    lastVoiceAlertTime = 0;
}

// ============================================================================
// Speed Helpers - Low Speed Muting
// ============================================================================

bool VoiceModule::getCurrentSpeedSample(unsigned long now, float& speedMphOut) const {
    if (cachedSpeedTimestamp == 0) {
        return false;
    }
    if ((now - cachedSpeedTimestamp) >= SPEED_CACHE_MAX_AGE_MS) {
        return false;
    }
    speedMphOut = cachedSpeedMph;
    return true;
}

float VoiceModule::getCurrentSpeedMph(unsigned long now) {
    float speedMph = 0.0f;
    if (getCurrentSpeedSample(now, speedMph)) {
        return speedMph;
    }

    return 0.0f;
}

void VoiceModule::updateSpeedSample(float speedMph, unsigned long timestampMs) {
    // Ignore invalid/negative input so stale-good data is preserved.
    if (!(speedMph >= 0.0f)) {
        return;
    }
    cachedSpeedMph = speedMph;
    cachedSpeedTimestamp = timestampMs;
}

void VoiceModule::clearSpeedSample() {
    cachedSpeedMph = 0.0f;
    cachedSpeedTimestamp = 0;
}

bool VoiceModule::hasValidSpeedSource(unsigned long now) const {
    float speedMph = 0.0f;
    return getCurrentSpeedSample(now, speedMph);
}

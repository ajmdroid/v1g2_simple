// V1 Alert Module - Header
// Handles V1 radar alert detection, display, audio, and clearing
// Extracted from main.cpp to improve maintainability

#ifndef V1_ALERT_MODULE_H
#define V1_ALERT_MODULE_H

#include <Arduino.h>
#include "packet_parser.h"  // For AlertData type

// Forward declarations (avoid including heavy headers)
class V1BLEClient;
struct V1Settings;
class V1Display;
class SettingsManager;

/**
 * V1AlertModule - Encapsulates all V1 alert handling
 * 
 * Responsibilities:
 * - BLE data parsing (via existing PacketParser)
 * - Alert display rendering
 * - Alert audio playback
 * - Alert clearing/timeout logic
 * 
 * This is a stub for Phase 1 migration. Will grow incrementally.
 */
class V1AlertModule {
public:
    V1AlertModule();
    
    // Initialize with dependencies (call from setup())
    void begin(V1BLEClient* ble, PacketParser* parser, V1Display* display, SettingsManager* settings);
    
    // Main update - call from loop()
    void update();
    
    // Cleanup
    void end();
    
    // Static utility: Get signal bars for alert based on direction
    // Returns front strength if front, rear if rear, otherwise max of both
    static uint8_t getAlertBars(const AlertData& alert);
    
    // Static utility: Create unique alert ID from band and frequency
    // Used for tracking announced alerts, lockout state, etc.
    static uint32_t makeAlertId(Band band, uint16_t freq);
    
    // Static utility: Check if band is enabled for secondary alert announcements
    static bool isBandEnabledForSecondary(Band band, const V1Settings& settings);
    
    // Announced alert tracking - for voice announcement deduplication
    // Tracks which alerts have been announced this session
    bool isAlertAnnounced(Band band, uint16_t freq);
    void markAlertAnnounced(Band band, uint16_t freq);
    void clearAnnouncedAlerts();
    
    // Combined clear - resets all tracking when alerts clear
    // Clears: announced alerts, alert histories, last announced, priority stability
    void clearAllAlertState();
    
    // Alert history tracking - for smart threat escalation
    // Detects signals ramping up over time (weak -> strong)
    void updateAlertHistory(Band band, uint16_t freq, uint8_t bars, unsigned long now);
    void cleanupStaleHistories(unsigned long now);
    bool shouldAnnounceThreatEscalation(Band band, uint16_t freq, uint8_t totalBogeys, unsigned long now);
    void markThreatEscalationAnnounced(Band band, uint16_t freq);
    void clearAlertHistories();
    
    // Direction change throttling - prevents spamming when V1 arrow bounces
    // Call resetDirectionThrottle() when priority alert changes
    // Call shouldThrottleDirectionChange() before announcing direction-only changes
    void resetDirectionThrottle(unsigned long now);
    bool shouldThrottleDirectionChange(unsigned long now);  // Returns true if should skip announcement
    uint8_t getDirectionChangeCount() const { return directionChangeCount; }  // For debug logging
    
    // Priority stability tracking - controls when secondary alerts can be announced
    // Secondary alerts require priority to be stable (unchanging) and a gap after announcement
    void updatePriorityStability(uint32_t currentAlertId, unsigned long now);  // Call when priority alert checked
    void markPriorityAnnounced(unsigned long now);  // Call when priority alert announced
    void resetPriorityStability();  // Call when all alerts clear
    bool canAnnounceSecondary(unsigned long now) const;  // Returns true if secondary announcement allowed
    
    // Voice alert "last announced" tracking - tracks what was last announced to avoid repeats
    bool hasAlertChanged(Band band, uint16_t freq) const;  // Band or frequency changed?
    bool hasDirectionChanged(Direction dir) const;
    bool hasCooldownPassed(unsigned long now) const;       // 5s cooldown for new alerts
    bool hasBogeyCountCooldownPassed(unsigned long now) const;  // 2s cooldown for count changes
    bool hasBogeyCountChanged(uint8_t count) const;
    void updateLastAnnounced(Band band, Direction dir, uint16_t freq, uint8_t bogeyCount, unsigned long now);
    void updateLastAnnouncedDirection(Direction dir, uint8_t bogeyCount);  // Direction-only update
    void updateLastAnnouncedTime(unsigned long now);  // Time-only update
    void resetLastAnnounced();  // Call when alerts clear
    uint8_t getLastBogeyCount() const { return lastVoiceAlertBogeyCount; }  // For debug logging
    
    // Alert persistence - shows last alert briefly after V1 clears it (grey/faded display)
    void setPersistedAlert(const AlertData& alert);  // Save alert for persistence
    void startPersistence(unsigned long now);        // Start persistence timer when alert clears
    void clearPersistence();                         // Clear persistence state
    bool shouldShowPersisted(unsigned long now, unsigned long persistMs) const;  // Check if should display
    const AlertData& getPersistedAlert() const { return persistedAlert; }
    bool isPersistenceActive() const { return alertPersistenceActive; }

private:
    // Dependencies (set in begin())
    V1BLEClient* bleClient = nullptr;
    PacketParser* parser = nullptr;
    V1Display* display = nullptr;
    SettingsManager* settings = nullptr;
    
    bool initialized = false;
    
    // Announced alert tracking state
    // Use band<<16 | freq to create unique identifiers (handles Laser freq=0)
    static constexpr int MAX_ANNOUNCED_ALERTS = 10;
    uint32_t announcedAlertIds[MAX_ANNOUNCED_ALERTS] = {0};
    uint8_t announcedAlertCount = 0;
    
    // Smart threat escalation tracking - detect signals ramping up over time
    // Trigger: was weak + now strong + sustained + not too many bogeys
    static constexpr int WEAK_THRESHOLD = 2;           // "Was weak" = 2 bars or less
    static constexpr int STRONG_THRESHOLD = 4;         // "Now strong" = 4+ bars
    static constexpr unsigned long SUSTAINED_MS = 500; // Must stay strong for 500ms
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
    
    static constexpr int MAX_ALERT_HISTORIES = 10;
    AlertHistory alertHistories[MAX_ALERT_HISTORIES] = {};
    uint8_t alertHistoryCount = 0;
    
    // Private helpers for alert history
    AlertHistory* findAlertHistory(uint32_t alertId);
    AlertHistory* getOrCreateAlertHistory(uint32_t alertId, unsigned long now);
    
    // Direction change throttling state - prevents spam when V1 arrow bounces
    static constexpr unsigned long DIRECTION_THROTTLE_WINDOW_MS = 10000;  // 10 second window
    static constexpr uint8_t DIRECTION_CHANGE_LIMIT = 3;  // Max changes before throttling
    uint8_t directionChangeCount = 0;
    unsigned long directionChangeWindowStart = 0;
    
    // Priority stability tracking state - controls secondary alert timing
    static constexpr unsigned long PRIORITY_STABILITY_MS = 1000;   // Priority must be stable 1s
    static constexpr unsigned long POST_PRIORITY_GAP_MS = 1500;    // Wait 1.5s after priority announcement
    unsigned long lastPriorityAnnouncementTime = 0;
    unsigned long priorityStableSince = 0;
    uint32_t lastPriorityAlertId = 0xFFFFFFFF;
    
    // Voice alert "last announced" tracking state
    static constexpr unsigned long VOICE_ALERT_COOLDOWN_MS = 5000;  // Min 5s between new alert announcements
    static constexpr unsigned long BOGEY_COUNT_COOLDOWN_MS = 2000;  // Min 2s between bogey count-only updates
    Band lastVoiceAlertBand = BAND_NONE;
    Direction lastVoiceAlertDirection = DIR_NONE;
    uint16_t lastVoiceAlertFrequency = 0xFFFF;
    uint8_t lastVoiceAlertBogeyCount = 0;
    unsigned long lastVoiceAlertTime = 0;
    
    // Alert persistence state - shows last alert briefly after V1 clears it
    AlertData persistedAlert;
    unsigned long alertClearedTime = 0;
    bool alertPersistenceActive = false;
};

#endif // V1_ALERT_MODULE_H

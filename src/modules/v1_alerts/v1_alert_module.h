// V1 Alert Module - Header
// Handles V1 radar alert detection, display, audio, and clearing
// Extracted from main.cpp to improve maintainability

#ifndef V1_ALERT_MODULE_H
#define V1_ALERT_MODULE_H

#include <Arduino.h>
#include "packet_parser.h"  // For AlertData type

// Forward declarations (avoid including heavy headers)
class V1BLEClient;
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
    
    // Announced alert tracking - for voice announcement deduplication
    // Tracks which alerts have been announced this session
    bool isAlertAnnounced(Band band, uint16_t freq);
    void markAlertAnnounced(Band band, uint16_t freq);
    void clearAnnouncedAlerts();
    
    // Alert history tracking - for smart threat escalation
    // Detects signals ramping up over time (weak -> strong)
    void updateAlertHistory(Band band, uint16_t freq, uint8_t bars, unsigned long now);
    void cleanupStaleHistories(unsigned long now);
    bool shouldAnnounceThreatEscalation(Band band, uint16_t freq, uint8_t totalBogeys, unsigned long now);
    void markThreatEscalationAnnounced(Band band, uint16_t freq);
    void clearAlertHistories();

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
};

#endif // V1_ALERT_MODULE_H

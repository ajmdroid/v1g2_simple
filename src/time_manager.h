/**
 * Time Manager for V1 Gen2 Display
 * 
 * Provides reliable time tracking by:
 * - Using millis() offset to track elapsed time internally
 * - Syncing via NTP when WiFi connects
 * - Supporting manual time setting from web UI
 * - Providing time ONLY when it has been properly set
 */

#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <Arduino.h>
#include <FS.h>
#include <time.h>

class TimeManager {
public:
    TimeManager();
    
    // Initialize time manager
    void begin(fs::FS* filesystem = nullptr);
    
    // Set time manually or from NTP (Unix timestamp)
    void setTime(time_t timestamp);
    
    // Get current time as Unix timestamp
    // Returns 0 if time has never been set
    time_t now() const;
    
    // Legacy alias for now()
    time_t getTime() const { return now(); }
    
    // Check if time has been set (via NTP or manually)
    bool isTimeValid() const;
    
    // Attempt NTP sync - call when WiFi connects
    // Returns true if sync successful
    bool syncNTP();
    
    // Periodic update - call from main loop
    // Re-syncs NTP if due and WiFi connected
    void update();
    
    // Get formatted timestamp string
    String getTimestamp() const;
    String getTimestampISO() const;  // ISO 8601 format
    
    // Get time components
    bool getLocalTime(struct tm* timeinfo) const;
    
    // Debug info
    unsigned long getSecondsSinceSet() const;
    
private:
    fs::FS* fs;
    bool timeSet;              // Has time been properly set?
    bool initialized;          // Has begin() been called?
    
    // Internal clock tracking
    time_t baseUnixTime;       // Unix timestamp when time was set
    unsigned long baseMillis;  // millis() when time was set
    
    // NTP re-sync tracking
    unsigned long lastNTPSyncMs;
    static const unsigned long NTP_RESYNC_INTERVAL_MS = 3600000;  // 1 hour
    static const int NTP_SYNC_TIMEOUT_RETRIES = 50;       // More retries
    static const int NTP_SYNC_RETRY_DELAY_MS = 200;       // Longer delay between retries
};

// Global instance
extern TimeManager timeManager;

#endif // TIME_MANAGER_H

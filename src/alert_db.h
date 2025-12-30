/**
 * SQLite Alert Database for V1 Gen2 Display
 * 
 * Robust logging with:
 * - SQLite database on SD card
 * - Indexed queries for fast lookups
 * - Session tracking across power cycles
 * - Prepared for GPS and RTC timestamps
 */

#ifndef ALERT_DB_H
#define ALERT_DB_H

#include <Arduino.h>
#include <sqlite3.h>
#include "packet_parser.h"

// Database path on SD card (SD_MMC mounts at /sdcard)
#ifndef ALERT_DB_PATH
#define ALERT_DB_PATH "/sdcard/v1_alerts.db"
#endif

// Maximum recent alerts to return in queries
#ifndef ALERT_DB_MAX_RECENT
#define ALERT_DB_MAX_RECENT 100
#endif

class AlertDB {
public:
    AlertDB();
    ~AlertDB();
    
    // Initialize database (call after SD_MMC.begin())
    bool begin();
    
    // Close database cleanly
    void end();
    
    // Check if database is ready
    bool isReady() const { return db != nullptr; }
    
    // Get status text
    String statusText() const;
    
    // Log an alert event
    bool logAlert(const AlertData& alert, const DisplayState& state, size_t alertCount);
    
    // Log alert cleared
    bool logClear();
    
    // Set GPS data for subsequent logs (call when GPS updates)
    void setGPS(double lat, double lon, float speedMph, float heading);
    
    // Set RTC timestamp for subsequent logs (call when RTC/NTP syncs)
    void setTimestampUTC(uint32_t unixTime);
    
    // Query functions
    String getRecentJson(size_t maxRows = ALERT_DB_MAX_RECENT) const;
    String getStatsJson() const;  // Band counts, peak frequencies, etc.
    
    // Get total alert count
    uint32_t getTotalAlerts() const;
    
    // Clear all data (dangerous!)
    bool clearAll();
    
    // Get current session ID
    uint32_t getSessionId() const { return sessionId; }

private:
    sqlite3* db;
    uint32_t sessionId;
    
    // GPS data (updated externally)
    bool hasGPS;
    double gpsLat;
    double gpsLon;
    float gpsSpeed;
    float gpsHeading;
    
    // RTC timestamp (updated externally)
    bool hasRTC;
    uint32_t rtcTimestamp;
    
    // Deduplication
    struct LastAlert {
        bool active = false;
        Band band = BAND_NONE;
        Direction direction = DIR_NONE;
        uint32_t frequency = 0;
        uint8_t front = 0;
        uint8_t rear = 0;
        size_t count = 0;
        bool muted = false;
    };
    LastAlert lastAlert;
    
    // Internal helpers
    bool createSchema();
    bool initSession();
    bool execSQL(const char* sql);
    bool shouldLog(const AlertData& alert, const DisplayState& state, size_t count);
    String bandToString(Band band) const;
    String dirToString(Direction dir) const;
};

// Global instance
extern AlertDB alertDB;

#endif // ALERT_DB_H

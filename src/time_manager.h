/**
 * Time Manager for V1 Gen2 Display
 * Manages system time with NTP sync and SD card persistence
 */

#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <Arduino.h>
#include <FS.h>
#include <time.h>

class TimeManager {
public:
    TimeManager();
    
    // Initialize time manager with optional filesystem
    void begin(fs::FS* filesystem = nullptr);
    
    // Load last known time from SD card
    bool loadTimeFromSD();
    
    // Save current time to SD card
    bool saveTimeToSD();
    
    // Set time manually (Unix timestamp)
    void setTime(time_t timestamp);
    
    // Get current time as Unix timestamp
    time_t getTime() const;
    
    // Check if time has been set (not default 1970)
    bool isTimeValid() const;
    
    // Get formatted timestamp string
    String getTimestamp() const;
    String getTimestampISO() const;  // ISO 8601 format
    
    // Get time components
    bool getLocalTime(struct tm* timeinfo) const;
    
    // Periodic save (call in loop, saves every 10 minutes)
    void process();
    
private:
    fs::FS* fs;
    unsigned long lastSaveTime;
    bool timeLoaded;
    static const unsigned long SAVE_INTERVAL = 600000; // 10 minutes
    static const char* TIME_FILE;
};

// Global instance
extern TimeManager timeManager;

#endif // TIME_MANAGER_H

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
    
private:
    fs::FS* fs;
    bool timeLoaded;
    bool initialized;  // Track if begin() has been called
};

// Global instance
extern TimeManager timeManager;

#endif // TIME_MANAGER_H

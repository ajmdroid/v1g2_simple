/**
 * Time Manager implementation
 * Simplified version without SD card persistence - relies on NTP only
 */

#include "time_manager.h"
#include <sys/time.h>

// Global instance
TimeManager timeManager;

TimeManager::TimeManager() : fs(nullptr), timeLoaded(false), initialized(false) {
}

void TimeManager::begin(fs::FS* filesystem) {
    fs = filesystem;  // Keep reference but don't use for time storage
    initialized = true;
    Serial.println("[TimeManager] Initialized (NTP-only mode)");
}

void TimeManager::setTime(time_t timestamp) {
    if (!initialized) {
        return;
    }
    if (timestamp < 1609459200) {  // Validate timestamp (after 2021-01-01)
        Serial.printf("[TimeManager] Invalid timestamp rejected: %ld\n", timestamp);
        return;
    }
    
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    
    timeLoaded = true;
    Serial.printf("[TimeManager] Time set: %ld\n", timestamp);
}

time_t TimeManager::getTime() const {
    return time(nullptr);
}

bool TimeManager::isTimeValid() const {
    if (!initialized) {
        return false;
    }
    time_t now = getTime();
    // Valid if after 2021-01-01 00:00:00 UTC (1609459200)
    return now >= 1609459200;
}

String TimeManager::getTimestamp() const {
    if (!initialized || !isTimeValid()) {
        return "N/A";
    }
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return "N/A";
    }
    
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buffer);
}

String TimeManager::getTimestampISO() const {
    if (!initialized || !isTimeValid()) {
        return "N/A";
    }
    
    time_t now = getTime();
    struct tm timeinfo;
    if (gmtime_r(&now, &timeinfo) == NULL) {
        return "N/A";
    }
    
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buffer);
}

bool TimeManager::getLocalTime(struct tm* timeinfo) const {
    if (!initialized || !timeinfo) {
        return false;
    }
    
    time_t now = getTime();
    if (!isTimeValid()) {
        return false;
    }
    
    // Use UTC (gmtime) instead of local time
    return gmtime_r(&now, timeinfo) != NULL;
}

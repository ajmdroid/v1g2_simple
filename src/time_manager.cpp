/**
 * Time Manager implementation
 */

#include "time_manager.h"
#include <sys/time.h>

const char* TimeManager::TIME_FILE = "/last_time.txt";

// Global instance
TimeManager timeManager;

TimeManager::TimeManager() : fs(nullptr), lastSaveTime(0), timeLoaded(false) {
}

void TimeManager::begin(fs::FS* filesystem) {
    fs = filesystem;
    
    // Try to load saved time
    if (fs && !timeLoaded) {
        if (loadTimeFromSD()) {
            Serial.println("[TimeManager] Loaded time from SD card");
        } else {
            Serial.println("[TimeManager] No saved time found");
        }
    }
}

bool TimeManager::loadTimeFromSD() {
    if (!fs) {
        return false;
    }
    
    File file = fs->open(TIME_FILE, FILE_READ);
    if (!file) {
        return false;
    }
    
    String line = file.readStringUntil('\n');
    file.close();
    
    line.trim();
    if (line.length() == 0) {
        return false;
    }
    
    // Parse Unix timestamp
    time_t timestamp = (time_t)line.toInt();
    if (timestamp < 1609459200) {  // Before 2021-01-01
        Serial.println("[TimeManager] Saved time too old, ignoring");
        return false;
    }
    
    // Set system time
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    
    timeLoaded = true;
    Serial.printf("[TimeManager] Set time from SD: %ld (%s)\n", timestamp, getTimestamp().c_str());
    return true;
}

bool TimeManager::saveTimeToSD() {
    if (!fs) {
        return false;
    }
    
    time_t now = getTime();
    if (!isTimeValid()) {
        return false;
    }
    
    File file = fs->open(TIME_FILE, FILE_WRITE);
    if (!file) {
        Serial.println("[TimeManager] Failed to open time file for writing");
        return false;
    }
    
    file.printf("%ld\n", now);
    file.close();
    
    lastSaveTime = millis();
    return true;
}

void TimeManager::setTime(time_t timestamp) {
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    
    Serial.printf("[TimeManager] Time set manually: %ld (%s)\n", timestamp, getTimestamp().c_str());
    
    // Save to SD immediately
    if (fs) {
        saveTimeToSD();
    }
}

time_t TimeManager::getTime() const {
    return time(nullptr);
}

bool TimeManager::isTimeValid() const {
    time_t now = getTime();
    // Valid if after 2021-01-01 00:00:00 UTC (1609459200)
    return now >= 1609459200;
}

String TimeManager::getTimestamp() const {
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
    time_t now = getTime();
    if (!isTimeValid()) {
        return "N/A";
    }
    
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);  // Use gmtime_r for UTC
    
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buffer);
}

bool TimeManager::getLocalTime(struct tm* timeinfo) const {
    time_t now = getTime();
    if (!isTimeValid()) {
        return false;
    }
    
    // Use UTC (gmtime) instead of local time
    gmtime_r(&now, timeinfo);
    return true;
}

void TimeManager::process() {
    // Save time periodically if valid and enough time has passed
    if (fs && isTimeValid() && (millis() - lastSaveTime > SAVE_INTERVAL || lastSaveTime == 0)) {
        saveTimeToSD();
    }
}

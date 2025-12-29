/**
 * Time Manager implementation
 */

#include "time_manager.h"
#include <sys/time.h>

const char* TimeManager::TIME_FILE = "/last_time.txt";

// Global instance
TimeManager timeManager;

TimeManager::TimeManager() : fs(nullptr), lastSaveTime(0), timeLoaded(false), initialized(false) {
}

void TimeManager::begin(fs::FS* filesystem) {
    fs = filesystem;
    initialized = true;  // Mark as initialized
    
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
        Serial.println("[TimeManager] No filesystem available");
        return false;
    }
    
    time_t now = getTime();
    if (!isTimeValid()) {
        Serial.println("[TimeManager] Time not valid for saving");
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
    Serial.printf("[TimeManager] Saved time to SD: %ld\n", now);
    return true;
}

void TimeManager::setTime(time_t timestamp) {
    if (!initialized) {
        return;  // Not initialized yet
    }
    if (timestamp < 1609459200) {  // Validate timestamp
        Serial.printf("[TimeManager] Invalid timestamp rejected: %ld\n", timestamp);
        return;
    }
    
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    
    Serial.printf("[TimeManager] Time set manually: %ld\n", timestamp);
    
    // Save to SD immediately (if available)
    if (fs) {
        saveTimeToSD();
    }
}

time_t TimeManager::getTime() const {
    return time(nullptr);
}

bool TimeManager::isTimeValid() const {
    if (!initialized) {
        return false;  // Not initialized yet
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

void TimeManager::process() {
    if (!initialized) {
        return;  // Not initialized yet
    }
    
    // Save time periodically if valid and enough time has passed
    if (fs && isTimeValid() && (millis() - lastSaveTime > SAVE_INTERVAL || lastSaveTime == 0)) {
        saveTimeToSD();
    }
}

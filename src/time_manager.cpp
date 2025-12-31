/**
 * Time Manager implementation
 * 
 * Uses internal millis()-based clock for reliable time tracking.
 * The ESP32 system time (time(nullptr)) is unreliable until NTP syncs,
 * so we track time ourselves using millis() offset from a known point.
 */

#include "time_manager.h"
#include <sys/time.h>
#include <WiFi.h>

// Global instance
TimeManager timeManager;

TimeManager::TimeManager() 
    : fs(nullptr)
    , timeSet(false)
    , initialized(false)
    , baseUnixTime(0)
    , baseMillis(0)
    , lastNTPSyncMs(0)
{
}

void TimeManager::begin(fs::FS* filesystem) {
    fs = filesystem;
    initialized = true;
    baseMillis = millis();
    Serial.println("[TimeManager] Initialized - waiting for NTP or manual time set");
}

void TimeManager::setTime(time_t timestamp) {
    if (!initialized) {
        return;
    }
    
    // Validate timestamp (must be after 2021-01-01)
    if (timestamp < 1609459200) {
        Serial.printf("[TimeManager] Invalid timestamp rejected: %ld\n", (long)timestamp);
        return;
    }
    
    // Store the base point for our internal clock
    baseUnixTime = timestamp;
    baseMillis = millis();
    timeSet = true;
    
    // Also update the system time for compatibility
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    
    Serial.printf("[TimeManager] Time set: %ld (base millis: %lu)\n", 
                  (long)timestamp, baseMillis);
}

time_t TimeManager::now() const {
    if (!initialized || !timeSet) {
        return 0;  // Return 0 if time not set (consumers should check isTimeValid())
    }
    
    // Calculate current time from our internal clock
    unsigned long elapsedMs = millis() - baseMillis;
    time_t currentTime = baseUnixTime + (elapsedMs / 1000);
    
    return currentTime;
}

bool TimeManager::isTimeValid() const {
    if (!initialized || !timeSet) {
        return false;
    }
    
    time_t current = now();
    // Valid if after 2021-01-01 00:00:00 UTC
    return current >= 1609459200;
}

bool TimeManager::syncNTP() {
    if (!initialized) {
        return false;
    }
    
    Serial.println("[TimeManager] Starting NTP sync...");
    
    // Check WiFi status
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[TimeManager] NTP sync skipped - WiFi not connected");
        return false;
    }
    
    // Configure NTP servers
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    
    // Wait for time to sync (longer timeout for network latency)
    struct tm timeinfo;
    int retries = 0;
    Serial.printf("[TimeManager] Waiting for NTP (max %d retries)...\n", NTP_SYNC_TIMEOUT_RETRIES);
    while (!::getLocalTime(&timeinfo, 100) && retries < NTP_SYNC_TIMEOUT_RETRIES) {
        delay(NTP_SYNC_RETRY_DELAY_MS);
        retries++;
        if (retries % 10 == 0) {
            Serial.printf("[TimeManager] NTP retry %d/%d...\n", retries, NTP_SYNC_TIMEOUT_RETRIES);
        }
    }
    
    if (retries < NTP_SYNC_TIMEOUT_RETRIES) {
        // NTP sync successful - get the time and set our internal clock
        time_t ntpTime = time(nullptr);
        
        Serial.println("[TimeManager] NTP sync successful!");
        Serial.printf("[TimeManager] Time (UTC): %04d-%02d-%02dT%02d:%02d:%02dZ\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        
        // Update our internal clock
        setTime(ntpTime);
        lastNTPSyncMs = millis();
        
        return true;
    } else {
        Serial.println("[TimeManager] NTP sync failed");
        return false;
    }
}

void TimeManager::update() {
    if (!initialized) {
        return;
    }
    
    // Check if it's time to re-sync NTP
    if (WiFi.status() == WL_CONNECTED) {
        unsigned long now = millis();
        
        // If we've never synced, or it's been over an hour, try NTP
        if (!timeSet || (now - lastNTPSyncMs > NTP_RESYNC_INTERVAL_MS)) {
            // Don't spam NTP - only try if enough time has passed
            static unsigned long lastAttempt = 0;
            if (now - lastAttempt > 60000) {  // At most once per minute
                lastAttempt = now;
                syncNTP();
            }
        }
    }
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
    
    time_t current = now();
    struct tm timeinfo;
    if (gmtime_r(&current, &timeinfo) == NULL) {
        return "N/A";
    }
    
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buffer);
}

bool TimeManager::getLocalTime(struct tm* timeinfo) const {
    if (!initialized || !timeinfo || !isTimeValid()) {
        return false;
    }
    
    time_t current = now();
    // Use UTC (gmtime) instead of local time
    return gmtime_r(&current, timeinfo) != NULL;
}

unsigned long TimeManager::getSecondsSinceSet() const {
    if (!initialized || !timeSet) {
        return 0;
    }
    return (millis() - baseMillis) / 1000;
}

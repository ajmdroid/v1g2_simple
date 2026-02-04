/**
 * Debug Logger - optional SD log sink.
 * Writes timestamped JSON lines when enabled in settings.
 * Uses buffered writes (4KB buffer, 1s flush) to minimize SD latency impact.
 * 
 * Log format: NDJSON (newline-delimited JSON) for ELK/Elasticsearch import
 */

#ifndef DEBUG_LOGGER_H
#define DEBUG_LOGGER_H

#include <Arduino.h>
#include <FS.h>
#include <time.h>
#include <freertos/FreeRTOS.h>

// Log file location and size cap (shared with UI/API)
inline constexpr const char* DEBUG_LOG_PATH = "/debug.log";
inline constexpr size_t DEBUG_LOG_MAX_BYTES = 1024 * 1024 * 1024;  // 1GB cap (SD card)

// Buffer settings for efficient SD writes
inline constexpr size_t DEBUG_LOG_BUFFER_SIZE = 4096;       // 4KB write buffer
inline constexpr size_t DEBUG_LOG_FLUSH_THRESHOLD = 3072;   // Flush when 75% full
inline constexpr unsigned long DEBUG_LOG_FLUSH_INTERVAL_MS = 2000;  // Flush every 2 seconds (reduces SD/display collision)

// WiFi transition deferral - avoid SD writes during WiFi reconnection (NVS flash contention)
inline constexpr unsigned long WIFI_TRANSITION_DEFER_MAX_MS = 5000;  // Max deferral before forced flush

// Log categories for selective filtering
enum class DebugLogCategory {
    System,
    Wifi,
    Alerts,
    Ble,
    Gps,
    Obd,
    Display,
    PerfMetrics,
    Audio,
    Camera,
    Lockout,
    Touch
};

struct DebugLogFilter {
    bool alerts = true;
    bool wifi = false;
    bool ble = false;
    bool gps = false;
    bool obd = false;
    bool system = true;
    bool display = false;
    bool perfMetrics = false;
    bool audio = false;
    bool camera = false;
    bool lockout = false;
    bool touch = false;
};

class DebugLogger {
public:
    void begin();

    // Enable/disable logging; safe to call repeatedly.
    void setEnabled(bool enabledFlag);
    void setFilter(const DebugLogFilter& filter);
    bool isEnabled() const { return enabled; }
    bool isEnabledFor(DebugLogCategory category) const;

    // Time source tracking
    enum class TimeSource {
        NONE,      // No time sync yet (1970 epoch)
        GPS,       // Synced from GPS
        NTP,       // Synced from NTP
        ESTIMATED  // Derived from previous sync + millis()
    };
    TimeSource getTimeSource() const { return timeSource; }

    // Time synchronization from GPS/NTP
    void syncTimeFromGPS(int year, int month, int day, int hour, int minute, int second);
    void syncTimeFromNTP(int year, int month, int day, int hour, int minute, int second);
    bool hasValidTime() const { return timeValid; }
    time_t getUnixTime() const;
    void getISO8601Timestamp(char* buf, size_t bufSize) const;

    // Append formatted line (auto timestamp + newline).
    void logf(DebugLogCategory category, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
    void logf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void log(DebugLogCategory category, const char* message);
    void log(const char* message);
    
    // Structured logging for JSON format (key-value pairs)
    void logEvent(DebugLogCategory category, const char* event, 
                  const char* jsonFields = nullptr);  // Additional JSON fields
    
    // Structured perf metrics (avoids message truncation)
    void logPerfMetrics(const char* fields);  // Pre-formatted key=value pairs
    
    // Buffer management - call periodically from main loop
    void update();  // Check if time-based flush needed
    void flush();   // Force flush buffer to SD (call on shutdown/crash)
    
    // WiFi transition deferral - defers SD writes during WiFi reconnection
    // to avoid NVS/flash contention that can cause multi-second stalls
    void notifyWifiTransition(bool stable);  // Called by WiFi manager on state changes
    bool isFlushDeferred() const { return wifiTransitionActive && !deferralExpired(); }
    
    // Render state guard - defers SD writes during display render to avoid collision
    void notifyRenderState(bool rendering);  // Call with true before render, false after
    bool isRenderActive() const { return renderActive; }

    // File helpers
    bool exists() const;
    size_t size() const;  // Returns 0 if file missing
    bool clear();
    bool storageReady() const;
    bool onSdCard() const;
    bool canEnable() const;  // Returns true if SD card present (required for logging)
    String tail(size_t maxBytes = 32768) const;  // Read last N bytes (default 32KB)

private:
    void bufferLine(const char* line);
    void flushBuffer();
    void rotateIfNeeded();
    bool categoryAllowed(DebugLogCategory category) const;
    const char* categoryName(DebugLogCategory category) const;
    void formatJsonLine(char* dest, size_t destSize, DebugLogCategory category, 
                        const char* message, const char* extraFields = nullptr);

    bool enabled = false;
    DebugLogFilter filter;
    
    // Time tracking (synced from GPS/NTP)
    bool timeValid = false;
    TimeSource timeSource = TimeSource::NONE;
    time_t timeSyncEpoch = 0;       // Unix timestamp when time was synced
    unsigned long timeSyncMillis = 0;  // millis() when time was synced
    
    // Ring buffer for batched writes
    char buffer[DEBUG_LOG_BUFFER_SIZE];
    size_t bufferPos = 0;
    unsigned long lastFlushMs = 0;
    
    // WiFi transition deferral state
    bool wifiTransitionActive = false;      // True during WiFi reconnect/disconnect
    unsigned long wifiTransitionStartMs = 0; // When deferral started
    bool deferralExpired() const;
    
    // Render state guard - prevents SD flush during display render
    volatile bool renderActive = false;     // True during display.update()
};

extern DebugLogger debugLogger;

#endif  // DEBUG_LOGGER_H

/**
 * Debug Logger - optional SD/LittleFS log sink.
 * Writes timestamped lines when enabled in settings.
 * Uses buffered writes to minimize SD latency impact on real-time tasks.
 */

#ifndef DEBUG_LOGGER_H
#define DEBUG_LOGGER_H

#include <Arduino.h>
#include <FS.h>

// Log file location and size cap (shared with UI/API)
inline constexpr const char* DEBUG_LOG_PATH = "/debug.log";
inline constexpr size_t DEBUG_LOG_MAX_BYTES = 1024 * 1024 * 1024;  // 1GB cap (SD card)

// Buffer settings for efficient SD writes
inline constexpr size_t DEBUG_LOG_BUFFER_SIZE = 4096;       // 4KB ring buffer
inline constexpr size_t DEBUG_LOG_FLUSH_THRESHOLD = 3072;   // Flush when 75% full
inline constexpr unsigned long DEBUG_LOG_FLUSH_INTERVAL_MS = 1000;  // Flush every 1 second

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
    bool wifi = true;
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

    // Append formatted line (auto timestamp + newline).
    void logf(DebugLogCategory category, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
    void logf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void log(DebugLogCategory category, const char* message);
    void log(const char* message);
    
    // Buffer management - call periodically from main loop
    void update();  // Check if time-based flush needed
    void flush();   // Force flush buffer to SD (call on shutdown/crash)

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

    bool enabled = false;
    DebugLogFilter filter;
    
    // Ring buffer for batched writes
    char buffer[DEBUG_LOG_BUFFER_SIZE];
    size_t bufferPos = 0;
    unsigned long lastFlushMs = 0;
};

extern DebugLogger debugLogger;

#endif  // DEBUG_LOGGER_H

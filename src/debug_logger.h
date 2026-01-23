/**
 * Debug Logger - optional SD/LittleFS log sink.
 * Writes timestamped lines when enabled in settings.
 */

#ifndef DEBUG_LOGGER_H
#define DEBUG_LOGGER_H

#include <Arduino.h>
#include <FS.h>

// Log file location and size cap (shared with UI/API)
inline constexpr const char* DEBUG_LOG_PATH = "/debug.log";
inline constexpr size_t DEBUG_LOG_MAX_BYTES = 1024 * 1024 * 1024;  // 1GB cap (SD card)

// Log categories for selective filtering
enum class DebugLogCategory {
    System,
    Wifi,
    Alerts,
    Ble,
    Gps,
    Obd,
    Display
};

struct DebugLogFilter {
    bool alerts = true;
    bool wifi = true;
    bool ble = false;
    bool gps = false;
    bool obd = false;
    bool system = true;
    bool display = false;
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

    // File helpers
    bool exists() const;
    size_t size() const;  // Returns 0 if file missing
    bool clear();
    bool storageReady() const;
    bool onSdCard() const;
    bool canEnable() const;  // Returns true if SD card present (required for logging)

private:
    void writeLine(const char* line);
    void rotateIfNeeded();
    bool categoryAllowed(DebugLogCategory category) const;

    bool enabled = false;
    DebugLogFilter filter;
};

extern DebugLogger debugLogger;

#endif  // DEBUG_LOGGER_H

/**
 * Debug Logger stub API.
 *
 * The SD-backed debug logger has been removed. This header keeps a
 * compatibility surface so existing call sites compile while performing
 * no logging work.
 */

#ifndef DEBUG_LOGGER_H
#define DEBUG_LOGGER_H

#include <Arduino.h>
#include <time.h>

// Log categories remain for API compatibility.
enum class DebugLogCategory {
    System,
    Wifi,
    Alerts,
    Ble,
    Display,
    PerfMetrics,
    Audio,
    Touch
};

struct DebugLogFilter {
    bool alerts = true;
    bool wifi = false;
    bool ble = false;
    bool system = true;
    bool display = false;
    bool perfMetrics = false;
    bool audio = false;
    bool touch = false;
};

// Legacy constants retained for web/API compatibility.
inline constexpr const char* DEBUG_LOG_PATH = "/debug.log";
inline constexpr size_t DEBUG_LOG_MAX_BYTES = 1024 * 1024 * 1024;

class DebugLogger {
public:
    enum class TimeSource {
        NONE,
        EXTERNAL_SOURCE,
        NTP,
        RTC,
        ESTIMATED
    };

    static constexpr uint32_t RTC_CACHE_MAX_AGE_HOURS = 24;

    void begin() {}
    void setEnabled(bool) {}
    void setFilter(const DebugLogFilter&) {}
    bool isEnabled() const { return false; }
    bool isEnabledFor(DebugLogCategory) const { return false; }

    TimeSource getTimeSource() const { return TimeSource::NONE; }

    void syncTimeFromExternal(int, int, int, int, int, int) {}
    void syncTimeFromNTP(int, int, int, int, int, int) {}
    bool hasValidTime() const { return false; }
    time_t getUnixTime() const { return 0; }
    void getISO8601Timestamp(char* buf, size_t bufSize) const {
        if (bufSize > 0) {
            buf[0] = '\0';
        }
    }

    bool restoreTimeFromCache() { return false; }
    void saveTimeToCache() {}
    void updateTimeCache() {}
    time_t getLastSyncEpoch() const { return 0; }

    void logf(DebugLogCategory, const char*, ...) __attribute__((format(printf, 3, 4))) {}
    void logf(const char*, ...) __attribute__((format(printf, 2, 3))) {}
    void log(DebugLogCategory, const char*) {}
    void log(const char*) {}
    void logEvent(DebugLogCategory, const char*, const char* = nullptr) {}
    void logPerfMetrics(const char*) {}

    void update() {}
    void flush() {}

    void enableAsyncMode() {}
    void disableAsyncMode() {}
    bool isAsyncMode() const { return false; }
    uint32_t getDropCount() const { return 0; }
    uint32_t getRateLimitDrops() const { return 0; }
    uint32_t getBufferFullDrops() const { return 0; }
    uint32_t getCoreViolationDrops() const { return 0; }
    uint32_t getQueueHighWater() const { return 0; }

    void notifyWifiTransition(bool) {}
    bool isFlushDeferred() const { return false; }
    void notifyRenderState(bool) {}
    bool isRenderActive() const { return false; }

    bool exists() const { return false; }
    size_t size() const { return 0; }
    bool clear() { return false; }
    bool storageReady() const { return false; }
    bool onSdCard() const { return false; }
    bool canEnable() const { return false; }
    String tail(size_t = 32768) const { return String(); }
};

inline DebugLogger debugLogger;

#if defined(DISABLE_DEBUG_LOGGER)
#define DBG_LOG_ENABLED(category) (false)
#define DBG_LOGF(category, ...) do { (void)sizeof(category); } while (0)
#define DBG_LOGLN(category, message) do { (void)sizeof(category); } while (0)
#else
#define DBG_LOG_ENABLED(category) (debugLogger.isEnabledFor(category))
#define DBG_LOGF(category, ...) do { \
    if (debugLogger.isEnabledFor(category)) debugLogger.logf(category, __VA_ARGS__); \
} while (0)
#define DBG_LOGLN(category, message) do { \
    if (debugLogger.isEnabledFor(category)) debugLogger.log(category, message); \
} while (0)
#endif

#endif  // DEBUG_LOGGER_H

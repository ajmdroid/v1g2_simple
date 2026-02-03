/**
 * Debug Logger - optional SD/LittleFS log sink.
 * Writes timestamped lines when enabled in settings.
 * Uses buffered writes to minimize SD latency impact on real-time tasks.
 * 
 * Supports two log formats:
 * - TEXT: Human-readable with millis/ISO timestamps
 * - JSON: NDJSON format for ELK/Elasticsearch import
 */

#ifndef DEBUG_LOGGER_H
#define DEBUG_LOGGER_H

#include <Arduino.h>
#include <FS.h>
#include <time.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Log file location and size cap (shared with UI/API)
inline constexpr const char* DEBUG_LOG_PATH = "/debug.log";
inline constexpr size_t DEBUG_LOG_MAX_BYTES = 1024 * 1024 * 1024;  // 1GB cap (SD card)

// Log format options
enum class DebugLogFormat {
    TEXT,   // Human-readable: [timestamp] message
    JSON    // NDJSON for ELK: {"@timestamp":"...","level":"info","category":"alerts","message":"..."}
};

// Buffer settings for efficient SD writes
inline constexpr size_t DEBUG_LOG_BUFFER_SIZE = 4096;       // 4KB write buffer
inline constexpr size_t DEBUG_LOG_FLUSH_THRESHOLD = 3072;   // Flush when 75% full
inline constexpr unsigned long DEBUG_LOG_FLUSH_INTERVAL_MS = 1000;  // Flush every 1 second

// WiFi transition deferral - avoid SD writes during WiFi reconnection (NVS flash contention)
inline constexpr unsigned long WIFI_TRANSITION_DEFER_MAX_MS = 5000;  // Max deferral before forced flush

// Breadcrumb ring buffer for incident capture (verbose events kept in RAM, dumped on incident)
inline constexpr size_t BREADCRUMB_RING_SIZE = 32768;  // 32KB ring buffer (configurable 32-128KB)
inline constexpr size_t BREADCRUMB_MAX_LINE = 256;     // Max chars per breadcrumb entry
inline constexpr const char* INCIDENT_LOG_PATH = "/incident.log";

// Async write queue settings (Phase 2: background SD writes)
inline constexpr size_t LOG_QUEUE_DEPTH = 32;          // Queue depth (32 items)
inline constexpr size_t LOG_QUEUE_LINE_SIZE = 512;     // Max formatted line size
inline constexpr size_t LOG_WRITER_STACK_SIZE = 4096;  // Writer task stack (4KB)

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

// Queue item for async write mode
struct LogQueueItem {
    char line[LOG_QUEUE_LINE_SIZE];  // Pre-formatted log line
    uint16_t length;                  // Actual length (avoid strlen in writer)
    uint8_t flags;                    // Special flags (flush, incident, etc.)
    
    static constexpr uint8_t FLAG_NONE = 0;
    static constexpr uint8_t FLAG_FLUSH_NOW = 1;       // Force immediate flush
    static constexpr uint8_t FLAG_INCIDENT = 2;        // Incident capture request
};

class DebugLogger {
public:
    void begin();

    // Enable/disable logging; safe to call repeatedly.
    void setEnabled(bool enabledFlag);
    void setFilter(const DebugLogFilter& filter);
    void setFormat(DebugLogFormat fmt) { logFormat = fmt; }
    bool isEnabled() const { return enabled; }
    bool isEnabledFor(DebugLogCategory category) const;
    DebugLogFormat getFormat() const { return logFormat; }

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
    
    // Breadcrumb ring buffer - records verbose events in RAM for incident capture
    void breadcrumb(const char* msg);  // Add to ring (overwrites oldest)
    void breadcrumbf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    
    // Incident capture - dumps breadcrumb ring to SD with context header
    void captureIncident(const char* reason, uint32_t loopMaxUs, uint32_t qDropDelta);

    // Async write mode - moves SD I/O to background task (Phase 2)
    void setAsyncMode(bool async);  // Enable/disable async writes
    bool isAsyncMode() const { return asyncMode; }
    uint32_t getAsyncDropCount() const { return asyncDropCount; }  // Logs dropped due to full queue

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
    void formatTextLine(char* dest, size_t destSize, DebugLogCategory category, const char* message);
    void formatJsonLine(char* dest, size_t destSize, DebugLogCategory category, 
                        const char* message, const char* extraFields = nullptr);

    bool enabled = false;
    DebugLogFilter filter;
    DebugLogFormat logFormat = DebugLogFormat::TEXT;
    
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
    
    // Breadcrumb ring buffer (static allocation - no heap)
    char breadcrumbRing[BREADCRUMB_RING_SIZE];
    size_t breadcrumbHead = 0;        // Write position
    size_t breadcrumbCount = 0;       // Bytes used (up to BREADCRUMB_RING_SIZE)
    bool breadcrumbWrapped = false;   // True after first wrap
    portMUX_TYPE breadcrumbMux = portMUX_INITIALIZER_UNLOCKED;  // Spinlock for ISR-safe atomicity
    
    // Async write mode - background task for SD I/O (Phase 2)
    bool asyncMode = false;
    QueueHandle_t writeQueue = nullptr;
    TaskHandle_t writerTaskHandle = nullptr;
    std::atomic<uint32_t> asyncDropCount{0};  // Logs dropped due to full queue
    
    // Writer task entry point (static for xTaskCreate)
    static void writerTaskEntry(void* param);
    void writerTaskLoop();  // Instance method called by entry point
};

extern DebugLogger debugLogger;

#endif  // DEBUG_LOGGER_H

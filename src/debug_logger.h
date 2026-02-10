/**
 * Debug Logger - optional SD log sink (Channel B: diagnostic logs)
 * 
 * TWO-CHANNEL LOGGING ARCHITECTURE:
 * - Channel A (perf_metrics.h): Always-on numeric counters. RED ZONE SAFE.
 *   Use PERF_INC() / PERF_MAX() macros only. No strings, no heap, no locks.
 * 
 * - Channel B (this file): Human-readable diagnostic logs. SAFE ZONE ONLY.
 *   Never call from BLE callbacks, display render, or frequent loops.
 *   Rate-limited (100 lines/sec), can be dropped, disabled by default.
 * 
 * RED ZONE RULES:
 * - In BLE notify callbacks, display flush, packet parsing: ONLY perf counters
 * - debugLogger.log*() calls are NOT safe from red zones (even if no I/O)
 * - Violations cause jitter, stalls, or WDT resets
 * 
 * SAFE ZONE for debugLogger:
 * - Main loop (after display update)
 * - WiFi handlers (non-time-critical)
 * - Settings changes
 * - Connection state transitions (once per event, not per packet)
 * 
 * Implementation:
 * - Writes timestamped JSON lines when enabled in settings
 * - Uses 4KB ring buffer, never does sync I/O from log() call
 * - Flushes only from update() which runs in safe zone
 * - Rate-limited to prevent log storms
 * - Async mode available: FreeRTOS task on Core 0 for background writes
 * 
 * Log format: NDJSON (newline-delimited JSON) for ELK/Elasticsearch import
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

// Buffer settings for efficient SD writes
// Max formatted line: 512 bytes (formatJsonLine uses char[512])
// Lines exceeding this are truncated before buffering
inline constexpr size_t DEBUG_LOG_MAX_LINE_SIZE = 512;      // Max single log line (enforced by format buffers)
inline constexpr size_t DEBUG_LOG_BUFFER_SIZE = 4096;       // 4KB write buffer
inline constexpr size_t DEBUG_LOG_FLUSH_THRESHOLD = 3072;   // Flush when 75% full
inline constexpr unsigned long DEBUG_LOG_FLUSH_INTERVAL_MS = 2000;  // Flush every 2 seconds (reduces SD/display collision)

// Rate limiting to prevent log storms (safe zone enforcement)
inline constexpr size_t DEBUG_LOG_RATE_LIMIT = 100;         // Max lines per second
inline constexpr unsigned long DEBUG_LOG_RATE_WINDOW_MS = 1000;  // Rate limit window

// Async write task settings (Core 0, low priority)
// Message size kept small - each queued message is ~512 bytes, not 4KB
inline constexpr size_t DEBUG_LOG_MESSAGE_SIZE = 512;       // Max bytes per queued message
inline constexpr size_t DEBUG_LOG_QUEUE_DEPTH = 16;         // Queue up to 16 messages (~8KB total)
inline constexpr size_t DEBUG_LOG_WRITER_STACK_SIZE = 3072; // Stack for writer task

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
        RTC,       // Restored from NVS cache (persisted across boots)
        ESTIMATED  // Derived from previous sync + millis()
    };
    TimeSource getTimeSource() const { return timeSource; }

    // Time synchronization from GPS/NTP
    void syncTimeFromGPS(int year, int month, int day, int hour, int minute, int second);
    void syncTimeFromNTP(int year, int month, int day, int hour, int minute, int second);
    bool hasValidTime() const { return timeValid; }
    time_t getUnixTime() const;
    void getISO8601Timestamp(char* buf, size_t bufSize) const;
    
    // RTC time cache persistence (survives power cycles via NVS)
    bool restoreTimeFromCache();  // Call early in setup(); returns true if valid time restored
    void saveTimeToCache();       // Called automatically on GPS/NTP sync + periodically
    void updateTimeCache();       // Call from main loop - saves every 5 min if time valid
    time_t getLastSyncEpoch() const { return timeSyncEpoch; }
    static constexpr uint32_t RTC_CACHE_MAX_AGE_HOURS = 24;  // Skip NTP if cache newer than this
    static constexpr uint32_t RTC_CACHE_SAVE_INTERVAL_MS = 5 * 60 * 1000;  // Save every 5 minutes

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
    
    // Async mode control (FreeRTOS task for non-blocking SD writes)
    // NOTE: Not auto-enabled - must be explicitly called after setEnabled()
    void enableAsyncMode();   // Start background writer task on Core 0
    void disableAsyncMode();  // Stop task, switch to sync writes
    bool isAsyncMode() const { return asyncMode; }
    uint32_t getDropCount() const { return logDropCount.load(std::memory_order_relaxed); }  // Messages dropped when queue full
    uint32_t getRateLimitDrops() const { return logRateLimitDrops.load(std::memory_order_relaxed); }  // Messages dropped due to rate limit
    uint32_t getBufferFullDrops() const { return logBufferFullDrops.load(std::memory_order_relaxed); }  // Messages dropped due to buffer full
    uint32_t getCoreViolationDrops() const { return logCoreViolationDrops.load(std::memory_order_relaxed); }  // Messages dropped: wrong core
    uint32_t getQueueHighWater() const { return logQueueHW.load(std::memory_order_relaxed); }  // Max queue depth observed
    
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
    void rotateIfNeededUnlocked(fs::FS* fs);  // Internal - caller must hold SD mutex
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
    unsigned long lastTimeCacheSaveMs = 0;  // Last time we saved to NVS
    
    // Ring buffer for batched writes
    char buffer[DEBUG_LOG_BUFFER_SIZE];
    size_t bufferPos = 0;
    unsigned long lastFlushMs = 0;
    
    // Async mode state (FreeRTOS task + queue for non-blocking writes)
    bool asyncMode = false;
    QueueHandle_t writeQueue = nullptr;
    TaskHandle_t writerTaskHandle = nullptr;
    
    // Queue message structure - small fixed-size buffer
    struct WriteMessage {
        char data[DEBUG_LOG_MESSAGE_SIZE];
        size_t length;
    };
    
    // Metrics - atomic for cross-core safety (Core 1 writes, Core 0 reader task)
    std::atomic<uint32_t> logDropCount{0};    // Messages dropped (queue full or heap exhausted)
    std::atomic<uint32_t> logRateLimitDrops{0}; // Messages dropped due to rate limiting
    std::atomic<uint32_t> logBufferFullDrops{0}; // Messages dropped due to buffer full
    std::atomic<uint32_t> logCoreViolationDrops{0}; // Messages dropped: called from wrong core
    std::atomic<uint32_t> logQueueHW{0};      // Queue high-water mark (max depth observed)
    
    // Rate limiting state
    uint32_t rateWindowLineCount = 0;         // Lines logged in current window
    unsigned long rateWindowStartMs = 0;      // Start of current rate window
    
    void flushBufferSync();                          // Synchronous write (direct or from task)
    void flushBufferAsync();                         // Queue buffer for async write
    static void writerTaskEntry(void* param);        // FreeRTOS task entry point
    void writerTaskLoop();                           // Task main loop
    
    // WiFi transition deferral state
    bool wifiTransitionActive = false;      // True during WiFi reconnect/disconnect
    unsigned long wifiTransitionStartMs = 0; // When deferral started
    bool deferralExpired() const;
    
    // Render state guard - prevents SD flush during display render
    volatile bool renderActive = false;     // True during display.update()
};

extern DebugLogger debugLogger;

#endif  // DEBUG_LOGGER_H

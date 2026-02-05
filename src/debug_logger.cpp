/**
 * Debug Logger implementation.
 * Writes timestamped lines to SD (preferred) or LittleFS when enabled.
 */

#include "debug_logger.h"
#include "storage_manager.h"
#include "perf_metrics.h"

#include <stdarg.h>
#include <sys/time.h>  // For settimeofday
#include <esp_heap_caps.h>  // For heap_caps_get_largest_free_block
#include <Preferences.h>    // For RTC time cache persistence
#include <new>  // For std::nothrow

// NVS namespace for RTC time cache
static constexpr const char* RTC_CACHE_NS = "rtc_cache";

DebugLogger debugLogger;

void DebugLogger::begin() {
    // No-op for now; storage mount handled by StorageManager.
}

void DebugLogger::setEnabled(bool enabledFlag) {
    // Debug logging requires SD card - LittleFS is too small for 1GB log cap
    bool wasEnabled = enabled;
    enabled = enabledFlag && storageManager.isReady() && storageManager.isSDCard();
    
    if (enabled && !wasEnabled) {
        rotateIfNeeded();
        bufferPos = 0;
        lastFlushMs = millis();
        // NOTE: Async mode must be enabled explicitly via enableAsyncMode()
        // This allows caller to control when the background task starts
    } else if (!enabled && wasEnabled) {
        // Disable async mode first (drains queue)
        disableAsyncMode();
        // Flush any remaining data before disabling
        flushBuffer();
    }
}

bool DebugLogger::canEnable() const {
    return storageManager.isReady() && storageManager.isSDCard();
}

void DebugLogger::setFilter(const DebugLogFilter& newFilter) {
    filter = newFilter;
}

bool DebugLogger::categoryAllowed(DebugLogCategory category) const {
    if (!enabled) return false;

    switch (category) {
        case DebugLogCategory::Alerts:      return filter.alerts;
        case DebugLogCategory::Wifi:        return filter.wifi;
        case DebugLogCategory::Ble:         return filter.ble;
        case DebugLogCategory::Gps:         return filter.gps;
        case DebugLogCategory::Obd:         return filter.obd;
        case DebugLogCategory::System:      return filter.system;
        case DebugLogCategory::Display:     return filter.display;
        case DebugLogCategory::PerfMetrics: return filter.perfMetrics;
        case DebugLogCategory::Audio:       return filter.audio;
        case DebugLogCategory::Camera:      return filter.camera;
        case DebugLogCategory::Lockout:     return filter.lockout;
        case DebugLogCategory::Touch:       return filter.touch;
        default: return false;
    }
}

bool DebugLogger::isEnabledFor(DebugLogCategory category) const {
    return categoryAllowed(category);
}

bool DebugLogger::storageReady() const {
    return storageManager.isReady();
}

bool DebugLogger::onSdCard() const {
    return storageManager.isReady() && storageManager.isSDCard();
}

void DebugLogger::rotateIfNeeded() {
    if (!storageManager.isReady()) return;
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return;

    // Mutex protection for SD access
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) return;  // Failed to acquire mutex

    rotateIfNeededUnlocked(fs);
}

// Internal helper - caller must hold SD mutex
void DebugLogger::rotateIfNeededUnlocked(fs::FS* fs) {
    if (!fs->exists(DEBUG_LOG_PATH)) return;

    File f = fs->open(DEBUG_LOG_PATH, FILE_READ);
    if (!f) return;
    size_t currentSize = f.size();
    f.close();

    if (currentSize >= DEBUG_LOG_MAX_BYTES) {
        fs->remove(DEBUG_LOG_PATH);  // Simple truncate strategy
    }
}

void DebugLogger::bufferLine(const char* line) {
    if (!enabled) return;
    
    size_t lineLen = strlen(line);
    bool needsNewline = (lineLen == 0 || line[lineLen - 1] != '\n');
    size_t totalLen = lineLen + (needsNewline ? 1 : 0);
    
    // If line is too big for buffer, flush first then write directly
    if (totalLen > DEBUG_LOG_BUFFER_SIZE) {
        flushBuffer();
        // Write oversized line directly to file (with mutex protection)
        fs::FS* fs = storageManager.getFilesystem();
        if (fs) {
            StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
            if (lock) {
                File f = fs->open(DEBUG_LOG_PATH, FILE_APPEND, true);
                if (f) {
                    f.print(line);
                    if (needsNewline) f.print('\n');
                    f.close();
                }
            }
        }
        return;
    }
    
    // If line won't fit in remaining buffer, flush first
    if (bufferPos + totalLen > DEBUG_LOG_BUFFER_SIZE) {
        flushBuffer();
    }
    
    // Append to buffer
    memcpy(buffer + bufferPos, line, lineLen);
    bufferPos += lineLen;
    if (needsNewline) {
        buffer[bufferPos++] = '\n';
    }
    
    // Flush if buffer is getting full
    if (bufferPos >= DEBUG_LOG_FLUSH_THRESHOLD) {
        flushBuffer();
    }
}

void DebugLogger::flushBuffer() {
    if (bufferPos == 0) return;
    if (!storageManager.isReady()) return;
    
    if (asyncMode && writeQueue) {
        flushBufferAsync();
    } else {
        flushBufferSync();
    }
}

void DebugLogger::flushBufferSync() {
    if (bufferPos == 0) return;
    if (!storageManager.isReady()) return;
    
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return;
    
    // Mutex protection for SD access (includes rotateIfNeeded)
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        // Failed to acquire mutex - don't block, try again next cycle
        return;
    }
    
    rotateIfNeededUnlocked(fs);  // Already holding mutex
    
    uint32_t startUs = PERF_TIMESTAMP_US();
    File f = fs->open(DEBUG_LOG_PATH, FILE_APPEND, true);
    if (f) {
        f.write((uint8_t*)buffer, bufferPos);
        f.close();
    }
    uint32_t durUs = PERF_TIMESTAMP_US() - startUs;
    perfRecordSdFlushUs(durUs);
    
    bufferPos = 0;
    lastFlushMs = millis();
}

void DebugLogger::flushBufferAsync() {
    if (bufferPos == 0 || !writeQueue) return;
    
    // Allocate message on heap (queue holds pointers, not data)
    WriteMessage* msg = new (std::nothrow) WriteMessage();
    if (!msg) {
        // Heap exhausted - DROP, don't block (per project rules: drops OK, blocking NOT OK)
        logDropCount.fetch_add(1, std::memory_order_relaxed);
        bufferPos = 0;  // Discard buffer
        lastFlushMs = millis();
        return;
    }
    
    // Copy buffer to message (truncate if buffer > message size)
    size_t copyLen = (bufferPos <= DEBUG_LOG_MESSAGE_SIZE) ? bufferPos : DEBUG_LOG_MESSAGE_SIZE;
    memcpy(msg->data, buffer, copyLen);
    msg->length = copyLen;
    
    // Non-blocking queue send (0 timeout - never block the hot path)
    if (xQueueSend(writeQueue, &msg, 0) != pdTRUE) {
        // Queue full - DROP, don't block (per project rules: drops OK, blocking NOT OK)
        delete msg;
        logDropCount.fetch_add(1, std::memory_order_relaxed);
        bufferPos = 0;  // Discard buffer
        lastFlushMs = millis();
        return;
    }
    
    // Track queue high-water mark (for monitoring queue pressure)
    UBaseType_t depth = uxQueueMessagesWaiting(writeQueue);
    uint32_t oldHW = logQueueHW.load(std::memory_order_relaxed);
    while (depth > oldHW && !logQueueHW.compare_exchange_weak(oldHW, depth, std::memory_order_relaxed)) {
        // CAS loop to update max
    }
    
    // Buffer queued successfully - clear local buffer
    bufferPos = 0;
    lastFlushMs = millis();
}

void DebugLogger::update() {
    if (!enabled || bufferPos == 0) return;
    
    // Time-based flush - but defer during WiFi transitions or display render
    if (millis() - lastFlushMs >= DEBUG_LOG_FLUSH_INTERVAL_MS) {
        // Defer flush during WiFi transitions unless deferral expired (max 5s)
        if (wifiTransitionActive && !deferralExpired()) {
            // Still defer - but check if buffer is critically full (>90%)
            if (bufferPos < DEBUG_LOG_BUFFER_SIZE * 9 / 10) {
                return;  // Safe to defer
            }
            // Buffer nearly full - force flush even during transition
        }
        // Defer flush during display render to avoid SD/render collision
        // This prevents the "perfect storm" where SD+render coincide
        if (renderActive) {
            // Buffer not critical - skip this flush, will catch next cycle
            if (bufferPos < DEBUG_LOG_BUFFER_SIZE * 9 / 10) {
                return;
            }
            // Buffer nearly full - must flush even during render
        }
        flushBuffer();
    }
}

void DebugLogger::notifyWifiTransition(bool stable) {
    if (stable) {
        // WiFi stable - clear deferral and flush any pending data
        if (wifiTransitionActive) {
            unsigned long deferredMs = millis() - wifiTransitionStartMs;
            wifiTransitionActive = false;
            wifiTransitionStartMs = 0;
            // Immediately flush any buffered data that was deferred
            if (enabled && bufferPos > 0) {
                flushBuffer();
            }
            // Log the deferral duration after flushing (so it appears in logs)
            if (enabled && categoryAllowed(DebugLogCategory::System)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "[Logger] SD write deferral ended after %lums (WiFi stable)", deferredMs);
                log(DebugLogCategory::System, msg);
            }
        }
    } else {
        // WiFi transitioning (disconnect, connecting, reconnecting)
        if (!wifiTransitionActive) {
            wifiTransitionActive = true;
            wifiTransitionStartMs = millis();
            // Log deferral start (this goes to buffer, won't block)
            if (enabled && categoryAllowed(DebugLogCategory::System)) {
                log(DebugLogCategory::System, "[Logger] SD write deferral started (WiFi transitioning)");
            }
        }
    }
}

bool DebugLogger::deferralExpired() const {
    if (!wifiTransitionActive) return true;
    return (millis() - wifiTransitionStartMs) >= WIFI_TRANSITION_DEFER_MAX_MS;
}

void DebugLogger::notifyRenderState(bool rendering) {
    // Simple flag to prevent SD flush during display render
    // Volatile write is atomic on ESP32, no lock needed
    renderActive = rendering;
}

// ============= Main Logging Methods =============

void DebugLogger::flush() {
    flushBuffer();
}

void DebugLogger::logf(const char* fmt, ...) {
    if (!categoryAllowed(DebugLogCategory::System)) return;

    char buffer[384];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    log(DebugLogCategory::System, buffer);
}

void DebugLogger::log(const char* message) {
    if (!categoryAllowed(DebugLogCategory::System)) return;

    log(DebugLogCategory::System, message);
}

void DebugLogger::logf(DebugLogCategory category, const char* fmt, ...) {
    if (!categoryAllowed(category)) return;

    char msgBuffer[384];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuffer, sizeof(msgBuffer), fmt, args);
    va_end(args);

    log(category, msgBuffer);
}

void DebugLogger::log(DebugLogCategory category, const char* message) {
    if (!categoryAllowed(category)) return;

    char line[512];
    formatJsonLine(line, sizeof(line), category, message);
    bufferLine(line);
}

void DebugLogger::logEvent(DebugLogCategory category, const char* event, const char* extraFields) {
    if (!categoryAllowed(category)) return;
    
    char line[512];
    formatJsonLine(line, sizeof(line), category, event, extraFields);
    bufferLine(line);
}

void DebugLogger::logPerfMetrics(const char* fields) {
    if (!categoryAllowed(DebugLogCategory::PerfMetrics)) return;
    
    unsigned long now = millis();
    char line[512];
    
    // Structured NDJSON: numeric fields are first-class, no message escaping
    char ts[32];
    if (timeValid) {
        getISO8601Timestamp(ts, sizeof(ts));
    } else {
        strcpy(ts, "1970-01-01T00:00:00Z");
    }
    snprintf(line, sizeof(line),
        "{\"@timestamp\":\"%s\",\"millis\":%lu,\"category\":\"perf\",%s}",
        ts, now, fields);
    
    bufferLine(line);
}

const char* DebugLogger::categoryName(DebugLogCategory category) const {
    switch (category) {
        case DebugLogCategory::Alerts:      return "alerts";
        case DebugLogCategory::Wifi:        return "wifi";
        case DebugLogCategory::Ble:         return "ble";
        case DebugLogCategory::Gps:         return "gps";
        case DebugLogCategory::Obd:         return "obd";
        case DebugLogCategory::System:      return "system";
        case DebugLogCategory::Display:     return "display";
        case DebugLogCategory::PerfMetrics: return "perf";
        case DebugLogCategory::Audio:       return "audio";
        case DebugLogCategory::Camera:      return "camera";
        case DebugLogCategory::Lockout:     return "lockout";
        case DebugLogCategory::Touch:       return "touch";
        default: return "unknown";
    }
}

void DebugLogger::formatJsonLine(char* dest, size_t destSize, DebugLogCategory category, 
                                  const char* message, const char* extraFields) {
    unsigned long now = millis();
    
    // Heap-free timestamp
    char ts[32];
    if (timeValid) {
        getISO8601Timestamp(ts, sizeof(ts));
    } else {
        strcpy(ts, "1970-01-01T00:00:00Z");
    }
    
    // Time source string for JSON
    const char* sourceStr = "none";
    switch (timeSource) {
        case TimeSource::GPS: sourceStr = "gps"; break;
        case TimeSource::NTP: sourceStr = "ntp"; break;
        case TimeSource::RTC: sourceStr = "rtc"; break;
        case TimeSource::ESTIMATED: sourceStr = "estimated"; break;
        default: sourceStr = "none"; break;
    }
    
    // Escape message for JSON - handle all control chars for NDJSON safety
    // Must escape: " \ and control chars (0x00-0x1F)
    char escapedMsg[256];
    size_t j = 0;
    for (size_t i = 0; message[i] && j < sizeof(escapedMsg) - 7; i++) {  // -7 for worst case \uXXXX + null
        unsigned char c = (unsigned char)message[i];
        if (c == '"') {
            escapedMsg[j++] = '\\';
            escapedMsg[j++] = '"';
        } else if (c == '\\') {
            escapedMsg[j++] = '\\';
            escapedMsg[j++] = '\\';
        } else if (c == '\n') {
            escapedMsg[j++] = '\\';
            escapedMsg[j++] = 'n';
        } else if (c == '\r') {
            escapedMsg[j++] = '\\';
            escapedMsg[j++] = 'r';
        } else if (c == '\t') {
            escapedMsg[j++] = '\\';
            escapedMsg[j++] = 't';
        } else if (c < 0x20) {
            // Other control chars - use \uXXXX format
            j += snprintf(escapedMsg + j, sizeof(escapedMsg) - j, "\\u%04x", c);
        } else {
            escapedMsg[j++] = c;
        }
    }
    escapedMsg[j] = '\0';
    
    if (extraFields && extraFields[0]) {
        snprintf(dest, destSize, 
            "{\"@timestamp\":\"%s\",\"millis\":%lu,\"timeSource\":\"%s\",\"category\":\"%s\",\"message\":\"%s\",%s}",
            ts, now, sourceStr, categoryName(category), escapedMsg, extraFields);
    } else {
        snprintf(dest, destSize, 
            "{\"@timestamp\":\"%s\",\"millis\":%lu,\"timeSource\":\"%s\",\"category\":\"%s\",\"message\":\"%s\"}",
            ts, now, sourceStr, categoryName(category), escapedMsg);
    }
}

// Validate GPS/NTP time is sane (year 2023-2050, month 1-12, day 1-31)
static bool isTimeSane(int year, int month, int day) {
    return (year >= 2023 && year <= 2050 && 
            month >= 1 && month <= 12 && 
            day >= 1 && day <= 31);
}

// Time synchronization from GPS
void DebugLogger::syncTimeFromGPS(int year, int month, int day, int hour, int minute, int second) {
    // Validate input before syncing
    if (!isTimeSane(year, month, day)) {
        Serial.printf("[DebugLogger] Invalid GPS time rejected: %d-%02d-%02d\n", year, month, day);
        return;
    }
    
    struct tm timeinfo;
    timeinfo.tm_year = year - 1900;  // tm_year is years since 1900
    timeinfo.tm_mon = month - 1;      // tm_mon is 0-11
    timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min = minute;
    timeinfo.tm_sec = second;
    timeinfo.tm_isdst = 0;  // UTC doesn't have DST
    
    timeSyncEpoch = mktime(&timeinfo);
    timeSyncMillis = millis();
    timeValid = true;
    timeSource = TimeSource::GPS;
    
    // Also set ESP32 system time for time() calls elsewhere
    struct timeval tv;
    tv.tv_sec = timeSyncEpoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    
    // Persist to NVS for next boot
    saveTimeToCache();
}

// Time synchronization from NTP (reuses GPS sync but marks source as NTP)
void DebugLogger::syncTimeFromNTP(int year, int month, int day, int hour, int minute, int second) {
    syncTimeFromGPS(year, month, day, hour, minute, second);
    timeSource = TimeSource::NTP;  // Override source to NTP
    saveTimeToCache();  // Persist to NVS for next boot
}

// Save current time to NVS cache (called automatically on GPS/NTP sync)
void DebugLogger::saveTimeToCache() {
    if (!timeValid || timeSyncEpoch < 1700000000) {  // Sanity: must be after ~Nov 2023
        return;
    }
    
    Preferences prefs;
    if (prefs.begin(RTC_CACHE_NS, false)) {
        prefs.putULong64("epoch", (uint64_t)timeSyncEpoch);
        prefs.putUChar("source", (uint8_t)timeSource);
        prefs.end();
        Serial.printf("[RTC] Saved time cache: epoch=%lu source=%d\n", 
                      (unsigned long)timeSyncEpoch, (int)timeSource);
    }
}

// Restore time from NVS cache (call early in setup)
// Returns true if valid cached time was restored
bool DebugLogger::restoreTimeFromCache() {
    Preferences prefs;
    if (!prefs.begin(RTC_CACHE_NS, true)) {
        Serial.println("[RTC] No time cache namespace found");
        return false;
    }
    
    uint64_t cachedEpoch = prefs.getULong64("epoch", 0);
    uint8_t cachedSource = prefs.getUChar("source", 0);
    prefs.end();
    
    // Sanity check: must be after Nov 2023 and not in far future
    if (cachedEpoch < 1700000000 || cachedEpoch > 2000000000) {
        Serial.printf("[RTC] Cached epoch invalid: %llu\n", cachedEpoch);
        return false;
    }
    
    // Check age - don't use if too old (drift would be significant)
    // We can't know real elapsed time without RTC hardware, so we just
    // restore the cached time and let GPS/NTP correct it later
    timeSyncEpoch = (time_t)cachedEpoch;
    timeSyncMillis = millis();
    timeValid = true;
    timeSource = TimeSource::RTC;
    
    // Set ESP32 system time
    struct timeval tv;
    tv.tv_sec = timeSyncEpoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    
    // Log restored time
    struct tm timeinfo;
    gmtime_r(&timeSyncEpoch, &timeinfo);
    Serial.printf("[RTC] Restored cached time: %04d-%02d-%02d %02d:%02d:%02d UTC (source was %s)\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                  cachedSource == 1 ? "GPS" : cachedSource == 2 ? "NTP" : "unknown");
    
    return true;
}

time_t DebugLogger::getUnixTime() const {
    if (!timeValid) return 0;
    
    // Calculate current time based on sync point + elapsed millis
    unsigned long elapsed = millis() - timeSyncMillis;
    return timeSyncEpoch + (elapsed / 1000);
}

void DebugLogger::getISO8601Timestamp(char* buf, size_t bufSize) const {
    time_t now = getUnixTime();
    if (now == 0 || bufSize < 21) {
        if (bufSize > 0) strncpy(buf, "1970-01-01T00:00:00Z", bufSize - 1);
        return;
    }
    
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);  // Thread-safe, no heap
    strftime(buf, bufSize, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
}

bool DebugLogger::exists() const {
    if (!storageManager.isReady()) return false;
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return false;
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) return false;
    return fs->exists(DEBUG_LOG_PATH);
}

size_t DebugLogger::size() const {
    if (!storageManager.isReady()) return 0;
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return 0;
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) return 0;
    File f = fs->open(DEBUG_LOG_PATH, FILE_READ);
    if (!f) return 0;
    size_t sz = f.size();
    f.close();
    return sz;
}

bool DebugLogger::clear() {
    if (!storageManager.isReady()) return false;
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return false;
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) return false;
    if (fs->exists(DEBUG_LOG_PATH)) {
        return fs->remove(DEBUG_LOG_PATH);
    }
    return true;  // Nothing to clear
}

String DebugLogger::tail(size_t maxBytes) const {
    if (!storageManager.isReady()) return "[Storage not ready]";
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return "[Filesystem unavailable]";
    
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) return "[SD busy]";
    
    if (!fs->exists(DEBUG_LOG_PATH)) return "[No log file]";

    File f = fs->open(DEBUG_LOG_PATH, FILE_READ);
    if (!f) return "[Failed to open log]";

    size_t fileSize = f.size();
    if (fileSize == 0) {
        f.close();
        return "[Log file empty]";
    }

    // Read from the end of the file
    size_t bytesToRead = (fileSize < maxBytes) ? fileSize : maxBytes;
    size_t startPos = fileSize - bytesToRead;

    // If not reading from start, try to align to start of a line
    if (startPos > 0) {
        f.seek(startPos);
        // Skip partial line by finding next newline
        while (startPos < fileSize && f.read() != '\n') {
            startPos++;
        }
        startPos++;  // Move past the newline
        if (startPos >= fileSize) {
            f.close();
            return "[Log too fragmented]";
        }
        bytesToRead = fileSize - startPos;
    }

    f.seek(startPos);
    String content;
    content.reserve(bytesToRead + 1);

    // Read in chunks to avoid memory issues
    char buffer[512];
    size_t remaining = bytesToRead;
    while (remaining > 0) {
        size_t chunkSize = (remaining < sizeof(buffer)) ? remaining : sizeof(buffer);
        size_t bytesRead = f.read((uint8_t*)buffer, chunkSize);
        if (bytesRead == 0) break;
        content.concat(buffer, bytesRead);
        remaining -= bytesRead;
    }

    f.close();
    return content;
}

// ============================================================================
// Async Mode - FreeRTOS task for non-blocking SD writes
// ============================================================================

void DebugLogger::enableAsyncMode() {
    if (asyncMode) return;  // Already enabled
    
    // Create queue for write messages (pointers to WriteMessage structs)
    if (!writeQueue) {
        writeQueue = xQueueCreate(DEBUG_LOG_QUEUE_DEPTH, sizeof(WriteMessage*));
        if (!writeQueue) {
            Serial.println("[DebugLog] ERROR: Failed to create write queue");
            return;
        }
        Serial.printf("[DebugLog] Write queue created (%u items x %u bytes/msg)\n",
                      (unsigned)DEBUG_LOG_QUEUE_DEPTH, (unsigned)DEBUG_LOG_MESSAGE_SIZE);
    }
    
    // Create writer task on Core 0 at low priority
    if (!writerTaskHandle) {
        BaseType_t result = xTaskCreatePinnedToCore(
            writerTaskEntry,                // Entry function
            "DebugLogWriter",               // Task name
            DEBUG_LOG_WRITER_STACK_SIZE,    // Stack size
            this,                           // Parameter (this pointer)
            1,                              // Priority (low - 1 above idle)
            &writerTaskHandle,              // Task handle
            0                               // Core 0 (protocol core)
        );
        
        if (result != pdPASS) {
            Serial.println("[DebugLog] ERROR: Failed to create writer task");
            return;
        }
        Serial.println("[DebugLog] Writer task started on Core 0");
    }
    
    asyncMode = true;
}

void DebugLogger::disableAsyncMode() {
    if (!asyncMode) return;
    
    asyncMode = false;
    
    // Flush any pending data synchronously
    if (bufferPos > 0) {
        flushBufferSync();
    }
    
    // Drain and process remaining queued messages synchronously
    if (writeQueue) {
        WriteMessage* msg = nullptr;
        while (xQueueReceive(writeQueue, &msg, 0) == pdTRUE) {
            if (msg) {
                // Write directly with mutex protection
                fs::FS* fs = storageManager.getFilesystem();
                if (fs) {
                    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
                    if (lock) {
                        rotateIfNeededUnlocked(fs);
                        File f = fs->open(DEBUG_LOG_PATH, FILE_APPEND, true);
                        if (f) {
                            f.write((uint8_t*)msg->data, msg->length);
                            f.close();
                        }
                    }
                }
                delete msg;
            }
        }
    }
    
    // Note: We don't delete the task or queue - they can be re-enabled
    // This avoids complexity of task termination synchronization
    Serial.println("[DebugLog] Async mode disabled, using sync writes");
}

void DebugLogger::writerTaskEntry(void* param) {
    DebugLogger* self = static_cast<DebugLogger*>(param);
    self->writerTaskLoop();
}

void DebugLogger::writerTaskLoop() {
    Serial.println("[DebugLog] Writer task running");
    
    while (true) {
        WriteMessage* msg = nullptr;
        
        // Block waiting for messages (portMAX_DELAY = infinite wait)
        if (xQueueReceive(writeQueue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        
        if (!msg) continue;
        
        // Write to SD with mutex protection
        if (storageManager.isReady()) {
            fs::FS* fs = storageManager.getFilesystem();
            if (fs) {
                StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
                if (lock) {
                    rotateIfNeededUnlocked(fs);
                    
                    uint32_t startUs = micros();
                    File f = fs->open(DEBUG_LOG_PATH, FILE_APPEND, true);
                    if (f) {
                        f.write((uint8_t*)msg->data, msg->length);
                        f.close();
                    }
                    uint32_t durUs = micros() - startUs;
                    perfRecordSdFlushUs(durUs);
                }
            }
        }
        
        // Free the message
        delete msg;
        
        // Yield to allow other tasks to run
        taskYIELD();
    }
}
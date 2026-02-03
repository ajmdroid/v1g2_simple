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
    } else if (!enabled && wasEnabled) {
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
        // Write oversized line directly to file
        fs::FS* fs = storageManager.getFilesystem();
        if (fs) {
            File f = fs->open(DEBUG_LOG_PATH, FILE_APPEND, true);
            if (f) {
                f.print(line);
                if (needsNewline) f.print('\n');
                f.close();
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
    
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return;
    
    rotateIfNeeded();
    
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

void DebugLogger::update() {
    if (!enabled || bufferPos == 0) return;
    
    // Time-based flush - but defer during WiFi transitions to avoid NVS contention
    if (millis() - lastFlushMs >= DEBUG_LOG_FLUSH_INTERVAL_MS) {
        // Defer flush during WiFi transitions unless deferral expired (max 5s)
        if (wifiTransitionActive && !deferralExpired()) {
            // Still defer - but check if buffer is critically full (>90%)
            if (bufferPos < DEBUG_LOG_BUFFER_SIZE * 9 / 10) {
                return;  // Safe to defer
            }
            // Buffer nearly full - force flush even during transition
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

// ============= Breadcrumb Ring Buffer (Zero-Heap Incident Context) =============

void DebugLogger::breadcrumb(const char* msg) {
    if (!msg || !msg[0]) return;
    
    // Format: "[millis] message\n" - compact, parseable
    // Format OUTSIDE critical section to minimize lock time
    char line[BREADCRUMB_MAX_LINE];
    int len = snprintf(line, sizeof(line), "[%lu] %s\n", millis(), msg);
    if (len <= 0) return;
    if ((size_t)len >= sizeof(line)) len = sizeof(line) - 1;
    
    // Write to ring buffer with spinlock (ISR-safe critical section)
    portENTER_CRITICAL(&breadcrumbMux);
    for (int i = 0; i < len; i++) {
        breadcrumbRing[breadcrumbHead] = line[i];
        breadcrumbHead = (breadcrumbHead + 1) % BREADCRUMB_RING_SIZE;
        if (breadcrumbCount < BREADCRUMB_RING_SIZE) {
            breadcrumbCount++;
        } else {
            breadcrumbWrapped = true;
        }
    }
    portEXIT_CRITICAL(&breadcrumbMux);
}

void DebugLogger::breadcrumbf(const char* fmt, ...) {
    char msg[BREADCRUMB_MAX_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    breadcrumb(msg);
}

void DebugLogger::captureIncident(const char* reason, uint32_t loopMaxUs, uint32_t qDropDelta) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) return;
    if (breadcrumbCount == 0) return;  // Nothing to dump
    
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return;
    
    // PHASE 1: Snapshot ring buffer state under spinlock (fast, no SD I/O)
    // This ensures consistent capture even if breadcrumbs are being written concurrently
    size_t snapHead, snapCount;
    bool snapWrapped;
    
    portENTER_CRITICAL(&breadcrumbMux);
    snapHead = breadcrumbHead;
    snapCount = breadcrumbCount;
    snapWrapped = breadcrumbWrapped;
    portEXIT_CRITICAL(&breadcrumbMux);
    
    if (snapCount == 0) return;  // Double-check after snapshot
    
    // PHASE 2: Write to SD (slow, outside critical section)
    File f = fs->open(INCIDENT_LOG_PATH, FILE_APPEND, true);
    if (!f) return;
    
    // Header with context
    char ts[32];
    if (timeValid) {
        getISO8601Timestamp(ts, sizeof(ts));
    } else {
        strcpy(ts, "1970-01-01T00:00:00Z");
    }
    
    f.printf("\n========== INCIDENT %s ==========\n", ts);
    f.printf("Reason: %s\n", reason);
    f.printf("loopMax_us: %lu, qDropDelta: %lu, millis: %lu\n", loopMaxUs, qDropDelta, millis());
    f.printf("Breadcrumb buffer: %u bytes, wrapped: %s\n", snapCount, snapWrapped ? "yes" : "no");
    f.println("--- Breadcrumb dump (oldest first) ---");
    
    // Dump ring buffer in chronological order using snapshot state
    // Note: We read from breadcrumbRing[] without lock - acceptable since:
    // 1. We're reading stale data at worst (snapshot was consistent)
    // 2. New writes may overwrite, but header shows correct snapshot size
    if (snapWrapped) {
        // Buffer wrapped - read from head to end, then start to head
        size_t readPos = snapHead;
        for (size_t i = 0; i < BREADCRUMB_RING_SIZE; i++) {
            f.write(breadcrumbRing[readPos]);
            readPos = (readPos + 1) % BREADCRUMB_RING_SIZE;
        }
    } else {
        // Buffer hasn't wrapped - read from start to count
        f.write((uint8_t*)breadcrumbRing, snapCount);
    }
    
    f.println("--- End breadcrumb dump ---\n");
    f.close();
    
    // Log that we captured an incident
    if (enabled && categoryAllowed(DebugLogCategory::System)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[Logger] Incident captured: %s (loopMax=%luus, qDrop=%lu)", 
                 reason, loopMaxUs, qDropDelta);
        log(DebugLogCategory::System, msg);
    }
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
    if (logFormat == DebugLogFormat::JSON) {
        formatJsonLine(line, sizeof(line), category, message);
    } else {
        formatTextLine(line, sizeof(line), category, message);
    }
    
    // Async mode: enqueue for background writer task
    if (asyncMode && writeQueue) {
        LogQueueItem item;
        size_t len = strlen(line);
        if (len >= LOG_QUEUE_LINE_SIZE) len = LOG_QUEUE_LINE_SIZE - 1;
        memcpy(item.line, line, len);
        item.line[len] = '\0';
        item.length = static_cast<uint16_t>(len);
        item.flags = LogQueueItem::FLAG_NONE;
        
        // Non-blocking send - drop if queue full to avoid blocking main loop
        if (xQueueSend(writeQueue, &item, 0) != pdTRUE) {
            asyncDropCount++;  // Atomically increment drop counter
        }
        return;
    }
    
    // Sync mode: buffer directly
    bufferLine(line);
}

void DebugLogger::logEvent(DebugLogCategory category, const char* event, const char* extraFields) {
    if (!categoryAllowed(category)) return;
    
    char line[512];
    if (logFormat == DebugLogFormat::JSON) {
        formatJsonLine(line, sizeof(line), category, event, extraFields);
    } else {
        // For TEXT format, just log as message with any extra fields appended
        char msg[256];
        if (extraFields && extraFields[0]) {
            snprintf(msg, sizeof(msg), "%s | %s", event, extraFields);
        } else {
            snprintf(msg, sizeof(msg), "%s", event);
        }
        formatTextLine(line, sizeof(line), category, msg);
    }
    
    // Async mode: enqueue for background writer task
    if (asyncMode && writeQueue) {
        LogQueueItem item;
        size_t len = strlen(line);
        if (len >= LOG_QUEUE_LINE_SIZE) len = LOG_QUEUE_LINE_SIZE - 1;
        memcpy(item.line, line, len);
        item.line[len] = '\0';
        item.length = static_cast<uint16_t>(len);
        item.flags = LogQueueItem::FLAG_NONE;
        
        if (xQueueSend(writeQueue, &item, 0) != pdTRUE) {
            asyncDropCount++;
        }
        return;
    }
    
    bufferLine(line);
}

void DebugLogger::logPerfMetrics(const char* fields) {
    if (!categoryAllowed(DebugLogCategory::PerfMetrics)) return;
    
    unsigned long now = millis();
    char line[512];
    
    if (logFormat == DebugLogFormat::JSON) {
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
    } else {
        // TEXT format: METRICS prefix for grep-ability
        snprintf(line, sizeof(line), "[%10lu ms] METRICS %s", now, fields);
    }
    
    // Async mode: enqueue for background writer task
    if (asyncMode && writeQueue) {
        LogQueueItem item;
        size_t len = strlen(line);
        if (len >= LOG_QUEUE_LINE_SIZE) len = LOG_QUEUE_LINE_SIZE - 1;
        memcpy(item.line, line, len);
        item.line[len] = '\0';
        item.length = static_cast<uint16_t>(len);
        item.flags = LogQueueItem::FLAG_NONE;
        
        if (xQueueSend(writeQueue, &item, 0) != pdTRUE) {
            asyncDropCount++;
        }
        return;
    }
    
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

void DebugLogger::formatTextLine(char* dest, size_t destSize, DebugLogCategory category, const char* message) {
    unsigned long now = millis();
    
    if (timeValid) {
        // Use real timestamp if available (heap-free)
        char ts[32];
        getISO8601Timestamp(ts, sizeof(ts));
        snprintf(dest, destSize, "[%s] [%10lu ms] %s", ts, now, message);
    } else {
        // Fall back to millis only
        snprintf(dest, destSize, "[%10lu ms] %s", now, message);
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
}

// Time synchronization from NTP (reuses GPS sync but marks source as NTP)
void DebugLogger::syncTimeFromNTP(int year, int month, int day, int hour, int minute, int second) {
    syncTimeFromGPS(year, month, day, hour, minute, second);
    timeSource = TimeSource::NTP;  // Override source to NTP
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
    return fs->exists(DEBUG_LOG_PATH);
}

size_t DebugLogger::size() const {
    if (!storageManager.isReady()) return 0;
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return 0;
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
    if (fs->exists(DEBUG_LOG_PATH)) {
        return fs->remove(DEBUG_LOG_PATH);
    }
    return true;  // Nothing to clear
}

String DebugLogger::tail(size_t maxBytes) const {
    if (!storageManager.isReady()) return "[Storage not ready]";
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return "[Filesystem unavailable]";
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
// =============================================================================
// Async Write Mode - Background FreeRTOS Task for SD I/O
// =============================================================================

void DebugLogger::setAsyncMode(bool async) {
    if (async == asyncMode) return;  // No change
    
    if (async) {
        // Enable async mode - create queue and task if not already running
        if (!writeQueue) {
            // Log heap state before allocation for diagnostics
            uint32_t freeHeap = ESP.getFreeHeap();
            uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
            size_t fullQueueBytes = LOG_QUEUE_DEPTH * sizeof(LogQueueItem);
            
            // Try full queue size first
            writeQueue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(LogQueueItem));
            
            // If full queue fails, try smaller queue as fallback (16 items instead of 32)
            if (!writeQueue && LOG_QUEUE_DEPTH > 16) {
                Serial.printf("[Logger] Full queue (%u items, ~%u bytes) failed, trying reduced queue...\n",
                              (unsigned)LOG_QUEUE_DEPTH, (unsigned)fullQueueBytes);
                writeQueue = xQueueCreate(16, sizeof(LogQueueItem));
            }
            
            if (!writeQueue) {
                // Queue creation failed - stay in sync mode, log diagnostics
                Serial.printf("[Logger] ERROR: Queue alloc failed (need ~%u bytes, heap=%u, block=%u)\n",
                              (unsigned)fullQueueBytes, (unsigned)freeHeap, (unsigned)largestBlock);
                if (enabled && categoryAllowed(DebugLogCategory::System)) {
                    logf(DebugLogCategory::System, 
                         "[Logger] ERROR: Queue alloc failed (need ~%u, heap=%u, block=%u)",
                         (unsigned)fullQueueBytes, (unsigned)freeHeap, (unsigned)largestBlock);
                }
                return;
            }
            
            Serial.printf("[Logger] Queue created (heap before=%u, block=%u)\n",
                          (unsigned)freeHeap, (unsigned)largestBlock);
        }
        
        if (!writerTaskHandle) {
            // Pin to Core 0 (protocol CPU) at lowest priority so it doesn't impact display/main loop on Core 1
            BaseType_t result = xTaskCreatePinnedToCore(
                writerTaskEntry,           // Entry function
                "LogWriter",               // Task name
                LOG_WRITER_STACK_SIZE,     // Stack size (4KB)
                this,                      // Parameter (this pointer)
                1,                         // Priority (lowest - 1 above idle)
                &writerTaskHandle,         // Task handle
                0                          // Core 0 (protocol core)
            );
            
            if (result != pdPASS) {
                // Task creation failed - stay in sync mode
                Serial.println("[Logger] ERROR: Failed to create writer task");
                if (enabled && categoryAllowed(DebugLogCategory::System)) {
                    log(DebugLogCategory::System, "[Logger] ERROR: Failed to create writer task");
                }
                return;
            }
        }
        
        asyncMode = true;
        
        if (enabled && categoryAllowed(DebugLogCategory::System)) {
            log(DebugLogCategory::System, "[Logger] Async write mode ENABLED (writer task running on Core 0)");
        }
    } else {
        // Disable async mode - flush any pending items and stop task
        asyncMode = false;  // Set first so log() stops enqueueing
        
        // Signal task to drain queue (send a flush item)
        if (writeQueue) {
            LogQueueItem flushItem;
            flushItem.length = 0;
            flushItem.flags = LogQueueItem::FLAG_FLUSH_NOW;
            xQueueSend(writeQueue, &flushItem, pdMS_TO_TICKS(100));
            
            // Wait briefly for task to drain
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        if (enabled && categoryAllowed(DebugLogCategory::System)) {
            log(DebugLogCategory::System, "[Logger] Async write mode DISABLED (sync writes restored)");
        }
        
        // Note: We don't delete the task/queue - keep them for potential re-enable
        // This avoids heap fragmentation from repeated create/delete cycles
    }
}

// Static entry point for FreeRTOS task
void DebugLogger::writerTaskEntry(void* param) {
    DebugLogger* logger = static_cast<DebugLogger*>(param);
    logger->writerTaskLoop();
}

// Writer task main loop - runs on Core 0, drains queue to SD
void DebugLogger::writerTaskLoop() {
    LogQueueItem item;
    
    for (;;) {
        // Block waiting for items (indefinitely)
        if (xQueueReceive(writeQueue, &item, portMAX_DELAY) == pdTRUE) {
            // Check for flush-only signal (length=0)
            if (item.length == 0 && (item.flags & LogQueueItem::FLAG_FLUSH_NOW)) {
                // Drain remaining queue items
                while (xQueueReceive(writeQueue, &item, 0) == pdTRUE) {
                    if (item.length > 0) {
                        // Write this item
                        bufferLine(item.line);
                    }
                }
                // Force flush buffer to SD
                flushBuffer();
                continue;
            }
            
            // Normal item - buffer it
            if (item.length > 0) {
                bufferLine(item.line);
            }
            
            // Periodic flush - check if buffer needs flushing
            if (bufferPos >= DEBUG_LOG_FLUSH_THRESHOLD) {
                flushBuffer();
            }
        }
    }
}
/**
 * Debug Logger implementation.
 * Writes timestamped lines to SD (preferred) or LittleFS when enabled.
 */

#include "debug_logger.h"
#include "storage_manager.h"

#include <stdarg.h>

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
    
    File f = fs->open(DEBUG_LOG_PATH, FILE_APPEND, true);
    if (f) {
        f.write((uint8_t*)buffer, bufferPos);
        f.close();
    }
    
    bufferPos = 0;
    lastFlushMs = millis();
}

void DebugLogger::update() {
    if (!enabled || bufferPos == 0) return;
    
    // Time-based flush
    if (millis() - lastFlushMs >= DEBUG_LOG_FLUSH_INTERVAL_MS) {
        flushBuffer();
    }
}

void DebugLogger::flush() {
    flushBuffer();
}

void DebugLogger::logf(const char* fmt, ...) {
    if (!categoryAllowed(DebugLogCategory::System)) return;

    char buffer[256];
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

    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    log(category, buffer);
}

void DebugLogger::log(DebugLogCategory category, const char* message) {
    if (!categoryAllowed(category)) return;

    char line[320];
    unsigned long now = millis();
    snprintf(line, sizeof(line), "[%10lu ms] %s", now, message);
    bufferLine(line);
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

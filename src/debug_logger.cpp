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
    enabled = enabledFlag && storageManager.isReady() && storageManager.isSDCard();
    if (enabled) {
        rotateIfNeeded();
        // Note: avoid recursive logging while enabling.
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
        case DebugLogCategory::Alerts:  return filter.alerts;
        case DebugLogCategory::Wifi:    return filter.wifi;
        case DebugLogCategory::Ble:     return filter.ble;
        case DebugLogCategory::Gps:     return filter.gps;
        case DebugLogCategory::Obd:     return filter.obd;
        case DebugLogCategory::System:  return filter.system;
        case DebugLogCategory::Display: return filter.display;
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

void DebugLogger::writeLine(const char* line) {
    if (!enabled) return;
    if (!storageManager.isReady()) return;

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return;

    rotateIfNeeded();

    // Use FILE_APPEND with create flag, or FILE_WRITE if file doesn't exist
    File f = fs->open(DEBUG_LOG_PATH, FILE_APPEND, true);  // true = create if missing
    if (!f) {
        return;
    }

    f.print(line);
    size_t len = strlen(line);
    if (len == 0 || line[len - 1] != '\n') {
        f.print('\n');
    }
    f.flush();
    f.close();
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
    writeLine(line);
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

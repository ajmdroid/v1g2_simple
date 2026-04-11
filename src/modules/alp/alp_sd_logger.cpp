/**
 * ALP SD Logger — implementation.
 *
 * CSV format: millis,event,from_state,to_state,byte0,byte1,byte2,checksum,gun,extra
 *
 * Uses non-blocking SDTryLock since this runs on Core 1 main loop.
 * Drops are expected and OK — we log what we can without blocking
 * the display pipeline.
 */

#include "alp_sd_logger.h"
#include "alp_runtime_module.h"

#include <cstdio>

#ifndef UNIT_TEST
#include <Arduino.h>
#include <FS.h>
#include "../../storage_manager.h"
#else
// Stubs for unit test builds
#endif

// ── Global instance ──────────────────────────────────────────────────
AlpSdLogger alpSdLogger;

static constexpr const char* ALP_DIR_PATH = "/alp";
static constexpr const char* ALP_CSV_HEADER =
    "millis,event,from_state,to_state,byte0,byte1,byte2,checksum,gun,extra\n";

// ── begin() ──────────────────────────────────────────────────────────

void AlpSdLogger::begin(bool enabled, bool sdReady) {
    enabled_ = enabled;
    sdReady_ = sdReady;
    dirReady_ = false;
    headerWritten_ = false;
    linesWritten_ = 0;
    dropCount_ = 0;

    if (!enabled || !sdReady) {
        return;
    }

#ifndef UNIT_TEST
    Serial.printf("[ALP_SD] begin: enabled=%d sdReady=%d\n", enabled, sdReady);
#endif
}

void AlpSdLogger::setBootId(uint32_t id) {
    bootId_ = id;
    // Build path: /alp/alp_<bootId>.csv
    snprintf(csvPathBuf_, sizeof(csvPathBuf_), "/alp/alp_%lu.csv",
             static_cast<unsigned long>(id));
    // Reset header state when boot ID changes
    headerWritten_ = false;

#ifndef UNIT_TEST
    if (enabled_ && sdReady_) {
        Serial.printf("[ALP_SD] path: %s\n", csvPathBuf_);
    }
#endif
}

void AlpSdLogger::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled) {
#ifndef UNIT_TEST
        Serial.printf("[ALP_SD] disabled (wrote %lu lines, dropped %lu)\n",
                      (unsigned long)linesWritten_, (unsigned long)dropCount_);
#endif
    } else {
#ifndef UNIT_TEST
        Serial.printf("[ALP_SD] enabled\n");
#endif
    }
}

// ── Logging methods ──────────────────────────────────────────────────

void AlpSdLogger::logStateTransition(uint32_t nowMs, AlpState from, AlpState to) {
    if (!enabled_ || !sdReady_) return;

    char line[128];
    snprintf(line, sizeof(line), "%lu,STATE,%s,%s,,,,,,\n",
             (unsigned long)nowMs,
             alpStateName(from),
             alpStateName(to));
    appendLine(line);
}

void AlpSdLogger::logHeartbeatByte1(uint32_t nowMs, uint8_t prevByte1, uint8_t newByte1,
                                     AlpState currentState) {
    if (!enabled_ || !sdReady_) return;

    char line[128];
    snprintf(line, sizeof(line), "%lu,HB_BYTE1,%s,,%02X,%02X,,,,%s\n",
             (unsigned long)nowMs,
             alpStateName(currentState),
             prevByte1, newByte1,
             newByte1 == 0x01 ? "ALERT" : "IDLE");
    appendLine(line);
}

void AlpSdLogger::logGunIdentified(uint32_t nowMs, AlpGunType gun, uint8_t byte0,
                                    uint8_t byte1or2, bool isObserveMode,
                                    AlpState currentState) {
    if (!enabled_ || !sdReady_) return;

    char line[128];
    snprintf(line, sizeof(line), "%lu,GUN_ID,%s,,,%02X,%02X,,%s,%s\n",
             (unsigned long)nowMs,
             alpStateName(currentState),
             byte0, byte1or2,
             alpGunName(gun),
             isObserveMode ? "observe" : "jam");
    appendLine(line);
}

void AlpSdLogger::logFrame(uint32_t nowMs, const char* frameType,
                            uint8_t b0, uint8_t b1, uint8_t b2, uint8_t cs,
                            AlpState currentState) {
    if (!enabled_ || !sdReady_) return;

    char line[128];
    snprintf(line, sizeof(line), "%lu,%s,%s,,,%02X,%02X,%02X,,\n",
             (unsigned long)nowMs,
             frameType,
             alpStateName(currentState),
             b0, b1, b2);
    appendLine(line);
}

void AlpSdLogger::logEvent(uint32_t nowMs, const char* event, AlpState currentState,
                            uint32_t extraValue) {
    if (!enabled_ || !sdReady_) return;

    char line[128];
    snprintf(line, sizeof(line), "%lu,%s,%s,,,,,,,%lu\n",
             (unsigned long)nowMs,
             event,
             alpStateName(currentState),
             (unsigned long)extraValue);
    appendLine(line);
}

// ── Internal helpers ─────────────────────────────────────────────────

bool AlpSdLogger::ensureDirectory() {
#ifndef UNIT_TEST
    if (dirReady_) return true;

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return false;

    // Try to create /alp directory
    if (!fs->exists(ALP_DIR_PATH)) {
        if (!fs->mkdir(ALP_DIR_PATH)) {
            return false;
        }
    }
    dirReady_ = true;
    return true;
#else
    dirReady_ = true;
    return true;
#endif
}

bool AlpSdLogger::ensureHeader() {
#ifndef UNIT_TEST
    if (headerWritten_) return true;
    if (csvPathBuf_[0] == '\0') return false;

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return false;

    // Check if file already exists and has content
    if (fs->exists(csvPathBuf_)) {
        File f = fs->open(csvPathBuf_, FILE_READ);
        if (f && f.size() > 0) {
            f.close();
            headerWritten_ = true;
            return true;
        }
        if (f) f.close();
    }

    // Write header to new file
    File f = fs->open(csvPathBuf_, FILE_APPEND, true);
    if (!f) return false;
    f.print(ALP_CSV_HEADER);
    f.close();
    headerWritten_ = true;
    return true;
#else
    headerWritten_ = true;
    return true;
#endif
}

bool AlpSdLogger::appendLine(const char* line) {
#ifndef UNIT_TEST
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        dropCount_++;
        return false;
    }

    // Non-blocking lock — main loop must never stall for SD
    StorageManager::SDTryLock lock(storageManager.getSDMutex());
    if (!lock) {
        dropCount_++;
        return false;
    }

    if (!ensureDirectory() || !ensureHeader()) {
        dropCount_++;
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        dropCount_++;
        return false;
    }

    File f = fs->open(csvPathBuf_, FILE_APPEND);
    if (!f) {
        dropCount_++;
        return false;
    }

    f.print(line);
    f.close();
    linesWritten_++;
    return true;
#else
    linesWritten_++;
    return true;
#endif
}

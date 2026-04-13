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

namespace {

void formatHexByte(char* dest, size_t destSize, uint8_t value) {
    snprintf(dest, destSize, "%02X", value);
}

void formatCsvRow(char* dest, size_t destSize,
                  uint32_t nowMs,
                  const char* event,
                  const char* fromState,
                  const char* toState,
                  const char* byte0,
                  const char* byte1,
                  const char* byte2,
                  const char* checksum,
                  const char* gun,
                  const char* extra) {
    snprintf(dest, destSize, "%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
             static_cast<unsigned long>(nowMs),
             event ? event : "",
             fromState ? fromState : "",
             toState ? toState : "",
             byte0 ? byte0 : "",
             byte1 ? byte1 : "",
             byte2 ? byte2 : "",
             checksum ? checksum : "",
             gun ? gun : "",
             extra ? extra : "");
}

}  // namespace

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
    formatCsvRow(line, sizeof(line),
                 nowMs,
                 "STATE",
                 alpStateName(from),
                 alpStateName(to),
                 "", "", "", "", "", "");
    appendLine(line);
}

void AlpSdLogger::logHeartbeatByte1(uint32_t nowMs, uint8_t prevByte1, uint8_t newByte1,
                                     AlpState currentState) {
    if (!enabled_ || !sdReady_) return;

    char prevByteBuf[3];
    char newByteBuf[3];
    char line[128];
    formatHexByte(prevByteBuf, sizeof(prevByteBuf), prevByte1);
    formatHexByte(newByteBuf, sizeof(newByteBuf), newByte1);
    formatCsvRow(line, sizeof(line),
                 nowMs,
                 "HB_BYTE1",
                 alpStateName(currentState),
                 "",
                 prevByteBuf,
                 newByteBuf,
                 "",
                 "",
                 "",
                 newByte1 == 0x01 ? "ALERT" : "IDLE");
    appendLine(line);
}

void AlpSdLogger::logGunIdentified(uint32_t nowMs, AlpGunType gun, uint8_t byte0,
                                    uint8_t byte1or2, bool isObserveMode,
                                    AlpState currentState) {
    if (!enabled_ || !sdReady_) return;

    const uint8_t frameByte1 = isObserveMode ? byte1or2 : 0x00;
    const uint8_t frameByte2 = isObserveMode ? 0x00 : byte1or2;
    char byte0Buf[3];
    char byte1Buf[3];
    char byte2Buf[3];
    char checksumBuf[3];
    char line[160];
    formatHexByte(byte0Buf, sizeof(byte0Buf), byte0);
    formatHexByte(byte1Buf, sizeof(byte1Buf), frameByte1);
    formatHexByte(byte2Buf, sizeof(byte2Buf), frameByte2);
    formatHexByte(checksumBuf, sizeof(checksumBuf), alpChecksum(byte0, frameByte1, frameByte2));
    formatCsvRow(line, sizeof(line),
                 nowMs,
                 "GUN_ID",
                 alpStateName(currentState),
                 "",
                 byte0Buf,
                 byte1Buf,
                 byte2Buf,
                 checksumBuf,
                 alpGunName(gun),
                 isObserveMode ? "observe" : "jam");
    appendLine(line);
}

void AlpSdLogger::logFrame(uint32_t nowMs, const char* frameType,
                            uint8_t b0, uint8_t b1, uint8_t b2, uint8_t cs,
                            AlpState currentState) {
    if (!enabled_ || !sdReady_) return;

    char byte0Buf[3];
    char byte1Buf[3];
    char byte2Buf[3];
    char checksumBuf[3];
    char line[128];
    formatHexByte(byte0Buf, sizeof(byte0Buf), b0);
    formatHexByte(byte1Buf, sizeof(byte1Buf), b1);
    formatHexByte(byte2Buf, sizeof(byte2Buf), b2);
    formatHexByte(checksumBuf, sizeof(checksumBuf), cs);
    formatCsvRow(line, sizeof(line),
                 nowMs,
                 frameType,
                 alpStateName(currentState),
                 "",
                 byte0Buf,
                 byte1Buf,
                 byte2Buf,
                 checksumBuf,
                 "",
                 "");
    appendLine(line);
}

void AlpSdLogger::logHeartbeat(uint32_t nowMs, uint8_t b0, uint8_t b1, uint8_t b2,
                                AlpState currentState) {
    if (!enabled_ || !sdReady_) return;

    // Rate-limit if configured (0 = log every heartbeat)
    if (HEARTBEAT_LOG_INTERVAL_MS > 0 &&
        (nowMs - lastHeartbeatLogMs_) < HEARTBEAT_LOG_INTERVAL_MS) {
        return;
    }
    lastHeartbeatLogMs_ = nowMs;

    char byte0Buf[3];
    char byte1Buf[3];
    char byte2Buf[3];
    char checksumBuf[3];
    char line[128];
    formatHexByte(byte0Buf, sizeof(byte0Buf), b0);
    formatHexByte(byte1Buf, sizeof(byte1Buf), b1);
    formatHexByte(byte2Buf, sizeof(byte2Buf), b2);
    formatHexByte(checksumBuf, sizeof(checksumBuf), alpChecksum(b0, b1, b2));
    formatCsvRow(line, sizeof(line),
                 nowMs,
                 "HEARTBEAT",
                 alpStateName(currentState),
                 "",
                 byte0Buf,
                 byte1Buf,
                 byte2Buf,
                 checksumBuf,
                 "",
                 "");
    appendLine(line);
}

void AlpSdLogger::logEvent(uint32_t nowMs, const char* event, AlpState currentState,
                            uint32_t extraValue) {
    if (!enabled_ || !sdReady_) return;

    char extraBuf[16];
    char line[128];
    snprintf(extraBuf, sizeof(extraBuf), "%lu", static_cast<unsigned long>(extraValue));
    formatCsvRow(line, sizeof(line),
                 nowMs,
                 event,
                 alpStateName(currentState),
                 "",
                 "", "", "", "", "", extraBuf);
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
    snprintf(lastLineBuf_, sizeof(lastLineBuf_), "%s", line);
    linesWritten_++;
    return true;
#endif
}

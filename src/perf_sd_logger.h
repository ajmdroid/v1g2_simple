/**
 * Standalone SD-backed performance CSV logger.
 *
 * Writes compact perf snapshots to /perf/YYYYMMDD_HHMMSS_perf_<bootId>.csv
 * (UTC timestamp) when epoch time is available. Falls back to
 * /perf/perf_boot_<bootId>.csv when time is not yet valid.
 * Uses a dedicated FreeRTOS writer task; enqueue is non-blocking and drops on queue full.
 */

#pragma once
#ifndef PERF_SD_LOGGER_H
#define PERF_SD_LOGGER_H

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "psram_freertos_alloc.h"

struct PerfSdSnapshot;

class PerfSdLogger {
public:
    void begin(bool sdAvailable);
    void setBootId(uint32_t id);
    bool enqueue(const PerfSdSnapshot& snapshot);
    bool isEnabled() const { return enabled; }
    const char* csvPath() const { return csvPathBuf; }

    /// Start a new logical session within the current boot file.
    /// Emits a fresh CSV header + #session_start marker so scoring tools
    /// can isolate V1-connected data from idle boot noise.
    void startNewSession();

private:
    static void writerTaskEntry(void* param);
    void writerTaskLoop();
    bool receiveSnapshot(PerfSdSnapshot& snapshot, TickType_t timeoutTicks);
    bool ensurePerfDir(fs::FS& fs);
    bool ensureCsvHeaderAndSessionMarker(File& f);
    bool writeSessionMarker(File& f);
    bool appendSnapshotLine(const PerfSdSnapshot& snapshot);

    bool enabled = false;
    QueueHandle_t queue = nullptr;
    TaskHandle_t writerTask = nullptr;
    PsramQueueAllocation queueAllocation = {};
    bool queueInPsram = false;
    bool writerTaskStackInPsram = false;
    bool perfDirReady = false;
    bool csvHeaderReady = false;
    bool sessionMarkerPending = false;
    uint32_t sessionSeq = 0;
    uint32_t sessionToken = 0;
    uint32_t sessionStartMs = 0;
    uint32_t bootId = 0;
    char csvPathBuf[64] = {0};

#ifdef UNIT_TEST
public:
    bool receiveSnapshotForTest(PerfSdSnapshot& snapshot, TickType_t timeoutTicks) {
        return receiveSnapshot(snapshot, timeoutTicks);
    }
#endif
};

extern PerfSdLogger perfSdLogger;

#endif  // PERF_SD_LOGGER_H

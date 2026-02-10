/**
 * Standalone SD-backed performance CSV logger.
 *
 * Writes compact perf snapshots to /perf/perf.csv using a dedicated
 * FreeRTOS writer task. Enqueue is non-blocking and drops on queue full.
 */

#ifndef PERF_SD_LOGGER_H
#define PERF_SD_LOGGER_H

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

struct PerfSdSnapshot {
    uint32_t millisTs;
    uint32_t rx;
    uint32_t qDrop;
    uint32_t parseOk;
    uint32_t parseFail;
    uint32_t disc;
    uint32_t reconn;
    uint32_t loopMaxUs;
    uint32_t bleDrainMaxUs;
    uint32_t dispMaxUs;
    uint32_t freeHeap;
    uint32_t freeDma;         // Cached internal 8-bit heap (legacy column)
    uint32_t largestDma;      // Cached largest internal 8-bit block (legacy column)
    uint32_t freeDmaCap;      // True MALLOC_CAP_DMA free bytes
    uint32_t largestDmaCap;   // True MALLOC_CAP_DMA largest free block
};

class PerfSdLogger {
public:
    void begin(bool sdAvailable);
    bool enqueue(const PerfSdSnapshot& snapshot);
    bool isEnabled() const { return enabled; }

private:
    static void writerTaskEntry(void* param);
    void writerTaskLoop();
    bool ensurePerfDir(fs::FS& fs);
    bool ensureCsvHeaderAndSessionMarker(File& f);
    bool writeSessionMarker(File& f);
    bool appendSnapshotLine(const PerfSdSnapshot& snapshot);

    bool enabled = false;
    QueueHandle_t queue = nullptr;
    TaskHandle_t writerTask = nullptr;
    bool perfDirReady = false;
    bool csvHeaderReady = false;
    bool sessionMarkerPending = false;
    uint32_t sessionSeq = 0;
    uint32_t sessionToken = 0;
    uint32_t sessionStartMs = 0;
};

extern PerfSdLogger perfSdLogger;

#endif  // PERF_SD_LOGGER_H

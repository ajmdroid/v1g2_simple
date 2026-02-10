/**
 * Standalone SD-backed performance CSV logger implementation.
 */

#include "perf_sd_logger.h"

#include "storage_manager.h"
#include "perf_metrics.h"
#include <FS.h>
#include <cstring>

namespace {
static constexpr const char* PERF_DIR_PATH = "/perf";
static constexpr const char* PERF_CSV_PATH = "/perf/perf.csv";
static constexpr const char* PERF_CSV_HEADER =
    "millis,rx,qDrop,parseOK,parseFail,disc,reconn,loopMax_us,bleDrainMax_us,dispMax_us,freeHeap\n";

static constexpr UBaseType_t PERF_SD_QUEUE_DEPTH = 32;
static constexpr uint32_t PERF_SD_WRITER_STACK_SIZE = 3072;
static constexpr UBaseType_t PERF_SD_WRITER_PRIORITY = 1;
}  // namespace

PerfSdLogger perfSdLogger;

void PerfSdLogger::begin(bool sdAvailable) {
    enabled = false;
    if (!sdAvailable) {
        return;
    }

    if (!queue) {
        queue = xQueueCreate(PERF_SD_QUEUE_DEPTH, sizeof(PerfSdSnapshot));
        if (!queue) {
            Serial.println("[Perf] ERROR: Failed to create SD logger queue");
            return;
        }
    }

    if (!writerTask) {
        BaseType_t rc = xTaskCreatePinnedToCore(
            writerTaskEntry,
            "PerfSdWriter",
            PERF_SD_WRITER_STACK_SIZE,
            this,
            PERF_SD_WRITER_PRIORITY,
            &writerTask,
            0);
        if (rc != pdPASS) {
            Serial.println("[Perf] ERROR: Failed to create SD logger task");
            return;
        }
    }

    enabled = true;
}

bool PerfSdLogger::enqueue(const PerfSdSnapshot& snapshot) {
    if (!enabled || !queue) {
        return false;
    }
    if (xQueueSend(queue, &snapshot, 0) != pdTRUE) {
        PERF_INC(perfDrop);
        return false;
    }
    return true;
}

void PerfSdLogger::writerTaskEntry(void* param) {
    PerfSdLogger* self = static_cast<PerfSdLogger*>(param);
    self->writerTaskLoop();
}

void PerfSdLogger::writerTaskLoop() {
    while (true) {
        PerfSdSnapshot snapshot{};
        if (xQueueReceive(queue, &snapshot, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        appendSnapshotLine(snapshot);
        taskYIELD();
    }
}

bool PerfSdLogger::appendSnapshotLine(const PerfSdSnapshot& snapshot) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        return false;
    }

    if (!fs->exists(PERF_DIR_PATH)) {
        fs->mkdir(PERF_DIR_PATH);
    }

    bool fileExists = fs->exists(PERF_CSV_PATH);
    File f = fs->open(PERF_CSV_PATH, FILE_APPEND, true);
    if (!f) {
        return false;
    }

    if (!fileExists || f.size() == 0) {
        f.write(reinterpret_cast<const uint8_t*>(PERF_CSV_HEADER), strlen(PERF_CSV_HEADER));
    }

    char line[192];
    int n = snprintf(
        line,
        sizeof(line),
        "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
        static_cast<unsigned long>(snapshot.millisTs),
        static_cast<unsigned long>(snapshot.rx),
        static_cast<unsigned long>(snapshot.qDrop),
        static_cast<unsigned long>(snapshot.parseOk),
        static_cast<unsigned long>(snapshot.parseFail),
        static_cast<unsigned long>(snapshot.disc),
        static_cast<unsigned long>(snapshot.reconn),
        static_cast<unsigned long>(snapshot.loopMaxUs),
        static_cast<unsigned long>(snapshot.bleDrainMaxUs),
        static_cast<unsigned long>(snapshot.dispMaxUs),
        static_cast<unsigned long>(snapshot.freeHeap));

    if (n > 0) {
        f.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
    }

    f.close();
    return n > 0;
}

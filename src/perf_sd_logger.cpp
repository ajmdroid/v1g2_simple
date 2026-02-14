/**
 * Standalone SD-backed performance CSV logger implementation.
 */

#include "perf_sd_logger.h"

#include "storage_manager.h"
#include "perf_metrics.h"
#include <FS.h>
#include <cstring>
#include <esp_system.h>

namespace {
static constexpr const char* PERF_DIR_PATH = "/perf";
static constexpr const char* PERF_CSV_PATH_FALLBACK = "/perf/perf.csv";
static constexpr uint32_t PERF_CSV_SCHEMA_VERSION = 4;
static constexpr const char* PERF_CSV_HEADER =
    "millis,timeValid,timeSource,rx,qDrop,parseOK,parseFail,disc,reconn,loopMax_us,bleDrainMax_us,dispMax_us,freeHeap,freeDma,largestDma,freeDmaCap,largestDmaCap,dmaFreeMin,dmaLargestMin,bleProcessMax_us,touchMax_us,wifiMax_us,uiToScan,uiToRest,uiScanToRest,uiFastScanExit,uiLastScanDwellMs,uiMinScanDwellMs,fadeDown,fadeRestore,fadeSkipEqual,fadeSkipNoBaseline,fadeSkipNotFaded,fadeLastDecision,fadeLastCurrentVol,fadeLastOriginalVol,fadeLastDecisionMs,bleScanStartMs,bleTargetFoundMs,bleConnectStartMs,bleConnectedMs,bleFirstRxMs,obdState,obdConnected,obdScanActive,obdHasValidData,obdSampleAgeMs,obdSpeedMph_x10,obdConnFailures,obdPollFailStreak,obdNotifyDrops,alertPersistStarts,alertPersistExpires,alertPersistClears,autoPushStarts,autoPushCompletes,autoPushNoProfile,autoPushProfileLoadFail,autoPushProfileWriteFail,autoPushBusyRetries,autoPushModeFail,autoPushVolumeFail,autoPushDisconnectAbort,speedVolBoosts,speedVolRestores,speedVolFadeTakeovers,speedVolNoHeadroom,voiceAnnouncePriority,voiceAnnounceDirection,voiceAnnounceSecondary,voiceAnnounceEscalation,voiceDirectionThrottled,powerAutoPowerArmed,powerAutoPowerTimerStart,powerAutoPowerTimerCancel,powerAutoPowerTimerExpire,powerCriticalWarn,powerCriticalShutdown,cmdBleBusy,gpsEnabled,gpsHasFix,gpsLocationValid,gpsSatellites,gpsParserActive,gpsModuleDetected,gpsDetectionTimedOut,gpsSpeedMph_x10,gpsHdop_x10,gpsSampleAgeMs,gpsObsDrops,gpsObsSize,gpsObsPublished,cameraEnabled,cameraIndexLoaded,cameraLastCapReached,cameraLoaderInProgress,cameraTicks,cameraTickSkipsOverload,cameraTickSkipsNonCore,cameraTickSkipsMemGuard,cameraCandidatesChecked,cameraMatches,cameraAlertsStarted,cameraBudgetExceeded,cameraLoadFailures,cameraLoadSkipsMemGuard,cameraIndexSwapCount,cameraIndexSwapFailures,cameraLastTick_us,cameraMaxTick_us,cameraLastLoadMs,cameraMaxLoadMs,cameraLastSortMs,cameraLastSpanMs,cameraLastInternalFree,cameraLastInternalBlock,cameraLoaderReadyVersion\n";

static constexpr UBaseType_t PERF_SD_QUEUE_DEPTH = 32;
static constexpr uint32_t PERF_SD_WRITER_STACK_SIZE = 8192;  // SD file ops need generous stack
static constexpr UBaseType_t PERF_SD_WRITER_PRIORITY = 1;

static uint16_t countCsvColumns(const char* text, size_t len) {
    if (!text || len == 0) {
        return 0;
    }
    uint16_t columns = 1;
    bool sawContent = false;
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '\0' || c == '\n' || c == '\r') {
            break;
        }
        sawContent = true;
        if (c == ',') {
            columns++;
        }
    }
    return sawContent ? columns : 0;
}

static uint16_t expectedPerfCsvColumns() {
    static const uint16_t kColumns = countCsvColumns(PERF_CSV_HEADER, strlen(PERF_CSV_HEADER));
    return kColumns;
}

static void buildPerfCsvPath(uint32_t bootId, char* out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    if (bootId == 0) {
        snprintf(out, outLen, "%s", PERF_CSV_PATH_FALLBACK);
        return;
    }
    snprintf(out, outLen, "/perf/perf_boot_%lu.csv", static_cast<unsigned long>(bootId));
}
}  // namespace

PerfSdLogger perfSdLogger;

void PerfSdLogger::setBootId(uint32_t id) {
    bootId = id;
    buildPerfCsvPath(bootId, csvPathBuf, sizeof(csvPathBuf));
    csvHeaderReady = false;
    sessionMarkerPending = true;
}

void PerfSdLogger::begin(bool sdAvailable) {
    enabled = false;
    if (!sdAvailable) {
        return;
    }

    if (csvPathBuf[0] == '\0') {
        setBootId(bootId);
    }

    // Reset cached file state for each runtime session and emit a marker on first write.
    perfDirReady = false;
    csvHeaderReady = false;
    sessionMarkerPending = true;
    sessionStartMs = millis();
    sessionToken = static_cast<uint32_t>(esp_random());
    sessionSeq++;

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

bool PerfSdLogger::ensurePerfDir(fs::FS& fs) {
    if (perfDirReady) {
        return true;
    }
    if (fs.mkdir(PERF_DIR_PATH) || fs.exists(PERF_DIR_PATH)) {
        perfDirReady = true;
        return true;
    }
    PERF_INC(perfSdDirFail);
    return false;
}

bool PerfSdLogger::writeSessionMarker(File& f) {
    char marker[128];
    int n = snprintf(
        marker,
        sizeof(marker),
        "#session_start,seq=%lu,bootId=%lu,uptime_ms=%lu,token=%08lX,schema=%lu\n",
        static_cast<unsigned long>(sessionSeq),
        static_cast<unsigned long>(bootId),
        static_cast<unsigned long>(sessionStartMs),
        static_cast<unsigned long>(sessionToken),
        static_cast<unsigned long>(PERF_CSV_SCHEMA_VERSION));
    if (n <= 0 || n >= static_cast<int>(sizeof(marker))) {
        return false;
    }
    size_t markerLen = static_cast<size_t>(n);
    size_t markerWritten = f.write(reinterpret_cast<const uint8_t*>(marker), markerLen);
    return markerWritten == markerLen;
}

bool PerfSdLogger::ensureCsvHeaderAndSessionMarker(File& f) {
    // If the file was rotated/deleted while running, size 0 means header must be rewritten.
    if (f.size() == 0) {
        csvHeaderReady = false;
    }

    if (!csvHeaderReady) {
        size_t headerLen = strlen(PERF_CSV_HEADER);
        size_t headerWritten = f.write(reinterpret_cast<const uint8_t*>(PERF_CSV_HEADER), headerLen);
        if (headerWritten != headerLen) {
            PERF_INC(perfSdHeaderFail);
            return false;
        }
        csvHeaderReady = true;
    }

    if (sessionMarkerPending) {
        if (!writeSessionMarker(f)) {
            PERF_INC(perfSdMarkerFail);
            return false;
        }
        sessionMarkerPending = false;
    }

    return true;
}

bool PerfSdLogger::appendSnapshotLine(const PerfSdSnapshot& snapshot) {
    uint32_t startUs = PERF_TIMESTAMP_US();

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        PERF_INC(perfSdLockFail);
        return false;
    }

    if (!ensurePerfDir(*fs)) {
        return false;
    }

    const char* csvPath = (csvPathBuf[0] != '\0') ? csvPathBuf : PERF_CSV_PATH_FALLBACK;
    File f = fs->open(csvPath, FILE_APPEND, true);
    if (!f && perfDirReady) {
        // Directory can be removed while running; invalidate cache and retry once.
        perfDirReady = false;
        if (ensurePerfDir(*fs)) {
            f = fs->open(csvPath, FILE_APPEND, true);
        }
    }
    if (!f) {
        PERF_INC(perfSdOpenFail);
        return false;
    }

    if (!ensureCsvHeaderAndSessionMarker(f)) {
        f.close();
        return false;
    }

    char line[1536];
    int n = snprintf(
        line,
        sizeof(line),
        "%lu,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%u,%u,%u,%u,%lu,%ld,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%u,%u,%u,%u,%u,%u,%ld,%u,%lu,%lu,%lu,%lu,%u,%u,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
        static_cast<unsigned long>(snapshot.millisTs),
        static_cast<unsigned int>(snapshot.timeValid),
        static_cast<unsigned int>(snapshot.timeSource),
        static_cast<unsigned long>(snapshot.rx),
        static_cast<unsigned long>(snapshot.qDrop),
        static_cast<unsigned long>(snapshot.parseOk),
        static_cast<unsigned long>(snapshot.parseFail),
        static_cast<unsigned long>(snapshot.disc),
        static_cast<unsigned long>(snapshot.reconn),
        static_cast<unsigned long>(snapshot.loopMaxUs),
        static_cast<unsigned long>(snapshot.bleDrainMaxUs),
        static_cast<unsigned long>(snapshot.dispMaxUs),
        static_cast<unsigned long>(snapshot.freeHeap),
        static_cast<unsigned long>(snapshot.freeDma),
        static_cast<unsigned long>(snapshot.largestDma),
        static_cast<unsigned long>(snapshot.freeDmaCap),
        static_cast<unsigned long>(snapshot.largestDmaCap),
        static_cast<unsigned long>(snapshot.dmaFreeMin),
        static_cast<unsigned long>(snapshot.dmaLargestMin),
        static_cast<unsigned long>(snapshot.bleProcessMaxUs),
        static_cast<unsigned long>(snapshot.touchMaxUs),
        static_cast<unsigned long>(snapshot.wifiMaxUs),
        static_cast<unsigned long>(snapshot.uiToScanCount),
        static_cast<unsigned long>(snapshot.uiToRestCount),
        static_cast<unsigned long>(snapshot.uiScanToRestCount),
        static_cast<unsigned long>(snapshot.uiFastScanExitCount),
        static_cast<unsigned long>(snapshot.uiLastScanDwellMs),
        static_cast<unsigned long>(snapshot.uiMinScanDwellMs),
        static_cast<unsigned long>(snapshot.fadeDownCount),
        static_cast<unsigned long>(snapshot.fadeRestoreCount),
        static_cast<unsigned long>(snapshot.fadeSkipEqualCount),
        static_cast<unsigned long>(snapshot.fadeSkipNoBaselineCount),
        static_cast<unsigned long>(snapshot.fadeSkipNotFadedCount),
        static_cast<unsigned int>(snapshot.fadeLastDecision),
        static_cast<unsigned int>(snapshot.fadeLastCurrentVol),
        static_cast<unsigned int>(snapshot.fadeLastOriginalVol),
        static_cast<unsigned long>(snapshot.fadeLastDecisionMs),
        static_cast<unsigned long>(snapshot.bleScanStartMs),
        static_cast<unsigned long>(snapshot.bleTargetFoundMs),
        static_cast<unsigned long>(snapshot.bleConnectStartMs),
        static_cast<unsigned long>(snapshot.bleConnectedMs),
        static_cast<unsigned long>(snapshot.bleFirstRxMs),
        static_cast<unsigned int>(snapshot.obdState),
        static_cast<unsigned int>(snapshot.obdConnected),
        static_cast<unsigned int>(snapshot.obdScanActive),
        static_cast<unsigned int>(snapshot.obdHasValidData),
        static_cast<unsigned long>(snapshot.obdSampleAgeMs),
        static_cast<long>(snapshot.obdSpeedMphX10),
        static_cast<unsigned int>(snapshot.obdConnFailures),
        static_cast<unsigned int>(snapshot.obdPollFailStreak),
        static_cast<unsigned long>(snapshot.obdNotifyDrops),
        static_cast<unsigned long>(snapshot.alertPersistStarts),
        static_cast<unsigned long>(snapshot.alertPersistExpires),
        static_cast<unsigned long>(snapshot.alertPersistClears),
        static_cast<unsigned long>(snapshot.autoPushStarts),
        static_cast<unsigned long>(snapshot.autoPushCompletes),
        static_cast<unsigned long>(snapshot.autoPushNoProfile),
        static_cast<unsigned long>(snapshot.autoPushProfileLoadFail),
        static_cast<unsigned long>(snapshot.autoPushProfileWriteFail),
        static_cast<unsigned long>(snapshot.autoPushBusyRetries),
        static_cast<unsigned long>(snapshot.autoPushModeFail),
        static_cast<unsigned long>(snapshot.autoPushVolumeFail),
        static_cast<unsigned long>(snapshot.autoPushDisconnectAbort),
        static_cast<unsigned long>(snapshot.speedVolBoosts),
        static_cast<unsigned long>(snapshot.speedVolRestores),
        static_cast<unsigned long>(snapshot.speedVolFadeTakeovers),
        static_cast<unsigned long>(snapshot.speedVolNoHeadroom),
        static_cast<unsigned long>(snapshot.voiceAnnouncePriority),
        static_cast<unsigned long>(snapshot.voiceAnnounceDirection),
        static_cast<unsigned long>(snapshot.voiceAnnounceSecondary),
        static_cast<unsigned long>(snapshot.voiceAnnounceEscalation),
        static_cast<unsigned long>(snapshot.voiceDirectionThrottled),
        static_cast<unsigned long>(snapshot.powerAutoPowerArmed),
        static_cast<unsigned long>(snapshot.powerAutoPowerTimerStart),
        static_cast<unsigned long>(snapshot.powerAutoPowerTimerCancel),
        static_cast<unsigned long>(snapshot.powerAutoPowerTimerExpire),
        static_cast<unsigned long>(snapshot.powerCriticalWarn),
        static_cast<unsigned long>(snapshot.powerCriticalShutdown),
        static_cast<unsigned long>(snapshot.cmdBleBusy),
        static_cast<unsigned int>(snapshot.gpsEnabled),
        static_cast<unsigned int>(snapshot.gpsHasFix),
        static_cast<unsigned int>(snapshot.gpsLocationValid),
        static_cast<unsigned int>(snapshot.gpsSatellites),
        static_cast<unsigned int>(snapshot.gpsParserActive),
        static_cast<unsigned int>(snapshot.gpsModuleDetected),
        static_cast<unsigned int>(snapshot.gpsDetectionTimedOut),
        static_cast<long>(snapshot.gpsSpeedMphX10),
        static_cast<unsigned int>(snapshot.gpsHdopX10),
        static_cast<unsigned long>(snapshot.gpsSampleAgeMs),
        static_cast<unsigned long>(snapshot.gpsObsDrops),
        static_cast<unsigned long>(snapshot.gpsObsSize),
        static_cast<unsigned long>(snapshot.gpsObsPublished),
        static_cast<unsigned int>(snapshot.cameraEnabled),
        static_cast<unsigned int>(snapshot.cameraIndexLoaded),
        static_cast<unsigned int>(snapshot.cameraLastCapReached),
        static_cast<unsigned int>(snapshot.cameraLoaderInProgress),
        static_cast<unsigned long>(snapshot.cameraTicks),
        static_cast<unsigned long>(snapshot.cameraTickSkipsOverload),
        static_cast<unsigned long>(snapshot.cameraTickSkipsNonCore),
        static_cast<unsigned long>(snapshot.cameraTickSkipsMemGuard),
        static_cast<unsigned long>(snapshot.cameraCandidatesChecked),
        static_cast<unsigned long>(snapshot.cameraMatches),
        static_cast<unsigned long>(snapshot.cameraAlertsStarted),
        static_cast<unsigned long>(snapshot.cameraBudgetExceeded),
        static_cast<unsigned long>(snapshot.cameraLoadFailures),
        static_cast<unsigned long>(snapshot.cameraLoadSkipsMemGuard),
        static_cast<unsigned long>(snapshot.cameraIndexSwapCount),
        static_cast<unsigned long>(snapshot.cameraIndexSwapFailures),
        static_cast<unsigned long>(snapshot.cameraLastTickUs),
        static_cast<unsigned long>(snapshot.cameraMaxTickUs),
        static_cast<unsigned long>(snapshot.cameraLastLoadMs),
        static_cast<unsigned long>(snapshot.cameraMaxLoadMs),
        static_cast<unsigned long>(snapshot.cameraLastSortMs),
        static_cast<unsigned long>(snapshot.cameraLastSpanMs),
        static_cast<unsigned long>(snapshot.cameraLastInternalFree),
        static_cast<unsigned long>(snapshot.cameraLastInternalBlock),
        static_cast<unsigned long>(snapshot.cameraLoaderReadyVersion));

    if (n <= 0 || n >= static_cast<int>(sizeof(line))) {
        f.close();
        return false;
    }
    size_t lineLen = static_cast<size_t>(n);
    const uint16_t expectedColumns = expectedPerfCsvColumns();
    const uint16_t lineColumns = countCsvColumns(line, lineLen);
    if (expectedColumns == 0 || lineColumns != expectedColumns) {
        PERF_INC(perfSdWriteFail);
        f.close();
        return false;
    }

    size_t lineWritten = f.write(reinterpret_cast<const uint8_t*>(line), lineLen);
    if (lineWritten != lineLen) {
        PERF_INC(perfSdWriteFail);
        f.close();
        return false;
    }

    f.close();
    perfRecordSdFlushUs(PERF_TIMESTAMP_US() - startUs);
    return true;
}

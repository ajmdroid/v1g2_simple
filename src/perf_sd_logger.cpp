/**
 * Standalone SD-backed performance CSV logger implementation.
 */

#include "perf_sd_logger.h"

#include "storage_manager.h"
#include "perf_metrics.h"
#include <FS.h>
#include <cstdarg>
#include <cstring>
#include <esp_system.h>

namespace {
static constexpr const char* PERF_DIR_PATH = "/perf";
static constexpr const char* PERF_CSV_PATH_FALLBACK = "/perf/perf.csv";
static constexpr uint32_t PERF_CSV_SCHEMA_VERSION = 27;
static constexpr const char* PERF_CSV_HEADER =
    "millis,rx,qDrop,parseOK,parseFail,disc,reconn,loopMax_us,bleDrainMax_us,dispMax_us,freeHeap,freeDma,largestDma,freeDmaCap,largestDmaCap,dmaFreeMin,dmaLargestMin,bleProcessMax_us,touchMax_us,wifiMax_us,uiToScan,uiToRest,uiScanToRest,uiFastScanExit,uiLastScanDwellMs,uiMinScanDwellMs,fadeDown,fadeRestore,fadeSkipEqual,fadeSkipNoBaseline,fadeSkipNotFaded,fadeLastDecision,fadeLastCurrentVol,fadeLastOriginalVol,fadeLastDecisionMs,speedVolDrop,speedVolRestore,speedVolRetry,bleScanStartMs,bleTargetFoundMs,bleConnectStartMs,bleConnectedMs,bleFirstRxMs,bleFollowupRequestAlertMax_us,bleFollowupRequestVersionMax_us,bleConnectStableCallbackMax_us,bleProxyStartMax_us,displayVoiceMax_us,displayGapRecoverMax_us,displayFullRenderCount,displayIncrementalRenderCount,displayCardsOnlyRenderCount,displayRestingFullRenderCount,displayRestingIncrementalRenderCount,displayPersistedRenderCount,displayPreviewRenderCount,displayRestoreRenderCount,displayLiveScenarioRenderCount,displayRestingScenarioRenderCount,displayPersistedScenarioRenderCount,displayPreviewScenarioRenderCount,displayRestoreScenarioRenderCount,displayRedrawReasonFirstRunCount,displayRedrawReasonEnterLiveCount,displayRedrawReasonLeaveLiveCount,displayRedrawReasonLeavePersistedCount,displayRedrawReasonForceRedrawCount,displayRedrawReasonFrequencyChangeCount,displayRedrawReasonBandSetChangeCount,displayRedrawReasonArrowChangeCount,displayRedrawReasonSignalBarChangeCount,displayRedrawReasonVolumeChangeCount,displayRedrawReasonBogeyCounterChangeCount,displayRedrawReasonRssiRefreshCount,displayRedrawReasonFlashTickCount,displayFullFlushCount,displayPartialFlushCount,displayFlushBatchCount,displayPartialFlushAreaPeakPx,displayPartialFlushAreaTotalPx,displayFlushEquivalentAreaTotalPx,displayFlushMaxAreaPx,displayBaseFrameMax_us,displayStatusStripMax_us,displayFrequencyMax_us,displayBandsBarsMax_us,displayArrowsIconsMax_us,displayCardsMax_us,displayFlushSubphaseMax_us,displayLiveRenderMax_us,displayRestingRenderMax_us,displayPersistedRenderMax_us,displayPreviewRenderMax_us,displayRestoreRenderMax_us,displayPreviewFirstRenderMax_us,displayPreviewSteadyRenderMax_us,alertPersistStarts,alertPersistExpires,alertPersistClears,autoPushStarts,autoPushCompletes,autoPushNoProfile,autoPushProfileLoadFail,autoPushProfileWriteFail,autoPushBusyRetries,autoPushModeFail,autoPushVolumeFail,autoPushDisconnectAbort,voiceAnnouncePriority,voiceAnnounceDirection,voiceAnnounceSecondary,voiceAnnounceEscalation,voiceDirectionThrottled,powerAutoPowerArmed,powerAutoPowerTimerStart,powerAutoPowerTimerCancel,powerAutoPowerTimerExpire,powerCriticalWarn,powerCriticalShutdown,cmdBleBusy,rxBytes,oversizeDrops,queueHighWater,bleMutexSkip,bleMutexTimeout,cmdPaceNotYet,bleDiscTaskCreateFail,displayUpdates,displaySkips,wifiConnectDeferred,pushNowRetries,pushNowFailures,audioPlayCount,audioPlayBusy,audioTaskFail,minLargestBlock,fsMax_us,sdMax_us,sdWriteCount,sdWriteLt1ms,sdWrite1to5ms,sdWrite5to10ms,sdWriteGe10ms,flushMax_us,bleConnectMax_us,bleDiscoveryMax_us,bleSubscribeMax_us,dispPipeMax_us,perfReportMax_us,prioritySelectDisplayIndex,prioritySelectRowFlag,prioritySelectFirstUsable,prioritySelectFirstEntry,prioritySelectAmbiguousIndex,prioritySelectUnusableIndex,prioritySelectInvalidChosen,alertTablePublishes,alertTablePublishes3Bogey,alertTableRowReplacements,alertTableAssemblyTimeouts,parserRowsBandNone,parserRowsKuRaw,displayLiveInvalidPrioritySkips,displayLiveFallbackToUsable,obdMax_us,obdConnectCallMax_us,obdSecurityStartCallMax_us,obdDiscoveryCallMax_us,obdSubscribeCallMax_us,obdWriteCallMax_us,obdRssiCallMax_us,obdPollErrors,obdStaleCount,perfDrop,eventBusDrops,wifiHandleClientMax_us,wifiMaintenanceMax_us,wifiStatusCheckMax_us,wifiTimeoutCheckMax_us,wifiHeapGuardMax_us,wifiApStaPollMax_us,wifiStopHttpServerMax_us,wifiStopStaDisconnectMax_us,wifiStopApDisableMax_us,wifiStopModeOffMax_us,wifiStartPreflightMax_us,wifiStartApBringupMax_us,freeDmaMin,largestDmaMin,bleState,subscribeStep,connectInProgress,asyncConnectPending,pendingDisconnectCleanup,proxyAdvertising,proxyAdvertisingLastTransitionReason,wifiPriorityMode,speedSourceSelected,speedSourceValid,speedSelectedMph_x10,speedSelectedAgeMs,speedSourceSwitches,speedNoSourceSelections\n";

static constexpr UBaseType_t PERF_SD_QUEUE_DEPTH = 16;       // Halved from 32 to reclaim ~7 KiB internal SRAM
static constexpr uint32_t PERF_SD_WRITER_STACK_SIZE = 12288; // CSV assembly + FS ops need extra headroom
static constexpr UBaseType_t PERF_SD_WRITER_PRIORITY = 1;
static constexpr TickType_t PERF_SD_QUEUE_RECEIVE_TIMEOUT_TICKS = pdMS_TO_TICKS(1000);

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

static void buildPerfCsvPath(uint32_t bootId_, char* out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    if (bootId_ == 0) {
        snprintf(out, outLen, "%s", PERF_CSV_PATH_FALLBACK);
        return;
    }

    snprintf(out, outLen, "/perf/perf_boot_%lu.csv", static_cast<unsigned long>(bootId_));
}

static bool appendCsvFormat(char* buffer, size_t bufferLen, size_t& offset, const char* fmt, ...) {
    if (!buffer || offset >= bufferLen) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(buffer + offset, bufferLen - offset, fmt, args);
    va_end(args);

    if (written <= 0 || static_cast<size_t>(written) >= (bufferLen - offset)) {
        return false;
    }

    offset += static_cast<size_t>(written);
    return true;
}

static bool appendCsvUInt32(char* buffer, size_t bufferLen, size_t& offset, uint32_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%lu,", static_cast<unsigned long>(value));
}

static bool appendCsvUInt8(char* buffer, size_t bufferLen, size_t& offset, uint8_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%u,", static_cast<unsigned int>(value));
}

static bool appendCsvInt16(char* buffer, size_t bufferLen, size_t& offset, int16_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%d,", static_cast<int>(value));
}

static bool appendCsvUInt8Last(char* buffer, size_t bufferLen, size_t& offset, uint8_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%u\n", static_cast<unsigned int>(value));
}

static bool appendCsvUInt32Last(char* buffer, size_t bufferLen, size_t& offset, uint32_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%lu\n", static_cast<unsigned long>(value));
}
}  // namespace

PerfSdLogger perfSdLogger;

void PerfSdLogger::setBootId(uint32_t id) {
    bootId_ = id;
    buildPerfCsvPath(bootId_, csvPathBuf_, sizeof(csvPathBuf_));
    csvHeaderReady_ = false;
    sessionMarkerPending_ = true;
}

void PerfSdLogger::begin(bool sdAvailable) {
    enabled_ = false;
    if (!sdAvailable) {
        return;
    }

    if (csvPathBuf_[0] == '\0') {
        setBootId(bootId_);
    }

    // Reset cached file state for each runtime session and emit a marker on first write.
    perfDirReady_ = false;
    csvHeaderReady_ = false;
    sessionMarkerPending_ = true;
    sessionStartMs_ = millis();
    sessionToken_ = static_cast<uint32_t>(esp_random());
    sessionSeq_++;

    if (!queue_) {
        queue_ = createQueuePreferPsram(PERF_SD_QUEUE_DEPTH,
                                       sizeof(PerfSdSnapshot),
                                       queueAllocation_,
                                       &queueInPsram_);
        if (!queue_) {
            Serial.println("[Perf] ERROR: Failed to create SD logger queue");
            return;
        }
        if (!queueInPsram_) {
            Serial.println("[Perf] WARN: SD logger queue using internal SRAM fallback");
        }
    }

    if (!writerTask_) {
        BaseType_t rc = createTaskPinnedToCorePreferPsram(writerTaskEntry,
                                                          "PerfSdWriter",
                                                          PERF_SD_WRITER_STACK_SIZE,
                                                          this,
                                                          PERF_SD_WRITER_PRIORITY,
                                                          &writerTask_,
                                                          0,
                                                          &writerTaskStackInPsram_);
        if (rc != pdPASS) {
            Serial.println("[Perf] ERROR: Failed to create SD logger task");
            return;
        }
        if (!writerTaskStackInPsram_) {
            Serial.println("[Perf] WARN: SD logger task stack using internal SRAM fallback");
        }
    }

    enabled_ = true;
}

bool PerfSdLogger::enqueue(const PerfSdSnapshot& snapshot) {
    if (!enabled_ || !queue_) {
        return false;
    }
    if (xQueueSend(queue_, &snapshot, 0) != pdTRUE) {
        PERF_INC(perfDrop);
        return false;
    }
    return true;
}

void PerfSdLogger::startNewSession() {
    if (!enabled_) {
        return;
    }
    // Force next write to emit a fresh header + session marker.
    csvHeaderReady_ = false;
    sessionMarkerPending_ = true;
    sessionStartMs_ = millis();
    sessionToken_ = static_cast<uint32_t>(esp_random());
    sessionSeq_++;
}

void PerfSdLogger::writerTaskEntry(void* param) {
    PerfSdLogger* self = static_cast<PerfSdLogger*>(param);
    self->writerTaskLoop();
}

bool PerfSdLogger::receiveSnapshot(PerfSdSnapshot& snapshot, TickType_t timeoutTicks) {
    if (!queue_) {
        return false;
    }
    return xQueueReceive(queue_, &snapshot, timeoutTicks) == pdTRUE;
}

void PerfSdLogger::writerTaskLoop() {
    while (true) {
        PerfSdSnapshot snapshot{};
        if (!receiveSnapshot(snapshot, PERF_SD_QUEUE_RECEIVE_TIMEOUT_TICKS)) {
            continue;
        }
        appendSnapshotLine(snapshot);
        taskYIELD();
    }
}

bool PerfSdLogger::ensurePerfDir(fs::FS& fs) {
    if (perfDirReady_) {
        return true;
    }
    if (fs.mkdir(PERF_DIR_PATH) || fs.exists(PERF_DIR_PATH)) {
        perfDirReady_ = true;
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
        "#session_start,seq=%lu,bootId_=%lu,uptime_ms=%lu,token=%08lX,schema=%lu\n",
        static_cast<unsigned long>(sessionSeq_),
        static_cast<unsigned long>(bootId_),
        static_cast<unsigned long>(sessionStartMs_),
        static_cast<unsigned long>(sessionToken_),
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
        csvHeaderReady_ = false;
    }

    if (!csvHeaderReady_) {
        size_t headerLen = strlen(PERF_CSV_HEADER);
        size_t headerWritten = f.write(reinterpret_cast<const uint8_t*>(PERF_CSV_HEADER), headerLen);
        if (headerWritten != headerLen) {
            PERF_INC(perfSdHeaderFail);
            return false;
        }
        csvHeaderReady_ = true;
    }

    if (sessionMarkerPending_) {
        if (!writeSessionMarker(f)) {
            PERF_INC(perfSdMarkerFail);
            return false;
        }
        sessionMarkerPending_ = false;
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

    const char* csvPath = (csvPathBuf_[0] != '\0') ? csvPathBuf_ : PERF_CSV_PATH_FALLBACK;
    File f = fs->open(csvPath, FILE_APPEND, true);
    if (!f && perfDirReady_) {
        // Directory can be removed while running; invalidate cache and retry once.
        perfDirReady_ = false;
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

    // Single-consumer writer task; keep the large CSV row buffer off the task stack.
    static char line[6144];
    size_t offset = 0;
    const bool ok =
        appendCsvUInt32(line, sizeof(line), offset, snapshot.millisTs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.rx) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.qDrop) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.parseOk) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.parseFail) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.disc) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.reconn) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.loopMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleDrainMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.dispMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.freeHeap) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.freeDma) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.largestDma) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.freeDmaCap) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.largestDmaCap) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.dmaFreeMin) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.dmaLargestMin) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleProcessMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.touchMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.uiToScanCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.uiToRestCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.uiScanToRestCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.uiFastScanExitCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.uiLastScanDwellMs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.uiMinScanDwellMs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.fadeDownCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.fadeRestoreCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.fadeSkipEqualCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.fadeSkipNoBaselineCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.fadeSkipNotFadedCount) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.fadeLastDecision) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.fadeLastCurrentVol) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.fadeLastOriginalVol) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.fadeLastDecisionMs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.speedVolDropCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.speedVolRestoreCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.speedVolRetryCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleScanStartMs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleTargetFoundMs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleConnectStartMs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleConnectedMs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleFirstRxMs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleFollowupRequestAlertMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleFollowupRequestVersionMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleConnectStableCallbackMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleProxyStartMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayVoiceMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayGapRecoverMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayFullRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayIncrementalRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayCardsOnlyRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRestingFullRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRestingIncrementalRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPersistedRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPreviewRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRestoreRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayLiveScenarioRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRestingScenarioRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPersistedScenarioRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPreviewScenarioRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRestoreScenarioRenderCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonFirstRunCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonEnterLiveCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonLeaveLiveCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonLeavePersistedCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonForceRedrawCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonFrequencyChangeCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonBandSetChangeCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonArrowChangeCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonSignalBarChangeCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonVolumeChangeCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonBogeyCounterChangeCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonRssiRefreshCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRedrawReasonFlashTickCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayFullFlushCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPartialFlushCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayFlushBatchCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPartialFlushAreaPeakPx) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPartialFlushAreaTotalPx) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayFlushEquivalentAreaTotalPx) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayFlushMaxAreaPx) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayBaseFrameMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayStatusStripMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayFrequencyMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayBandsBarsMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayArrowsIconsMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayCardsMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayFlushSubphaseMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayLiveRenderMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRestingRenderMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPersistedRenderMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPreviewRenderMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayRestoreRenderMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPreviewFirstRenderMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayPreviewSteadyRenderMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.alertPersistStarts) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.alertPersistExpires) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.alertPersistClears) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.autoPushStarts) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.autoPushCompletes) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.autoPushNoProfile) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.autoPushProfileLoadFail) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.autoPushProfileWriteFail) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.autoPushBusyRetries) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.autoPushModeFail) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.autoPushVolumeFail) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.autoPushDisconnectAbort) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.voiceAnnouncePriority) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.voiceAnnounceDirection) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.voiceAnnounceSecondary) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.voiceAnnounceEscalation) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.voiceDirectionThrottled) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.powerAutoPowerArmed) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.powerAutoPowerTimerStart) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.powerAutoPowerTimerCancel) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.powerAutoPowerTimerExpire) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.powerCriticalWarn) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.powerCriticalShutdown) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.cmdBleBusy) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.rxBytes) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.oversizeDrops) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.queueHighWater) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleMutexSkip) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleMutexTimeout) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.cmdPaceNotYet) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleDiscTaskCreateFail) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayUpdates) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displaySkips) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiConnectDeferred) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.pushNowRetries) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.pushNowFailures) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.audioPlayCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.audioPlayBusy) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.audioTaskFail) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.minLargestBlock) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.fsMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.sdMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.sdWriteCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.sdWriteLt1msCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.sdWrite1to5msCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.sdWrite5to10msCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.sdWriteGe10msCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.flushMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleConnectMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleDiscoveryMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.bleSubscribeMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.dispPipeMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.perfReportMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.prioritySelectDisplayIndex) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.prioritySelectRowFlag) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.prioritySelectFirstUsable) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.prioritySelectFirstEntry) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.prioritySelectAmbiguousIndex) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.prioritySelectUnusableIndex) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.prioritySelectInvalidChosen) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.alertTablePublishes) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.alertTablePublishes3Bogey) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.alertTableRowReplacements) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.alertTableAssemblyTimeouts) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.parserRowsBandNone) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.parserRowsKuRaw) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayLiveInvalidPrioritySkips) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.displayLiveFallbackToUsable) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.obdMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.obdConnectCallMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.obdSecurityStartCallMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.obdDiscoveryCallMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.obdSubscribeCallMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.obdWriteCallMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.obdRssiCallMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.obdPollErrors) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.obdStaleCount) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.perfDrop) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.eventBusDrops) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiHandleClientMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiMaintenanceMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiStatusCheckMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiTimeoutCheckMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiHeapGuardMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiApStaPollMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiStopHttpServerMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiStopStaDisconnectMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiStopApDisableMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiStopModeOffMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiStartPreflightMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.wifiStartApBringupMaxUs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.freeDmaMin) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.largestDmaMin) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.bleState) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.subscribeStep) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.connectInProgress) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.asyncConnectPending) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.pendingDisconnectCleanup) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.proxyAdvertising) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.proxyAdvertisingLastTransitionReason) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.wifiPriorityMode) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.speedSourceSelected) &&
        appendCsvUInt8(line, sizeof(line), offset, snapshot.speedSourceValid) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.speedSelectedMph_x10) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.speedSelectedAgeMs) &&
        appendCsvUInt32(line, sizeof(line), offset, snapshot.speedSourceSwitches) &&
        appendCsvUInt32Last(line, sizeof(line), offset, snapshot.speedNoSourceSelections);

    if (!ok) {
        f.close();
        return false;
    }
    const size_t lineLen = offset;
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

/**
 * Standalone SD-backed performance CSV logger.
 *
 * Writes compact perf snapshots to /perf/perf_boot_<bootId>.csv using a dedicated
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
    uint8_t timeValid;
    uint8_t timeSource;
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
    uint32_t dmaFreeMin;      // Min MALLOC_CAP_DMA free bytes since session start
    uint32_t dmaLargestMin;   // Min MALLOC_CAP_DMA largest block since session start
    uint32_t bleProcessMaxUs; // Window max bleClient.process() duration
    uint32_t touchMaxUs;      // Window max touchUiModule.process() duration
    uint32_t obdMaxUs;         // Window max obdHandler.update() duration
    uint32_t gpsMaxUs;         // Window max gpsRuntimeModule.update() duration
    uint32_t cameraMaxUs;      // Window max cameraRuntimeModule.process() duration
    uint32_t lockoutMaxUs;     // Window max lockoutEnforcer.process() + signalCapture duration
    uint32_t wifiMaxUs;        // Window max wifiManager.process() duration
    uint32_t uiToScanCount;   // Screen transitions to scanning
    uint32_t uiToRestCount;   // Screen transitions to resting
    uint32_t uiScanToRestCount;   // Scanning -> resting transitions
    uint32_t uiFastScanExitCount; // Scan dwell below threshold before exit
    uint32_t uiLastScanDwellMs;   // Most recent scan dwell duration
    uint32_t uiMinScanDwellMs;    // Session minimum scan dwell duration
    uint32_t fadeDownCount;       // Fade-down actions emitted
    uint32_t fadeRestoreCount;    // Restore actions emitted
    uint32_t fadeSkipEqualCount;  // Restore skipped (current == baseline)
    uint32_t fadeSkipNoBaselineCount; // Restore skipped (missing baseline)
    uint32_t fadeSkipNotFadedCount;   // Restore skipped (session never faded)
    uint8_t fadeLastDecision;     // PerfFadeDecision code
    uint8_t fadeLastCurrentVol;   // Last observed current volume
    uint8_t fadeLastOriginalVol;  // Last observed baseline/original volume
    uint32_t fadeLastDecisionMs;  // Last fade decision timestamp
    uint32_t bleScanStartMs;      // First scan start timestamp
    uint32_t bleTargetFoundMs;    // First target-found timestamp
    uint32_t bleConnectStartMs;   // First connect-start timestamp
    uint32_t bleConnectedMs;      // First connected timestamp
    uint32_t bleFirstRxMs;        // First parsed/received V1 packet timestamp
    uint8_t obdState;             // OBDState enum value (0xFF when unavailable)
    uint8_t obdConnected;         // OBD READY/POLLING flag
    uint8_t obdScanActive;        // OBD manual scan in progress
    uint8_t obdHasValidData;      // OBD fresh-data flag
    uint32_t obdSampleAgeMs;      // OBD sample age in ms (UINT32_MAX when unknown)
    int32_t obdSpeedMphX10;       // OBD speed in mph * 10 (-1 when unavailable)
    uint8_t obdConnFailures;      // OBD connect/init failure count
    uint8_t obdPollFailStreak;    // OBD consecutive poll failure streak
    uint32_t obdNotifyDrops;      // OBD notify stream-buffer drops
    uint32_t alertPersistStarts;  // Persisted-alert sessions started
    uint32_t alertPersistExpires; // Persisted-alert windows expired naturally
    uint32_t alertPersistClears;  // Persisted-alert state cleared explicitly
    uint32_t autoPushStarts;      // Auto-push runs initiated
    uint32_t autoPushCompletes;   // Auto-push runs completed
    uint32_t autoPushNoProfile;   // Auto-push slot had no configured profile
    uint32_t autoPushProfileLoadFail;  // Auto-push profile load failures
    uint32_t autoPushProfileWriteFail; // Auto-push profile write exhausted retries
    uint32_t autoPushBusyRetries; // Auto-push write-busy retries
    uint32_t autoPushModeFail;    // Auto-push mode set failures
    uint32_t autoPushVolumeFail;  // Auto-push volume set failures
    uint32_t autoPushDisconnectAbort; // Auto-push aborted due to disconnect
    uint32_t voiceAnnouncePriority;   // Voice priority announcements emitted
    uint32_t voiceAnnounceDirection;  // Voice direction/bogey announcements emitted
    uint32_t voiceAnnounceSecondary;  // Voice secondary announcements emitted
    uint32_t voiceAnnounceEscalation; // Voice escalation announcements emitted
    uint32_t voiceDirectionThrottled; // Voice direction announcements suppressed by throttle
    uint32_t powerAutoPowerArmed;     // Auto power-off armed on first V1 data
    uint32_t powerAutoPowerTimerStart; // Auto power-off timer started
    uint32_t powerAutoPowerTimerCancel; // Auto power-off timer cancelled on reconnect
    uint32_t powerAutoPowerTimerExpire; // Auto power-off timer expired
    uint32_t powerCriticalWarn;       // Critical-battery warning shown
    uint32_t powerCriticalShutdown;   // Critical-battery shutdown triggered
    uint32_t cmdBleBusy;              // BLE command write transient failures/retries
    uint8_t gpsEnabled;               // GPS runtime enabled flag
    uint8_t gpsHasFix;                // GPS currently has fix
    uint8_t gpsLocationValid;         // GPS location fields are valid
    uint8_t gpsSatellites;            // Current satellite count
    uint8_t gpsParserActive;          // GPS parser active flag
    uint8_t gpsModuleDetected;        // UART/NMEA module detected
    uint8_t gpsDetectionTimedOut;     // Detection timeout latched
    int32_t gpsSpeedMphX10;           // GPS speed mph * 10 (-1 when unavailable)
    uint16_t gpsHdopX10;              // GPS HDOP * 10 (UINT16_MAX when unavailable)
    uint32_t gpsSampleAgeMs;          // GPS speed sample age (UINT32_MAX when unavailable)
    uint32_t gpsObsDrops;             // GPS observation ring dropped samples
    uint32_t gpsObsSize;              // GPS observation ring current size
    uint32_t gpsObsPublished;         // GPS observation ring lifetime published samples
    uint8_t cameraEnabled;            // Camera runtime enabled
    uint8_t cameraIndexLoaded;        // Camera index ready
    uint8_t cameraLastCapReached;     // Last tick hit raw candidate cap
    uint8_t cameraLoaderInProgress;   // Camera loader currently running
    uint32_t cameraTicks;             // Camera ticks executed
    uint32_t cameraTickSkipsOverload; // Camera ticks skipped due to overload guard
    uint32_t cameraTickSkipsNonCore;  // Camera ticks skipped due to non-core guard
    uint32_t cameraTickSkipsMemGuard; // Camera ticks skipped due to memory guard
    uint32_t cameraCandidatesChecked; // Camera candidates visited
    uint32_t cameraMatches;           // Camera matches found
    uint32_t cameraAlertsStarted;     // Camera alerts published
    uint32_t cameraBudgetExceeded;    // Camera candidate-cap hits
    uint32_t cameraLoadFailures;      // Camera loader hard failures
    uint32_t cameraLoadSkipsMemGuard; // Camera loader skips due to memory guard
    uint32_t cameraIndexSwapCount;    // Camera index swap successes
    uint32_t cameraIndexSwapFailures; // Camera index swap failures
    uint32_t cameraLastTickUs;        // Last camera tick duration
    uint32_t cameraMaxTickUs;         // Max camera tick duration
    uint32_t cameraLastLoadMs;        // Last camera load duration
    uint32_t cameraMaxLoadMs;         // Max camera load duration
    uint32_t cameraLastSortMs;        // Last camera sort duration
    uint32_t cameraLastSpanMs;        // Last camera span-build duration
    uint32_t cameraLastInternalFree;  // Last camera tick internal free bytes
    uint32_t cameraLastInternalBlock; // Last camera tick largest internal block
    uint32_t cameraLoaderReadyVersion; // Last published loader version

    // CSV schema v6 additions (kept at tail for backwards column stability)
    uint32_t rxBytes;
    uint32_t oversizeDrops;
    uint32_t queueHighWater;
    uint32_t bleMutexSkip;
    uint32_t bleMutexTimeout;
    uint32_t cmdPaceNotYet;
    uint32_t bleDiscTaskCreateFail;
    uint32_t displayUpdates;
    uint32_t displaySkips;
    uint32_t wifiConnectDeferred;
    uint32_t pushNowRetries;
    uint32_t pushNowFailures;
    uint32_t audioPlayCount;
    uint32_t audioPlayBusy;
    uint32_t audioTaskFail;
    uint32_t sigObsQueueDrops;
    uint32_t sigObsWriteFail;
    uint32_t minLargestBlock;
    uint32_t fsMaxUs;
    uint32_t sdMaxUs;
    uint32_t flushMaxUs;
    uint32_t bleConnectMaxUs;
    uint32_t bleDiscoveryMaxUs;
    uint32_t bleSubscribeMaxUs;
    uint32_t dispPipeMaxUs;
    uint32_t lockoutSaveMaxUs;   // Window max lockout zone JSON+SD write
    uint32_t learnerSaveMaxUs;   // Window max learner pending JSON+SD write
    uint32_t timeSaveMaxUs;      // Window max timeService.periodicSave NVS write
    uint32_t perfReportMaxUs;    // Window max perfMetricsCheckReport snapshot+enqueue
    uint32_t prioritySelectDisplayIndex; // Legacy display-aux0 priority path (compat-only)
    uint32_t prioritySelectRowFlag;      // Priority chosen from alert-row isPriority bit
    uint32_t prioritySelectFirstUsable;  // Priority chosen from first usable alert fallback
    uint32_t prioritySelectFirstEntry;   // Priority fell back to entry 0 (last resort)
    uint32_t prioritySelectAmbiguousIndex; // Alert table complete under both 0-based and 1-based mapping
    uint32_t prioritySelectUnusableIndex;  // Row-priority candidate present but unusable
    uint32_t prioritySelectInvalidChosen;  // Final chosen alert invalid/zero-freq non-laser
    uint32_t alertTablePublishes;          // Complete alert tables published
    uint32_t alertTablePublishes3Bogey;    // Complete tables published with count=3
    uint32_t alertTableRowReplacements;    // Duplicate row-index replacements
    uint32_t alertTableAssemblyTimeouts;   // Partial table assemblies dropped on timeout
    uint32_t parserRowsBandNone;           // Alert rows decoded with BAND_NONE
    uint32_t parserRowsKuRaw;              // Alert rows containing Ku raw bit (0x10)
    uint32_t displayLiveInvalidPrioritySkips; // Live display invalid-priority early returns
    uint32_t displayLiveFallbackToUsable;  // Live display fallback-to-usable selections
};

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
    uint32_t bootId = 0;
    char csvPathBuf[64] = {0};
};

extern PerfSdLogger perfSdLogger;

#endif  // PERF_SD_LOGGER_H

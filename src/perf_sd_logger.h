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
    uint32_t wifiMaxUs;       // Window max wifiManager.process() duration
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
    uint32_t speedVolBoosts;      // Speed-volume boosts applied
    uint32_t speedVolRestores;    // Speed-volume restores applied
    uint32_t speedVolFadeTakeovers; // Fade took over while speed boost active
    uint32_t speedVolNoHeadroom;  // Boost requested but volume already maxed
};

class PerfSdLogger {
public:
    void begin(bool sdAvailable);
    void setBootId(uint32_t id);
    bool enqueue(const PerfSdSnapshot& snapshot);
    bool isEnabled() const { return enabled; }
    const char* csvPath() const { return csvPathBuf; }

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

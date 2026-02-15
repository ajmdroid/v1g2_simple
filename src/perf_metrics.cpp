/**
 * Low-Overhead Performance Metrics Implementation
 */

#include "perf_metrics.h"
#include "debug_logger.h"  // For drop counter access (never log via debug logger)
#include "perf_sd_logger.h"
#include "storage_manager.h"
#include "time_service.h"
#include "obd_handler.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_observation_log.h"
#include "modules/camera/camera_runtime_module.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <cmath>

// Global instances
PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;

#if PERF_METRICS
PerfLatency perfLatency;
#endif

#if PERF_METRICS && PERF_MONITORING
bool perfDebugEnabled = false;
uint32_t perfLastReportMs = 0;
#endif

// Session minima for true MALLOC_CAP_DMA heap (updated only in sampled snapshot path).
static uint32_t sDmaFreeCapMin = UINT32_MAX;
static uint32_t sDmaLargestCapMin = UINT32_MAX;

void perfMetricsInit() {
    perfCounters.reset();
    perfExtended.reset();
    sDmaFreeCapMin = UINT32_MAX;
    sDmaLargestCapMin = UINT32_MAX;
#if PERF_METRICS
    perfLatency.reset();
#if PERF_MONITORING
    perfDebugEnabled = false;
    perfLastReportMs = millis();
#endif
#endif
}

void perfMetricsReset() {
    perfCounters.reset();
    perfExtended.reset();
    sDmaFreeCapMin = UINT32_MAX;
    sDmaLargestCapMin = UINT32_MAX;
#if PERF_METRICS
    perfLatency.reset();
#endif
}

namespace {
static constexpr uint32_t kLatencyBucketsMs[PerfHistogramMs::kBucketCount] = {
    1, 2, 5, 10, 20, 50, 100, 200, 500, 1000
};
// Keep aligned with UI scan dwell target so "fast exit" remains actionable.
static constexpr uint32_t kFastScanExitThresholdMs = 400;
static portMUX_TYPE sPerfSnapshotMux = portMUX_INITIALIZER_UNLOCKED;

static void addLatencySample(PerfHistogramMs& hist, uint32_t ms) {
    if (ms > hist.maxMs) {
        hist.maxMs = ms;
    }
    // Always increment total - values > max bucket go into overflow
    hist.total++;
    for (size_t i = 0; i < PerfHistogramMs::kBucketCount; ++i) {
        if (ms <= kLatencyBucketsMs[i]) {
            hist.buckets[i]++;
            return;
        }
    }
    // Value exceeds all buckets - counted in total but not in any bucket
    // calcP95 will return maxMs for these overflow cases
    hist.overflow++;
}

static uint32_t calcP95(const PerfHistogramMs& hist) {
    if (hist.total == 0) {
        return 0;
    }
    uint32_t target = (hist.total * 95 + 99) / 100;
    uint32_t cumulative = 0;
    for (size_t i = 0; i < PerfHistogramMs::kBucketCount; ++i) {
        cumulative += hist.buckets[i];
        if (cumulative >= target) {
            return kLatencyBucketsMs[i];
        }
    }
    return hist.maxMs;
}

static void captureSdSnapshot(PerfSdSnapshot& snapshot) {
    // Keep expensive calls outside the critical section; only copy shared state
    // while holding the lock so the snapshot is internally consistent.
    uint32_t nowMs = millis();
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t freeDma = StorageManager::getCachedFreeDma();
    uint32_t largestDma = StorageManager::getCachedLargestDma();
    uint32_t freeDmaCap = heap_caps_get_free_size(MALLOC_CAP_DMA);
    uint32_t largestDmaCap = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    OBDPerfSnapshot obdPerf = obdHandler.getPerfSnapshot();
    GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    GpsObservationLogStats gpsLogStats = gpsObservationLog.stats();
    CameraRuntimeStatus cameraStatus = cameraRuntimeModule.snapshot();

    portENTER_CRITICAL(&sPerfSnapshotMux);
    if (freeDmaCap < sDmaFreeCapMin) {
        sDmaFreeCapMin = freeDmaCap;
    }
    if (largestDmaCap < sDmaLargestCapMin) {
        sDmaLargestCapMin = largestDmaCap;
    }

    snapshot.millisTs = nowMs;
    snapshot.timeValid = timeService.timeValid() ? 1 : 0;
    snapshot.timeSource = timeService.timeSource();
    snapshot.rx = perfCounters.rxPackets.load(std::memory_order_relaxed);
    snapshot.qDrop = perfCounters.queueDrops.load(std::memory_order_relaxed);
    snapshot.parseOk = perfCounters.parseSuccesses.load(std::memory_order_relaxed);
    snapshot.parseFail = perfCounters.parseFailures.load(std::memory_order_relaxed);
    snapshot.disc = perfCounters.disconnects.load(std::memory_order_relaxed);
    snapshot.reconn = perfCounters.reconnects.load(std::memory_order_relaxed);
    snapshot.loopMaxUs = perfExtended.loopMaxUs;
    snapshot.bleDrainMaxUs = perfExtended.bleDrainMaxUs;
    snapshot.dispMaxUs = perfExtended.dispPipeMaxUs;
    snapshot.freeHeap = freeHeap;
    snapshot.freeDma = freeDma;
    snapshot.largestDma = largestDma;
    snapshot.freeDmaCap = freeDmaCap;
    snapshot.largestDmaCap = largestDmaCap;
    snapshot.dmaFreeMin = (sDmaFreeCapMin == UINT32_MAX) ? freeDmaCap : sDmaFreeCapMin;
    snapshot.dmaLargestMin = (sDmaLargestCapMin == UINT32_MAX) ? largestDmaCap : sDmaLargestCapMin;
    snapshot.bleProcessMaxUs = perfExtended.bleProcessMaxUs;
    snapshot.touchMaxUs = perfExtended.touchMaxUs;
    snapshot.obdMaxUs = perfExtended.obdMaxUs;
    snapshot.gpsMaxUs = perfExtended.gpsMaxUs;
    snapshot.cameraMaxUs = perfExtended.cameraMaxUs;
    snapshot.lockoutMaxUs = perfExtended.lockoutMaxUs;
    snapshot.wifiMaxUs = perfExtended.wifiMaxUs;
    snapshot.uiToScanCount = perfExtended.uiToScanCount;
    snapshot.uiToRestCount = perfExtended.uiToRestCount;
    snapshot.uiScanToRestCount = perfExtended.uiScanToRestCount;
    snapshot.uiFastScanExitCount = perfExtended.uiFastScanExitCount;
    snapshot.uiLastScanDwellMs = perfExtended.uiLastScanDwellMs;
    snapshot.uiMinScanDwellMs = (perfExtended.uiMinScanDwellMs == UINT32_MAX) ? 0 : perfExtended.uiMinScanDwellMs;
    snapshot.fadeDownCount = perfExtended.fadeDownCount;
    snapshot.fadeRestoreCount = perfExtended.fadeRestoreCount;
    snapshot.fadeSkipEqualCount = perfExtended.fadeSkipEqualCount;
    snapshot.fadeSkipNoBaselineCount = perfExtended.fadeSkipNoBaselineCount;
    snapshot.fadeSkipNotFadedCount = perfExtended.fadeSkipNotFadedCount;
    snapshot.fadeLastDecision = perfExtended.fadeLastDecision;
    snapshot.fadeLastCurrentVol = perfExtended.fadeLastCurrentVol;
    snapshot.fadeLastOriginalVol = perfExtended.fadeLastOriginalVol;
    snapshot.fadeLastDecisionMs = perfExtended.fadeLastDecisionMs;
    snapshot.bleScanStartMs = perfExtended.bleScanStartMs;
    snapshot.bleTargetFoundMs = perfExtended.bleTargetFoundMs;
    snapshot.bleConnectStartMs = perfExtended.bleConnectStartMs;
    snapshot.bleConnectedMs = perfExtended.bleConnectedMs;
    snapshot.bleFirstRxMs = perfExtended.bleFirstRxMs;
    snapshot.obdState = obdPerf.state;
    snapshot.obdConnected = obdPerf.connected;
    snapshot.obdScanActive = obdPerf.scanActive;
    snapshot.obdHasValidData = obdPerf.hasValidData;
    snapshot.obdSampleAgeMs = obdPerf.sampleAgeMs;
    snapshot.obdSpeedMphX10 = obdPerf.speedMphX10;
    snapshot.obdConnFailures = obdPerf.connectionFailures;
    snapshot.obdPollFailStreak = obdPerf.consecutivePollFailures;
    snapshot.obdNotifyDrops = obdPerf.notifyDrops;
    snapshot.alertPersistStarts = perfCounters.alertPersistStarts.load(std::memory_order_relaxed);
    snapshot.alertPersistExpires = perfCounters.alertPersistExpires.load(std::memory_order_relaxed);
    snapshot.alertPersistClears = perfCounters.alertPersistClears.load(std::memory_order_relaxed);
    snapshot.autoPushStarts = perfCounters.autoPushStarts.load(std::memory_order_relaxed);
    snapshot.autoPushCompletes = perfCounters.autoPushCompletes.load(std::memory_order_relaxed);
    snapshot.autoPushNoProfile = perfCounters.autoPushNoProfile.load(std::memory_order_relaxed);
    snapshot.autoPushProfileLoadFail = perfCounters.autoPushProfileLoadFail.load(std::memory_order_relaxed);
    snapshot.autoPushProfileWriteFail = perfCounters.autoPushProfileWriteFail.load(std::memory_order_relaxed);
    snapshot.autoPushBusyRetries = perfCounters.autoPushBusyRetries.load(std::memory_order_relaxed);
    snapshot.autoPushModeFail = perfCounters.autoPushModeFail.load(std::memory_order_relaxed);
    snapshot.autoPushVolumeFail = perfCounters.autoPushVolumeFail.load(std::memory_order_relaxed);
    snapshot.autoPushDisconnectAbort = perfCounters.autoPushDisconnectAbort.load(std::memory_order_relaxed);
    snapshot.speedVolBoosts = perfCounters.speedVolBoosts.load(std::memory_order_relaxed);
    snapshot.speedVolRestores = perfCounters.speedVolRestores.load(std::memory_order_relaxed);
    snapshot.speedVolFadeTakeovers = perfCounters.speedVolFadeTakeovers.load(std::memory_order_relaxed);
    snapshot.speedVolNoHeadroom = perfCounters.speedVolNoHeadroom.load(std::memory_order_relaxed);
    snapshot.voiceAnnouncePriority = perfCounters.voiceAnnouncePriority.load(std::memory_order_relaxed);
    snapshot.voiceAnnounceDirection = perfCounters.voiceAnnounceDirection.load(std::memory_order_relaxed);
    snapshot.voiceAnnounceSecondary = perfCounters.voiceAnnounceSecondary.load(std::memory_order_relaxed);
    snapshot.voiceAnnounceEscalation = perfCounters.voiceAnnounceEscalation.load(std::memory_order_relaxed);
    snapshot.voiceDirectionThrottled = perfCounters.voiceDirectionThrottled.load(std::memory_order_relaxed);
    snapshot.powerAutoPowerArmed = perfCounters.powerAutoPowerArmed.load(std::memory_order_relaxed);
    snapshot.powerAutoPowerTimerStart = perfCounters.powerAutoPowerTimerStart.load(std::memory_order_relaxed);
    snapshot.powerAutoPowerTimerCancel = perfCounters.powerAutoPowerTimerCancel.load(std::memory_order_relaxed);
    snapshot.powerAutoPowerTimerExpire = perfCounters.powerAutoPowerTimerExpire.load(std::memory_order_relaxed);
    snapshot.powerCriticalWarn = perfCounters.powerCriticalWarn.load(std::memory_order_relaxed);
    snapshot.powerCriticalShutdown = perfCounters.powerCriticalShutdown.load(std::memory_order_relaxed);
    snapshot.cmdBleBusy = perfCounters.cmdBleBusy.load(std::memory_order_relaxed);
    snapshot.gpsEnabled = gpsStatus.enabled ? 1 : 0;
    snapshot.gpsHasFix = gpsStatus.hasFix ? 1 : 0;
    snapshot.gpsLocationValid = gpsStatus.locationValid ? 1 : 0;
    snapshot.gpsSatellites = gpsStatus.satellites;
    snapshot.gpsParserActive = gpsStatus.parserActive ? 1 : 0;
    snapshot.gpsModuleDetected = gpsStatus.moduleDetected ? 1 : 0;
    snapshot.gpsDetectionTimedOut = gpsStatus.detectionTimedOut ? 1 : 0;
    snapshot.gpsSpeedMphX10 =
        (gpsStatus.sampleValid && std::isfinite(gpsStatus.speedMph))
            ? static_cast<int32_t>(std::lround(gpsStatus.speedMph * 10.0f))
            : -1;
    snapshot.gpsHdopX10 =
        std::isfinite(gpsStatus.hdop)
            ? static_cast<uint16_t>(std::lround(((gpsStatus.hdop < 0.0f) ? 0.0f : gpsStatus.hdop) * 10.0f))
            : UINT16_MAX;
    snapshot.gpsSampleAgeMs = gpsStatus.sampleAgeMs;
    snapshot.gpsObsDrops = gpsLogStats.drops;
    snapshot.gpsObsSize = static_cast<uint32_t>(gpsLogStats.size);
    snapshot.gpsObsPublished = gpsLogStats.published;
    snapshot.cameraEnabled = cameraStatus.enabled ? 1u : 0u;
    snapshot.cameraIndexLoaded = cameraStatus.indexLoaded ? 1u : 0u;
    snapshot.cameraLastCapReached = cameraStatus.lastCapReached ? 1u : 0u;
    snapshot.cameraLoaderInProgress = cameraStatus.loader.loadInProgress ? 1u : 0u;
    snapshot.cameraTicks = cameraStatus.counters.cameraTicks;
    snapshot.cameraTickSkipsOverload = cameraStatus.counters.cameraTickSkipsOverload;
    snapshot.cameraTickSkipsNonCore = cameraStatus.counters.cameraTickSkipsNonCore;
    snapshot.cameraTickSkipsMemGuard = cameraStatus.counters.cameraTickSkipsMemoryGuard;
    snapshot.cameraCandidatesChecked = cameraStatus.counters.cameraCandidatesChecked;
    snapshot.cameraMatches = cameraStatus.counters.cameraMatches;
    snapshot.cameraAlertsStarted = cameraStatus.counters.cameraAlertsStarted;
    snapshot.cameraBudgetExceeded = cameraStatus.counters.cameraBudgetExceeded;
    snapshot.cameraLoadFailures = cameraStatus.loader.loadFailures;
    snapshot.cameraLoadSkipsMemGuard = cameraStatus.loader.loadSkipsMemoryGuard;
    snapshot.cameraIndexSwapCount = cameraStatus.counters.cameraIndexSwapCount;
    snapshot.cameraIndexSwapFailures = cameraStatus.counters.cameraIndexSwapFailures;
    snapshot.cameraLastTickUs = cameraStatus.lastTickDurationUs;
    snapshot.cameraMaxTickUs = cameraStatus.maxTickDurationUs;
    snapshot.cameraLastLoadMs = cameraStatus.loader.lastLoadDurationMs;
    snapshot.cameraMaxLoadMs = cameraStatus.loader.maxLoadDurationMs;
    snapshot.cameraLastSortMs = cameraStatus.loader.lastSortDurationMs;
    snapshot.cameraLastSpanMs = cameraStatus.loader.lastSpanBuildDurationMs;
    snapshot.cameraLastInternalFree = cameraStatus.lastInternalFree;
    snapshot.cameraLastInternalBlock = cameraStatus.lastInternalLargestBlock;
    snapshot.cameraLoaderReadyVersion = cameraStatus.loader.readyVersion;

    // Windowed maxima for the CSV logger.
    perfExtended.loopMaxUs = 0;
    perfExtended.bleDrainMaxUs = 0;
    perfExtended.dispPipeMaxUs = 0;
    perfExtended.bleProcessMaxUs = 0;
    perfExtended.touchMaxUs = 0;
    perfExtended.obdMaxUs = 0;
    perfExtended.gpsMaxUs = 0;
    perfExtended.cameraMaxUs = 0;
    perfExtended.lockoutMaxUs = 0;
    perfExtended.wifiMaxUs = 0;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}
} // namespace

void perfRecordNotifyToDisplayMs(uint32_t ms) {
    addLatencySample(perfExtended.notifyToDisplayMs, ms);
}

void perfRecordNotifyToProxyMs(uint32_t ms) {
    addLatencySample(perfExtended.notifyToProxyMs, ms);
}

void perfRecordLoopJitterUs(uint32_t us) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    if (us > perfExtended.loopMaxUs) {
        perfExtended.loopMaxUs = us;
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordHeapStats(uint32_t freeHeap, uint32_t largestBlock, uint32_t freeDma, uint32_t largestDma) {
    if (freeHeap < perfExtended.minFreeHeap) {
        perfExtended.minFreeHeap = freeHeap;
    }
    if (largestBlock < perfExtended.minLargestBlock) {
        perfExtended.minLargestBlock = largestBlock;
    }
    if (freeDma < perfExtended.minFreeDma) {
        perfExtended.minFreeDma = freeDma;
    }
    if (largestDma < perfExtended.minLargestDma) {
        perfExtended.minLargestDma = largestDma;
    }
}

void perfRecordWifiProcessUs(uint32_t us) {
    if (us > perfExtended.wifiMaxUs) {
        perfExtended.wifiMaxUs = us;
    }
}

void perfRecordFsServeUs(uint32_t us) {
    if (us > perfExtended.fsMaxUs) {
        perfExtended.fsMaxUs = us;
    }
}

void perfRecordSdFlushUs(uint32_t us) {
    if (us > perfExtended.sdMaxUs) {
        perfExtended.sdMaxUs = us;
    }
}

void perfRecordFlushUs(uint32_t us) {
    if (us > perfExtended.flushMaxUs) {
        perfExtended.flushMaxUs = us;
    }
}

void perfRecordDisplayRenderUs(uint32_t us) {
    if (us > perfExtended.displayRenderMaxUs) {
        perfExtended.displayRenderMaxUs = us;
    }
}

void perfRecordBleDrainUs(uint32_t us) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    if (us > perfExtended.bleDrainMaxUs) {
        perfExtended.bleDrainMaxUs = us;
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordBleConnectUs(uint32_t us) {
    if (us > perfExtended.bleConnectMaxUs) {
        perfExtended.bleConnectMaxUs = us;
    }
}

void perfRecordBleDiscoveryUs(uint32_t us) {
    if (us > perfExtended.bleDiscoveryMaxUs) {
        perfExtended.bleDiscoveryMaxUs = us;
    }
}

void perfRecordBleSubscribeUs(uint32_t us) {
    if (us > perfExtended.bleSubscribeMaxUs) {
        perfExtended.bleSubscribeMaxUs = us;
    }
}

void perfRecordBleProcessUs(uint32_t us) {
    if (us > perfExtended.bleProcessMaxUs) {
        perfExtended.bleProcessMaxUs = us;
    }
}

void perfRecordDispPipeUs(uint32_t us) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    if (us > perfExtended.dispPipeMaxUs) {
        perfExtended.dispPipeMaxUs = us;
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordTouchUs(uint32_t us) {
    if (us > perfExtended.touchMaxUs) {
        perfExtended.touchMaxUs = us;
    }
}

void perfRecordObdUs(uint32_t us) {
    if (us > perfExtended.obdMaxUs) {
        perfExtended.obdMaxUs = us;
    }
}

void perfRecordGpsUs(uint32_t us) {
    if (us > perfExtended.gpsMaxUs) {
        perfExtended.gpsMaxUs = us;
    }
}

void perfRecordCameraUs(uint32_t us) {
    if (us > perfExtended.cameraMaxUs) {
        perfExtended.cameraMaxUs = us;
    }
}

void perfRecordLockoutUs(uint32_t us) {
    if (us > perfExtended.lockoutMaxUs) {
        perfExtended.lockoutMaxUs = us;
    }
}

void perfRecordDisplayScreenTransition(PerfDisplayScreen from, PerfDisplayScreen to, uint32_t nowMs) {
    if (from == to) {
        return;
    }

    portENTER_CRITICAL(&sPerfSnapshotMux);

    if (to == PerfDisplayScreen::Scanning) {
        perfExtended.uiToScanCount++;
        perfExtended.uiLastScanEnteredMs = nowMs;
    } else if (to == PerfDisplayScreen::Resting) {
        perfExtended.uiToRestCount++;
    }

    if (from == PerfDisplayScreen::Scanning && to == PerfDisplayScreen::Resting) {
        perfExtended.uiScanToRestCount++;
    }

    if (from == PerfDisplayScreen::Scanning && to != PerfDisplayScreen::Scanning) {
        if (to != PerfDisplayScreen::Unknown && perfExtended.uiLastScanEnteredMs > 0) {
            uint32_t dwellMs = nowMs - perfExtended.uiLastScanEnteredMs;
            perfExtended.uiLastScanDwellMs = dwellMs;
            if (dwellMs < perfExtended.uiMinScanDwellMs) {
                perfExtended.uiMinScanDwellMs = dwellMs;
            }
            if (dwellMs < kFastScanExitThresholdMs) {
                perfExtended.uiFastScanExitCount++;
            }
        }
        perfExtended.uiLastScanEnteredMs = 0;
    }

    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordVolumeFadeDecision(PerfFadeDecision decision, uint8_t currentVolume, uint8_t originalVolume, uint32_t nowMs) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    switch (decision) {
        case PerfFadeDecision::FadeDown:
            perfExtended.fadeDownCount++;
            break;
        case PerfFadeDecision::RestoreApplied:
            perfExtended.fadeRestoreCount++;
            break;
        case PerfFadeDecision::RestoreSkippedEqual:
            perfExtended.fadeSkipEqualCount++;
            break;
        case PerfFadeDecision::RestoreSkippedNoBaseline:
            perfExtended.fadeSkipNoBaselineCount++;
            break;
        case PerfFadeDecision::RestoreSkippedNotFaded:
            perfExtended.fadeSkipNotFadedCount++;
            break;
        case PerfFadeDecision::None:
        default:
            break;
    }
    perfExtended.fadeLastDecision = static_cast<uint8_t>(decision);
    perfExtended.fadeLastCurrentVol = currentVolume;
    perfExtended.fadeLastOriginalVol = originalVolume;
    perfExtended.fadeLastDecisionMs = nowMs;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordBleTimelineEvent(PerfBleTimelineEvent event, uint32_t nowMs) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    uint32_t* target = nullptr;
    switch (event) {
        case PerfBleTimelineEvent::ScanStart:
            target = &perfExtended.bleScanStartMs;
            break;
        case PerfBleTimelineEvent::TargetFound:
            target = &perfExtended.bleTargetFoundMs;
            break;
        case PerfBleTimelineEvent::ConnectStart:
            target = &perfExtended.bleConnectStartMs;
            break;
        case PerfBleTimelineEvent::Connected:
            target = &perfExtended.bleConnectedMs;
            break;
        case PerfBleTimelineEvent::FirstRx:
            target = &perfExtended.bleFirstRxMs;
            break;
        default:
            break;
    }
    if (target && *target == 0) {
        *target = nowMs;
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

uint32_t perfGetNotifyToDisplayP95Ms() { return calcP95(perfExtended.notifyToDisplayMs); }
uint32_t perfGetNotifyToDisplayMaxMs() { return perfExtended.notifyToDisplayMs.maxMs; }
uint32_t perfGetNotifyToProxyP95Ms() { return calcP95(perfExtended.notifyToProxyMs); }
uint32_t perfGetNotifyToProxyMaxMs() { return perfExtended.notifyToProxyMs.maxMs; }
uint32_t perfGetLoopMaxUs() { return perfExtended.loopMaxUs; }
uint32_t perfGetMinFreeHeap() { return perfExtended.minFreeHeap == UINT32_MAX ? 0 : perfExtended.minFreeHeap; }
uint32_t perfGetMinLargestBlock() { return perfExtended.minLargestBlock == UINT32_MAX ? 0 : perfExtended.minLargestBlock; }
uint32_t perfGetMinFreeDma() { return perfExtended.minFreeDma == UINT32_MAX ? 0 : perfExtended.minFreeDma; }
uint32_t perfGetMinLargestDma() { return perfExtended.minLargestDma == UINT32_MAX ? 0 : perfExtended.minLargestDma; }
uint32_t perfGetWifiMaxUs() { return perfExtended.wifiMaxUs; }
uint32_t perfGetFsMaxUs() { return perfExtended.fsMaxUs; }
uint32_t perfGetSdMaxUs() { return perfExtended.sdMaxUs; }
uint32_t perfGetFlushMaxUs() { return perfExtended.flushMaxUs; }
uint32_t perfGetDisplayRenderMaxUs() { return perfExtended.displayRenderMaxUs; }
uint32_t perfGetBleDrainMaxUs() { return perfExtended.bleDrainMaxUs; }
uint32_t perfGetBleConnectMaxUs() { return perfExtended.bleConnectMaxUs; }
uint32_t perfGetBleDiscoveryMaxUs() { return perfExtended.bleDiscoveryMaxUs; }
uint32_t perfGetBleSubscribeMaxUs() { return perfExtended.bleSubscribeMaxUs; }
uint32_t perfGetBleProcessMaxUs() { return perfExtended.bleProcessMaxUs; }
uint32_t perfGetDispPipeMaxUs() { return perfExtended.dispPipeMaxUs; }
uint32_t perfGetTouchMaxUs() { return perfExtended.touchMaxUs; }
uint32_t perfGetObdMaxUs() { return perfExtended.obdMaxUs; }
uint32_t perfGetGpsMaxUs() { return perfExtended.gpsMaxUs; }
uint32_t perfGetCameraMaxUs() { return perfExtended.cameraMaxUs; }
uint32_t perfGetLockoutMaxUs() { return perfExtended.lockoutMaxUs; }

void perfExtendedResetWindow() {
    perfExtended.reset();
}

#if PERF_METRICS && PERF_MONITORING
bool perfMetricsCheckReport() {
    if (!perfSdLogger.isEnabled()) {
        return false;
    }
    
    uint32_t now = millis();
    constexpr uint32_t STABILITY_REPORT_INTERVAL_MS = 5000;
    if (perfLastReportMs == 0) {
        perfLastReportMs = now;
        return false;
    }
    if (now - perfLastReportMs < STABILITY_REPORT_INTERVAL_MS) {
        return false;
    }
    perfLastReportMs = now;

    PerfSdSnapshot snapshot{};
    captureSdSnapshot(snapshot);
    perfSdLogger.enqueue(snapshot);
    return true;
}
#else
bool perfMetricsCheckReport() {
    return false;
}
#endif

bool perfMetricsEnqueueSnapshotNow() {
    if (!perfSdLogger.isEnabled()) {
        return false;
    }

    PerfSdSnapshot snapshot{};
    captureSdSnapshot(snapshot);
    return perfSdLogger.enqueue(snapshot);
}

void perfMetricsPrint() {
#if PERF_METRICS && PERF_MONITORING
    uint32_t avgUs = perfLatency.avgUs();
    uint32_t minUsVal = perfLatency.minUs.load();
    uint32_t minUs = (minUsVal == UINT32_MAX) ? 0 : minUsVal;
    
    // Guard: skip report if serial TX buffer has backpressure (prevents 10-30ms stall)
    if (Serial.availableForWrite() < 128) return;

    // Single snprintf + print to minimize CDC transactions and avoid interleaved jitter
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "[PERF] rx=%lu rxB=%lu pOk=%lu pFail=%lu "
        "qDrop=%lu perfDrop=%lu qOver=%lu qHW=%lu proxyHW=%lu phoneHW=%lu "
        "dUpd=%lu dSkip=%lu "
        "reconn=%lu disc=%lu "
        "mSkip=%lu mTout=%lu pace=%lu bleBusy=%lu "
        "uuid128=%lu discTaskFail=%lu wifiConnDef=%lu pushRetry=%lu pushFail=%lu "
        "sdFail=%lu/%lu/%lu/%lu/%lu/%lu "
        "logRate=%lu logBuf=%lu logQ=%lu "
        "latMin=%luus avg=%luus max=%luus n=%lu\n",
        (unsigned long)perfCounters.rxPackets.load(),
        (unsigned long)perfCounters.rxBytes.load(),
        (unsigned long)perfCounters.parseSuccesses.load(),
        (unsigned long)perfCounters.parseFailures.load(),
        (unsigned long)perfCounters.queueDrops.load(),
        (unsigned long)perfCounters.perfDrop.load(),
        (unsigned long)perfCounters.oversizeDrops.load(),
        (unsigned long)perfCounters.queueHighWater.load(),
        (unsigned long)perfCounters.proxyQueueHighWater.load(),
        (unsigned long)perfCounters.phoneCmdQueueHighWater.load(),
        (unsigned long)perfCounters.displayUpdates.load(),
        (unsigned long)perfCounters.displaySkips.load(),
        (unsigned long)perfCounters.reconnects.load(),
        (unsigned long)perfCounters.disconnects.load(),
        (unsigned long)perfCounters.bleMutexSkip.load(),
        (unsigned long)perfCounters.bleMutexTimeout.load(),
        (unsigned long)perfCounters.cmdPaceNotYet.load(),
        (unsigned long)perfCounters.cmdBleBusy.load(),
        (unsigned long)perfCounters.uuid128FallbackHits.load(),
        (unsigned long)perfCounters.bleDiscTaskCreateFail.load(),
        (unsigned long)perfCounters.wifiConnectDeferred.load(),
        (unsigned long)perfCounters.pushNowRetries.load(),
        (unsigned long)perfCounters.pushNowFailures.load(),
        (unsigned long)perfCounters.perfSdLockFail.load(),
        (unsigned long)perfCounters.perfSdDirFail.load(),
        (unsigned long)perfCounters.perfSdOpenFail.load(),
        (unsigned long)perfCounters.perfSdHeaderFail.load(),
        (unsigned long)perfCounters.perfSdMarkerFail.load(),
        (unsigned long)perfCounters.perfSdWriteFail.load(),
        (unsigned long)debugLogger.getRateLimitDrops(),
        (unsigned long)debugLogger.getBufferFullDrops(),
        (unsigned long)debugLogger.getDropCount(),
        (unsigned long)minUs,
        (unsigned long)avgUs,
        (unsigned long)perfLatency.maxUs.load(),
        (unsigned long)perfLatency.sampleCount.load());
    if (n > 0 && n < (int)sizeof(buf)) {
        Serial.print(buf);
    }
#elif PERF_METRICS
    Serial.println("Performance monitoring disabled (PERF_MONITORING=0)");
#else
    Serial.println("Performance metrics disabled (PERF_METRICS=0)");
#endif
}

String perfMetricsToJson() {
    JsonDocument doc;
    
    doc["rxPackets"] = perfCounters.rxPackets.load();
    doc["rxBytes"] = perfCounters.rxBytes.load();
    doc["parseSuccesses"] = perfCounters.parseSuccesses.load();
    doc["parseFailures"] = perfCounters.parseFailures.load();
    doc["queueDrops"] = perfCounters.queueDrops.load();
    doc["perfDrop"] = perfCounters.perfDrop.load();
    doc["perfSdLockFail"] = perfCounters.perfSdLockFail.load();
    doc["perfSdDirFail"] = perfCounters.perfSdDirFail.load();
    doc["perfSdOpenFail"] = perfCounters.perfSdOpenFail.load();
    doc["perfSdHeaderFail"] = perfCounters.perfSdHeaderFail.load();
    doc["perfSdMarkerFail"] = perfCounters.perfSdMarkerFail.load();
    doc["perfSdWriteFail"] = perfCounters.perfSdWriteFail.load();
    doc["oversizeDrops"] = perfCounters.oversizeDrops.load();
    doc["queueHighWater"] = perfCounters.queueHighWater.load();
    doc["proxyQueueHighWater"] = perfCounters.proxyQueueHighWater.load();
    doc["phoneCmdQueueHighWater"] = perfCounters.phoneCmdQueueHighWater.load();
    doc["displayUpdates"] = perfCounters.displayUpdates.load();
    doc["displaySkips"] = perfCounters.displaySkips.load();
    doc["reconnects"] = perfCounters.reconnects.load();
    doc["disconnects"] = perfCounters.disconnects.load();
    doc["uuid128FallbackHits"] = perfCounters.uuid128FallbackHits.load();
    doc["bleDiscTaskCreateFail"] = perfCounters.bleDiscTaskCreateFail.load();
    doc["wifiConnectDeferred"] = perfCounters.wifiConnectDeferred.load();
    doc["pushNowRetries"] = perfCounters.pushNowRetries.load();
    doc["pushNowFailures"] = perfCounters.pushNowFailures.load();
    doc["alertPersistStarts"] = perfCounters.alertPersistStarts.load();
    doc["alertPersistExpires"] = perfCounters.alertPersistExpires.load();
    doc["alertPersistClears"] = perfCounters.alertPersistClears.load();
    doc["autoPushStarts"] = perfCounters.autoPushStarts.load();
    doc["autoPushCompletes"] = perfCounters.autoPushCompletes.load();
    doc["autoPushNoProfile"] = perfCounters.autoPushNoProfile.load();
    doc["autoPushProfileLoadFail"] = perfCounters.autoPushProfileLoadFail.load();
    doc["autoPushProfileWriteFail"] = perfCounters.autoPushProfileWriteFail.load();
    doc["autoPushBusyRetries"] = perfCounters.autoPushBusyRetries.load();
    doc["autoPushModeFail"] = perfCounters.autoPushModeFail.load();
    doc["autoPushVolumeFail"] = perfCounters.autoPushVolumeFail.load();
    doc["autoPushDisconnectAbort"] = perfCounters.autoPushDisconnectAbort.load();
    doc["speedVolBoosts"] = perfCounters.speedVolBoosts.load();
    doc["speedVolRestores"] = perfCounters.speedVolRestores.load();
    doc["speedVolFadeTakeovers"] = perfCounters.speedVolFadeTakeovers.load();
    doc["speedVolNoHeadroom"] = perfCounters.speedVolNoHeadroom.load();
    doc["voiceAnnouncePriority"] = perfCounters.voiceAnnouncePriority.load();
    doc["voiceAnnounceDirection"] = perfCounters.voiceAnnounceDirection.load();
    doc["voiceAnnounceSecondary"] = perfCounters.voiceAnnounceSecondary.load();
    doc["voiceAnnounceEscalation"] = perfCounters.voiceAnnounceEscalation.load();
    doc["voiceDirectionThrottled"] = perfCounters.voiceDirectionThrottled.load();
    doc["powerAutoPowerArmed"] = perfCounters.powerAutoPowerArmed.load();
    doc["powerAutoPowerTimerStart"] = perfCounters.powerAutoPowerTimerStart.load();
    doc["powerAutoPowerTimerCancel"] = perfCounters.powerAutoPowerTimerCancel.load();
    doc["powerAutoPowerTimerExpire"] = perfCounters.powerAutoPowerTimerExpire.load();
    doc["powerCriticalWarn"] = perfCounters.powerCriticalWarn.load();
    doc["powerCriticalShutdown"] = perfCounters.powerCriticalShutdown.load();
    
#if PERF_METRICS
    doc["monitoringEnabled"] = (bool)PERF_MONITORING;
#if PERF_MONITORING
    uint32_t minUsVal = perfLatency.minUs.load();
    uint32_t minUs = (minUsVal == UINT32_MAX) ? 0 : minUsVal;
    doc["latencyMinUs"] = minUs;
    doc["latencyAvgUs"] = perfLatency.avgUs();
    doc["latencyMaxUs"] = perfLatency.maxUs.load();
    doc["latencySamples"] = perfLatency.sampleCount.load();
    doc["debugEnabled"] = perfDebugEnabled;
#else
    doc["latencyMinUs"] = 0;
    doc["latencyAvgUs"] = 0;
    doc["latencyMaxUs"] = 0;
    doc["latencySamples"] = 0;
    doc["debugEnabled"] = false;
#endif
#else
    doc["metricsEnabled"] = false;
#endif
    
    String json;
    serializeJson(doc, json);
    return json;
}

void perfMetricsSetDebug(bool enabled) {
#if PERF_METRICS && PERF_MONITORING
    perfDebugEnabled = enabled;
    if (enabled) {
        perfLastReportMs = millis();  // Reset report timer
    }
#else
    (void)enabled;
#endif
}

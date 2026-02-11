/**
 * Low-Overhead Performance Metrics Implementation
 */

#include "perf_metrics.h"
#include "debug_logger.h"  // For drop counter access (never log via debug logger)
#include "perf_sd_logger.h"
#include "storage_manager.h"
#include "time_service.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>

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
    snapshot.wifiMaxUs = perfExtended.wifiMaxUs;

    // Windowed maxima for the CSV logger.
    perfExtended.loopMaxUs = 0;
    perfExtended.bleDrainMaxUs = 0;
    perfExtended.dispPipeMaxUs = 0;
    perfExtended.bleProcessMaxUs = 0;
    perfExtended.touchMaxUs = 0;
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

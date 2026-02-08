/**
 * Low-Overhead Performance Metrics Implementation
 */

#include "perf_metrics.h"
#include "debug_logger.h"  // For drop counter access (never log via debug logger)
#include "debug_logger.h"
#include "settings.h"
#include <ArduinoJson.h>

// PerfMetrics logging macro - logs to Serial AND debugLogger when PerfMetrics category enabled
static constexpr bool PERF_DEBUG_LOGS = false;  // Set true for verbose Serial logging
#define PERF_LOG(...) do { \
    if (PERF_DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    if (debugLogger.isEnabledFor(DebugLogCategory::PerfMetrics)) debugLogger.logf(DebugLogCategory::PerfMetrics, __VA_ARGS__); \
} while(0)

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

void perfMetricsInit() {
    perfCounters.reset();
    perfExtended.reset();
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
#if PERF_METRICS
    perfLatency.reset();
#endif
}

namespace {
static constexpr uint32_t kLatencyBucketsMs[PerfHistogramMs::kBucketCount] = {
    1, 2, 5, 10, 20, 50, 100, 200, 500, 1000
};

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
} // namespace

void perfRecordNotifyToDisplayMs(uint32_t ms) {
    addLatencySample(perfExtended.notifyToDisplayMs, ms);
}

void perfRecordNotifyToProxyMs(uint32_t ms) {
    addLatencySample(perfExtended.notifyToProxyMs, ms);
}

void perfRecordLoopJitterUs(uint32_t us) {
    if (us > perfExtended.loopMaxUs) {
        perfExtended.loopMaxUs = us;
    }
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
    if (us > perfExtended.bleDrainMaxUs) {
        perfExtended.bleDrainMaxUs = us;
    }
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

void perfRecordGpsUs(uint32_t us) {
    if (us > perfExtended.gpsMaxUs) {
        perfExtended.gpsMaxUs = us;
    }
}

void perfRecordObdUs(uint32_t us) {
    if (us > perfExtended.obdMaxUs) {
        perfExtended.obdMaxUs = us;
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

void perfRecordDispPipeUs(uint32_t us) {
    if (us > perfExtended.dispPipeMaxUs) {
        perfExtended.dispPipeMaxUs = us;
    }
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
uint32_t perfGetGpsMaxUs() { return perfExtended.gpsMaxUs; }
uint32_t perfGetObdMaxUs() { return perfExtended.obdMaxUs; }
uint32_t perfGetCameraMaxUs() { return perfExtended.cameraMaxUs; }
uint32_t perfGetLockoutMaxUs() { return perfExtended.lockoutMaxUs; }
uint32_t perfGetDispPipeMaxUs() { return perfExtended.dispPipeMaxUs; }
uint32_t perfGetTouchMaxUs() { return perfExtended.touchMaxUs; }

void perfExtendedResetWindow() {
    perfExtended.reset();
}

#if PERF_METRICS && PERF_MONITORING
bool perfMetricsCheckReport() {
    // Check if perf logging is enabled via settings (not just perfDebugEnabled)
    extern SettingsManager settingsManager;
    bool logEnabled = settingsManager.get().logPerfMetrics || perfDebugEnabled;
    if (!logEnabled) {
        return false;
    }
    
    uint32_t now = millis();
    // Use 2s interval for stability diagnosis (configurable at compile time)
    constexpr uint32_t STABILITY_REPORT_INTERVAL_MS = 2000;
    if (now - perfLastReportMs < STABILITY_REPORT_INTERVAL_MS) {
        return false;
    }
    perfLastReportMs = now;
    
    // Stability-focused compact report format per CT's recommendation:
    // loopMax_us, bleDrainMax_us, bleConnMax_us, wifiMax_us, sdMax_us, gpsMax_us, obdMax_us, camMax_us, lockoutMax_us, dispMax_us, touchMax_us, heapMin, qDrop, parseFail, qHW
    // Plus otherMax_us = gap not explained by instrumented buckets (signals uninstrumented code)
    uint32_t loopUs = perfExtended.loopMaxUs;
    uint32_t bleConnUs = perfExtended.bleProcessMaxUs;
    uint32_t bleDrainUs = perfExtended.bleDrainMaxUs;
    uint32_t wifiUs = perfExtended.wifiMaxUs;
    uint32_t sdUs = perfExtended.sdMaxUs;
    uint32_t gpsUs = perfExtended.gpsMaxUs;
    uint32_t obdUs = perfExtended.obdMaxUs;
    uint32_t camUs = perfExtended.cameraMaxUs;
    uint32_t lockoutUs = perfExtended.lockoutMaxUs;
    uint32_t dispUs = perfExtended.dispPipeMaxUs;
    uint32_t touchUs = perfExtended.touchMaxUs;
    
    // Calculate "other" = unexplained loop time (not additive, but signals gaps)
    // If loopMax is huge but all buckets are small, something else blocked
    uint32_t explainedMax = bleConnUs;
    if (bleDrainUs > explainedMax) explainedMax = bleDrainUs;
    if (wifiUs > explainedMax) explainedMax = wifiUs;
    if (sdUs > explainedMax) explainedMax = sdUs;
    if (gpsUs > explainedMax) explainedMax = gpsUs;
    if (obdUs > explainedMax) explainedMax = obdUs;
    if (camUs > explainedMax) explainedMax = camUs;
    if (lockoutUs > explainedMax) explainedMax = lockoutUs;
    if (dispUs > explainedMax) explainedMax = dispUs;
    if (touchUs > explainedMax) explainedMax = touchUs;
    uint32_t otherUs = (loopUs > explainedMax) ? (loopUs - explainedMax) : 0;
    
    PERF_LOG("[PERF] loopMax_us=%lu bleConnMax_us=%lu bleDrainMax_us=%lu wifiMax_us=%lu sdMax_us=%lu gpsMax_us=%lu obdMax_us=%lu camMax_us=%lu lockoutMax_us=%lu dispMax_us=%lu touchMax_us=%lu otherMax_us=%lu heapMin=%lu heapBlock=%lu dmaMin=%lu dmaBlock=%lu qDrop=%lu parseFail=%lu qHW=%lu",
        (unsigned long)loopUs,
        (unsigned long)bleConnUs,
        (unsigned long)bleDrainUs,
        (unsigned long)wifiUs,
        (unsigned long)sdUs,
        (unsigned long)gpsUs,
        (unsigned long)obdUs,
        (unsigned long)camUs,
        (unsigned long)lockoutUs,
        (unsigned long)dispUs,
        (unsigned long)touchUs,
        (unsigned long)otherUs,
        (unsigned long)perfExtended.minFreeHeap,
        (unsigned long)perfExtended.minLargestBlock,
        (unsigned long)perfExtended.minFreeDma,
        (unsigned long)perfExtended.minLargestDma,
        (unsigned long)perfCounters.queueDrops.load(),
        (unsigned long)perfCounters.parseFailures.load(),
        (unsigned long)perfCounters.queueHighWater.load());
    
    // Reset window metrics (counters are cumulative)
    perfExtended.reset();
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
    
    Serial.println("=== Performance Metrics ===");
    Serial.printf("RX: packets=%lu bytes=%lu\n", 
        (unsigned long)perfCounters.rxPackets.load(), 
        (unsigned long)perfCounters.rxBytes.load());
    Serial.printf("Parse: ok=%lu fail=%lu\n",
        (unsigned long)perfCounters.parseSuccesses.load(),
        (unsigned long)perfCounters.parseFailures.load());
    Serial.printf("Queue: drops=%lu oversize=%lu highWater=%lu\n",
        (unsigned long)perfCounters.queueDrops.load(),
        (unsigned long)perfCounters.oversizeDrops.load(),
        (unsigned long)perfCounters.queueHighWater.load());
    Serial.printf("Queues: proxyHW=%lu phoneHW=%lu obdScanHW=%lu\n",
        (unsigned long)perfCounters.proxyQueueHighWater.load(),
        (unsigned long)perfCounters.phoneCmdQueueHighWater.load(),
        (unsigned long)perfCounters.obdScanQueueHighWater.load());
    Serial.printf("Display: updates=%lu skips=%lu\n",
        (unsigned long)perfCounters.displayUpdates.load(),
        (unsigned long)perfCounters.displaySkips.load());
    Serial.printf("Camera: bgLoads=%lu cacheRefreshes=%lu\n",
        (unsigned long)perfCounters.cameraBgLoads.load(),
        (unsigned long)perfCounters.cameraCacheRefreshes.load());
    Serial.printf("Connection: reconnects=%lu disconnects=%lu\n",
        (unsigned long)perfCounters.reconnects.load(),
        (unsigned long)perfCounters.disconnects.load());
    Serial.printf("Mutex: skip=%lu timeout=%lu paceNotYet=%lu bleBusy=%lu\n",
        (unsigned long)perfCounters.bleMutexSkip.load(),
        (unsigned long)perfCounters.bleMutexTimeout.load(),
        (unsigned long)perfCounters.cmdPaceNotYet.load(),
        (unsigned long)perfCounters.cmdBleBusy.load());
    Serial.printf("DebugLog: rateDrops=%lu bufDrops=%lu queueDrops=%lu\n",
        (unsigned long)debugLogger.getRateLimitDrops(),
        (unsigned long)debugLogger.getBufferFullDrops(),
        (unsigned long)debugLogger.getDropCount());
    Serial.printf("Latency (BLE->flush): min=%luus avg=%luus max=%luus samples=%lu\n",
        (unsigned long)minUs,
        (unsigned long)avgUs,
        (unsigned long)perfLatency.maxUs.load(),
        (unsigned long)perfLatency.sampleCount.load());
    Serial.println("===========================");
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
    doc["oversizeDrops"] = perfCounters.oversizeDrops.load();
    doc["queueHighWater"] = perfCounters.queueHighWater.load();
    doc["proxyQueueHighWater"] = perfCounters.proxyQueueHighWater.load();
    doc["phoneCmdQueueHighWater"] = perfCounters.phoneCmdQueueHighWater.load();
    doc["obdScanQueueHighWater"] = perfCounters.obdScanQueueHighWater.load();
    doc["displayUpdates"] = perfCounters.displayUpdates.load();
    doc["displaySkips"] = perfCounters.displaySkips.load();
    doc["cameraBgLoads"] = perfCounters.cameraBgLoads.load();
    doc["cameraCacheRefreshes"] = perfCounters.cameraCacheRefreshes.load();
    doc["reconnects"] = perfCounters.reconnects.load();
    doc["disconnects"] = perfCounters.disconnects.load();
    
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

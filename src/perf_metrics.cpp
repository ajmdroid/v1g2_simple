/**
 * Low-Overhead Performance Metrics Implementation
 */

#include "perf_metrics.h"
#include "debug_logger.h"
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
    1, 2, 5, 10, 20, 50, 100, 200, 500, UINT32_MAX
};

static void addLatencySample(PerfHistogramMs& hist, uint32_t ms) {
    if (ms > hist.maxMs) {
        hist.maxMs = ms;
    }
    for (size_t i = 0; i < PerfHistogramMs::kBucketCount; ++i) {
        if (ms <= kLatencyBucketsMs[i]) {
            hist.buckets[i]++;
            hist.total++;
            return;
        }
    }
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

void perfRecordHeapStats(uint32_t freeHeap, uint32_t largestBlock) {
    if (freeHeap < perfExtended.minFreeHeap) {
        perfExtended.minFreeHeap = freeHeap;
    }
    if (largestBlock < perfExtended.minLargestBlock) {
        perfExtended.minLargestBlock = largestBlock;
    }
}

uint32_t perfGetNotifyToDisplayP95Ms() { return calcP95(perfExtended.notifyToDisplayMs); }
uint32_t perfGetNotifyToDisplayMaxMs() { return perfExtended.notifyToDisplayMs.maxMs; }
uint32_t perfGetNotifyToProxyP95Ms() { return calcP95(perfExtended.notifyToProxyMs); }
uint32_t perfGetNotifyToProxyMaxMs() { return perfExtended.notifyToProxyMs.maxMs; }
uint32_t perfGetLoopMaxUs() { return perfExtended.loopMaxUs; }
uint32_t perfGetMinFreeHeap() { return perfExtended.minFreeHeap == UINT32_MAX ? 0 : perfExtended.minFreeHeap; }
uint32_t perfGetMinLargestBlock() { return perfExtended.minLargestBlock == UINT32_MAX ? 0 : perfExtended.minLargestBlock; }

void perfExtendedResetWindow() {
    perfExtended.reset();
}

#if PERF_METRICS && PERF_MONITORING
bool perfMetricsCheckReport() {
    if (!perfDebugEnabled) {
        return false;
    }
    
    uint32_t now = millis();
    if (now - perfLastReportMs < PERF_REPORT_INTERVAL_MS) {
        return false;
    }
    perfLastReportMs = now;
    
    // Single-line compact report
    uint32_t avgUs = perfLatency.avgUs();
    uint32_t minUsVal = perfLatency.minUs.load();
    uint32_t minUs = (minUsVal == UINT32_MAX) ? 0 : minUsVal;
    
    PERF_LOG("[METRICS] rx=%lu parse=%lu drop=%lu oversize=%lu hw=%lu lat=%lu/%lu/%luus updates=%lu camLoad=%lu camCache=%lu\n",
        (unsigned long)perfCounters.rxPackets.load(),
        (unsigned long)perfCounters.parseSuccesses.load(),
        (unsigned long)perfCounters.queueDrops.load(),
        (unsigned long)perfCounters.oversizeDrops.load(),
        (unsigned long)perfCounters.queueHighWater.load(),
        (unsigned long)minUs,
        (unsigned long)avgUs,
        (unsigned long)perfLatency.maxUs.load(),
        (unsigned long)perfCounters.displayUpdates.load(),
        (unsigned long)perfCounters.cameraBgLoads.load(),
        (unsigned long)perfCounters.cameraCacheRefreshes.load());
    
    // Reset latency stats for next window (counters are cumulative)
    perfLatency.reset();
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
    Serial.printf("Display: updates=%lu skips=%lu\n",
        (unsigned long)perfCounters.displayUpdates.load(),
        (unsigned long)perfCounters.displaySkips.load());
    Serial.printf("Camera: bgLoads=%lu cacheRefreshes=%lu\n",
        (unsigned long)perfCounters.cameraBgLoads.load(),
        (unsigned long)perfCounters.cameraCacheRefreshes.load());
    Serial.printf("Connection: reconnects=%lu disconnects=%lu\n",
        (unsigned long)perfCounters.reconnects.load(),
        (unsigned long)perfCounters.disconnects.load());
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

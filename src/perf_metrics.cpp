/**
 * Low-Overhead Performance Metrics Implementation
 */

#include "perf_metrics.h"
#include <ArduinoJson.h>

// Global instances
PerfCounters perfCounters;

#if PERF_METRICS
PerfLatency perfLatency;
#endif

#if PERF_METRICS && PERF_MONITORING
bool perfDebugEnabled = false;
uint32_t perfLastReportMs = 0;
#endif

void perfMetricsInit() {
    perfCounters.reset();
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
#if PERF_METRICS
    perfLatency.reset();
#endif
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
    
    Serial.printf("[METRICS] rx=%lu parse=%lu drop=%lu hw=%lu lat=%lu/%lu/%luus updates=%lu\n",
        (unsigned long)perfCounters.rxPackets.load(),
        (unsigned long)perfCounters.parseSuccesses.load(),
        (unsigned long)perfCounters.queueDrops.load(),
        (unsigned long)perfCounters.queueHighWater.load(),
        (unsigned long)minUs,
        (unsigned long)avgUs,
        (unsigned long)perfLatency.maxUs.load(),
        (unsigned long)perfCounters.displayUpdates.load());
    
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
    Serial.printf("Queue: drops=%lu highWater=%lu\n",
        (unsigned long)perfCounters.queueDrops.load(),
        (unsigned long)perfCounters.queueHighWater.load());
    Serial.printf("Display: updates=%lu skips=%lu\n",
        (unsigned long)perfCounters.displayUpdates.load(),
        (unsigned long)perfCounters.displaySkips.load());
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
    doc["queueHighWater"] = perfCounters.queueHighWater.load();
    doc["displayUpdates"] = perfCounters.displayUpdates.load();
    doc["displaySkips"] = perfCounters.displaySkips.load();
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

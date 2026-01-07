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
    uint32_t minUs = (perfLatency.minUs == UINT32_MAX) ? 0 : perfLatency.minUs;
    
    Serial.printf("[METRICS] rx=%lu parse=%lu drop=%lu hw=%lu lat=%lu/%lu/%luus updates=%lu\n",
        (unsigned long)perfCounters.rxPackets,
        (unsigned long)perfCounters.parseSuccesses,
        (unsigned long)perfCounters.queueDrops,
        (unsigned long)perfCounters.queueHighWater,
        (unsigned long)minUs,
        (unsigned long)avgUs,
        (unsigned long)perfLatency.maxUs,
        (unsigned long)perfCounters.displayUpdates);
    
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
    uint32_t minUs = (perfLatency.minUs == UINT32_MAX) ? 0 : perfLatency.minUs;
    
    Serial.println("=== Performance Metrics ===");
    Serial.printf("RX: packets=%lu bytes=%lu\n", 
        (unsigned long)perfCounters.rxPackets, 
        (unsigned long)perfCounters.rxBytes);
    Serial.printf("Parse: ok=%lu fail=%lu\n",
        (unsigned long)perfCounters.parseSuccesses,
        (unsigned long)perfCounters.parseFailures);
    Serial.printf("Queue: drops=%lu highWater=%lu\n",
        (unsigned long)perfCounters.queueDrops,
        (unsigned long)perfCounters.queueHighWater);
    Serial.printf("Display: updates=%lu skips=%lu\n",
        (unsigned long)perfCounters.displayUpdates,
        (unsigned long)perfCounters.displaySkips);
    Serial.printf("Connection: reconnects=%lu disconnects=%lu\n",
        (unsigned long)perfCounters.reconnects,
        (unsigned long)perfCounters.disconnects);
    Serial.printf("Latency (BLE->flush): min=%luus avg=%luus max=%luus samples=%lu\n",
        (unsigned long)minUs,
        (unsigned long)avgUs,
        (unsigned long)perfLatency.maxUs,
        (unsigned long)perfLatency.sampleCount);
    Serial.println("===========================");

    Serial.println("Performance monitoring disabled (PERF_MONITORING=0)");
#else
    Serial.println("Performance metrics disabled (PERF_METRICS=0)");
#endif
}

String perfMetricsToJson() {
    JsonDocument doc;
    
    doc["rxPackets"] = perfCounters.rxPackets;
    doc["rxBytes"] = perfCounters.rxBytes;
    doc["parseSuccesses"] = perfCounters.parseSuccesses;
    doc["parseFailures"] = perfCounters.parseFailures;
    doc["queueDrops"] = perfCounters.queueDrops;
    doc["queueHighWater"] = perfCounters.queueHighWater;
    doc["displayUpdates"] = perfCounters.displayUpdates;
    doc["displaySkips"] = perfCounters.displaySkips;
    doc["reconnects"] = perfCounters.reconnects;
    doc["disconnects"] = perfCounters.disconnects;
    
#if PERF_METRICS
    doc["monitoringEnabled"] = (bool)PERF_MONITORING;
#if PERF_MONITORING
    uint32_t minUs = (perfLatency.minUs == UINT32_MAX) ? 0 : perfLatency.minUs;
    doc["latencyMinUs"] = minUs;
    doc["latencyAvgUs"] = perfLatency.avgUs();
    doc["latencyMaxUs"] = perfLatency.maxUs;
    doc["latencySamples"] = perfLatency.sampleCount;
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

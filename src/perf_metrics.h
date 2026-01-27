/**
 * Low-Overhead Performance Metrics
 * 
 * Embedded-friendly observability for BLE→display latency tracking.
 * 
 * Design principles:
 * - No heap allocations
 * - No logging in hot paths
 * - Counters/timestamps stored in RAM
 * - Sampled timing (1/N packets) to reduce overhead
 * - Compile-time gating via PERF_METRICS
 * 
 * Usage:
 * - PERF_METRICS=0: Release builds, only essential counters
 * - PERF_METRICS=1: Debug builds, sampled timing + periodic reports
 */

#ifndef PERF_METRICS_H
#define PERF_METRICS_H

#include <Arduino.h>
#include <atomic>

// ============================================================================
// Compile-time gating
// Set PERF_METRICS=0 for release builds (minimal overhead)
// Set PERF_METRICS=1 for debug builds (sampled timing + reports)
// ============================================================================
#ifndef PERF_METRICS
#define PERF_METRICS 0  // Disabled for release
#endif

// Compile-time toggles for monitoring and verbose alerts
#ifndef PERF_MONITORING
#define PERF_MONITORING 1  // Disable to keep counters only (no sampling/prints)
#endif

#ifndef PERF_VERBOSE
#define PERF_VERBOSE 0  // Enable to allow immediate alerts and stage timings
#endif

// Sampling rate: measure 1 in N packets to reduce overhead
#ifndef PERF_SAMPLE_RATE
#define PERF_SAMPLE_RATE 8  // Measure every 8th packet
#endif

// Report interval (only in DEBUG mode)
#ifndef PERF_REPORT_INTERVAL_MS
#define PERF_REPORT_INTERVAL_MS 10000  // 10 seconds
#endif

// Threshold for alert (print immediately if exceeded)
#ifndef PERF_LATENCY_ALERT_MS
#define PERF_LATENCY_ALERT_MS 100  // Alert if latency > 100ms
#endif

// ============================================================================
// Always-on counters (zero overhead when not accessed)
// Uses std::atomic for thread-safe access from main loop and web handlers
// ============================================================================
struct PerfCounters {
    // Packet flow
    std::atomic<uint32_t> rxPackets{0};        // Total BLE notifications received
    std::atomic<uint32_t> rxBytes{0};          // Total bytes received
    std::atomic<uint32_t> queueDrops{0};       // Packets dropped (queue full)
    std::atomic<uint32_t> oversizeDrops{0};    // Packets dropped (too large for buffer)
    std::atomic<uint32_t> queueHighWater{0};   // Max queue depth seen
    std::atomic<uint32_t> parseSuccesses{0};   // Successfully parsed packets
    std::atomic<uint32_t> parseFailures{0};    // Parse failures (resync)
    
    // Connection
    std::atomic<uint32_t> reconnects{0};       // BLE reconnection count
    std::atomic<uint32_t> disconnects{0};      // BLE disconnection count
    
    // Display
    std::atomic<uint32_t> displayUpdates{0};   // Frames drawn
    std::atomic<uint32_t> displaySkips{0};     // Updates skipped (throttled)
    
    // Timing (microseconds for precision)
    std::atomic<uint32_t> lastNotifyUs{0};     // Timestamp of last notify
    std::atomic<uint32_t> lastFlushUs{0};      // Timestamp of last flush
    
    void reset() {
        rxPackets.store(0, std::memory_order_relaxed);
        rxBytes.store(0, std::memory_order_relaxed);
        queueDrops.store(0, std::memory_order_relaxed);
        oversizeDrops.store(0, std::memory_order_relaxed);
        queueHighWater.store(0, std::memory_order_relaxed);
        parseSuccesses.store(0, std::memory_order_relaxed);
        parseFailures.store(0, std::memory_order_relaxed);
        reconnects.store(0, std::memory_order_relaxed);
        disconnects.store(0, std::memory_order_relaxed);
        displayUpdates.store(0, std::memory_order_relaxed);
        displaySkips.store(0, std::memory_order_relaxed);
    }
};

// ============================================================================
// Sampled latency tracking (only when PERF_METRICS=1)
// Uses std::atomic for thread-safe access
// ============================================================================
struct PerfLatency {
    // BLE→Flush latency (microseconds)
    std::atomic<uint32_t> minUs{UINT32_MAX};
    std::atomic<uint32_t> maxUs{0};
    std::atomic<uint64_t> totalUs{0};
    std::atomic<uint32_t> sampleCount{0};
    
    // Per-stage breakdown (for debugging bottlenecks)
    std::atomic<uint32_t> notifyToQueueUs{0};    // notify callback → queue send
    std::atomic<uint32_t> queueToParseUs{0};     // queue receive → parse done
    std::atomic<uint32_t> parseToFlushUs{0};     // parse done → display flush
    
    void reset() {
        minUs.store(UINT32_MAX, std::memory_order_relaxed);
        maxUs.store(0, std::memory_order_relaxed);
        totalUs.store(0, std::memory_order_relaxed);
        sampleCount.store(0, std::memory_order_relaxed);
        notifyToQueueUs.store(0, std::memory_order_relaxed);
        queueToParseUs.store(0, std::memory_order_relaxed);
        parseToFlushUs.store(0, std::memory_order_relaxed);
    }
    
    uint32_t avgUs() const {
        uint32_t count = sampleCount.load(std::memory_order_relaxed);
        return count > 0 ? static_cast<uint32_t>(totalUs.load(std::memory_order_relaxed) / count) : 0;
    }
};

// ============================================================================
// Global instances
// ============================================================================
extern PerfCounters perfCounters;

#if PERF_METRICS
extern PerfLatency perfLatency;
#endif

#if PERF_METRICS && PERF_MONITORING
extern bool perfDebugEnabled;        // Runtime debug print enable
extern uint32_t perfLastReportMs;    // Last report timestamp
#endif

// ============================================================================
// Inline instrumentation macros (zero cost when disabled)
// ============================================================================

// Always-on counter increments
#define PERF_INC(counter) (perfCounters.counter++)
#define PERF_ADD(counter, value) (perfCounters.counter += (value))
#define PERF_SET(counter, value) (perfCounters.counter = (value))
#define PERF_MAX(counter, value) do { \
    if ((value) > perfCounters.counter) perfCounters.counter = (value); \
} while(0)

// Timestamp capture (always on, but cheap)
#define PERF_TIMESTAMP_US() ((uint32_t)esp_timer_get_time())

#if PERF_METRICS && PERF_MONITORING

// Sampled latency recording
#define PERF_SAMPLE_LATENCY(startUs, endUs) do { \
    static uint32_t _sampleCounter = 0; \
    if ((++_sampleCounter & (PERF_SAMPLE_RATE - 1)) == 0) { \
        uint32_t _lat = (endUs) - (startUs); \
        if (_lat < perfLatency.minUs) perfLatency.minUs = _lat; \
        if (_lat > perfLatency.maxUs) perfLatency.maxUs = _lat; \
        perfLatency.totalUs += _lat; \
        perfLatency.sampleCount++; \
    } \
} while(0)

// Stage timing (for debugging)
#if PERF_VERBOSE
#define PERF_STAGE_TIME(stage, value) (perfLatency.stage = (value))
#else
#define PERF_STAGE_TIME(stage, value) ((void)0)
#endif

// Threshold alert (immediate print if exceeded)
#if PERF_VERBOSE
#define PERF_ALERT_IF_SLOW(latencyUs) do { \
    if (perfDebugEnabled && (latencyUs) > (PERF_LATENCY_ALERT_MS * 1000)) { \
        Serial.printf("[PERF ALERT] latency=%luus\n", (unsigned long)(latencyUs)); \
    } \
} while(0)
#else
#define PERF_ALERT_IF_SLOW(latencyUs) ((void)0)
#endif

#else  // PERF_METRICS == 0 or PERF_MONITORING == 0

#define PERF_SAMPLE_LATENCY(startUs, endUs) ((void)0)
#define PERF_STAGE_TIME(stage, value) ((void)0)
#define PERF_ALERT_IF_SLOW(latencyUs) ((void)0)

#endif  // PERF_METRICS && PERF_MONITORING

// ============================================================================
// API functions
// ============================================================================

// Initialize metrics system
void perfMetricsInit();

// Reset all metrics
void perfMetricsReset();

// Check if periodic report is due (call from loop)
// Returns true if report was printed
bool perfMetricsCheckReport();

// Force immediate report
void perfMetricsPrint();

// Get JSON summary for web API
String perfMetricsToJson();

// Enable/disable debug prints at runtime
void perfMetricsSetDebug(bool enabled);

#endif // PERF_METRICS_H

/**
 * Performance A/B Test Flags
 * 
 * Enable ONE flag at a time to isolate the cause of ENQ→DEQ queue-wait spikes.
 * Run each configuration for 2 minutes with the same alert scenario.
 * 
 * Usage: Uncomment ONE flag at a time, build, flash, run test, record metrics.
 */

#ifndef PERF_TEST_FLAGS_H
#define PERF_TEST_FLAGS_H

// ============================================================================
// A/B TEST FLAGS - Uncomment ONE at a time to test
// ============================================================================

// Test 1: BASELINE - all systems enabled (comment all flags)

// Test 2: Disable WiFi/WebServer processing in loop
// #define PERF_TEST_DISABLE_WIFI

// Test 3: Disable touch handler polling (skip getTouchPoint calls)
// #define PERF_TEST_DISABLE_TOUCH

// Test 4: Disable all Serial/SD logging in hot path
// #define PERF_TEST_DISABLE_LOGGING

// Test 5: Disable display throttle (always draw, no DISPLAY_SKIP)
// #define PERF_TEST_DISABLE_THROTTLE

// Test 6: Disable battery manager updates in loop
// #define PERF_TEST_DISABLE_BATTERY

// Test 7: Move queue drain to high-priority position (before all other work)
// #define PERF_TEST_EARLY_DRAIN

// Test 8: Disable BLE proxy forwarding
// #define PERF_TEST_DISABLE_PROXY

// ============================================================================
// PERCENTILE TRACKING for precise latency analysis
// ============================================================================
#define PERF_TEST_PERCENTILE_TRACKING 1

#ifdef PERF_TEST_PERCENTILE_TRACKING
#include <algorithm>

// Ring buffer to collect latency samples for percentile calculation
#define LATENCY_SAMPLE_SIZE 500  // ~10 seconds at 50Hz

struct LatencyHistogram {
    int64_t samples[LATENCY_SAMPLE_SIZE];
    uint32_t count;
    uint32_t writeIdx;
    
    void reset() {
        count = 0;
        writeIdx = 0;
    }
    
    void add(int64_t value_us) {
        samples[writeIdx] = value_us;
        writeIdx = (writeIdx + 1) % LATENCY_SAMPLE_SIZE;
        if (count < LATENCY_SAMPLE_SIZE) count++;
    }
    
    // Get percentile (0-100) in microseconds
    int64_t percentile(int pct) {
        if (count == 0) return 0;
        
        // Copy and sort
        int64_t sorted[LATENCY_SAMPLE_SIZE];
        uint32_t n = count;
        for (uint32_t i = 0; i < n; i++) {
            sorted[i] = samples[i];
        }
        std::sort(sorted, sorted + n);
        
        uint32_t idx = (pct * n) / 100;
        if (idx >= n) idx = n - 1;
        return sorted[idx];
    }
    
    int64_t p50() { return percentile(50); }
    int64_t p95() { return percentile(95); }
    int64_t p99() { return percentile(99); }
    int64_t max() { return percentile(100); }
};

#endif // PERF_TEST_PERCENTILE_TRACKING

#endif // PERF_TEST_FLAGS_H

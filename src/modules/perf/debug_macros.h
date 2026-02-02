#pragma once

/**
 * Debug Macros - Compile-time debug logging switches and performance timing utilities.
 * 
 * These macros gate verbose logging behind constexpr flags that compile to zero-cost
 * when disabled. Include this header in any module that needs debug logging.
 */

#include <Arduino.h>

// Use Serial directly (SerialLog alias was removed)
#define SerialLog Serial

// -----------------------------------------------------------------------------
// Debug log switches (set to true to enable, false for production)
// -----------------------------------------------------------------------------
static constexpr bool DEBUG_LOGS = false;          // General debug logging (packet dumps, status)
static constexpr bool PERF_TIMING_LOGS = false;    // Hot path timing measurements (enable for bench testing)
static constexpr bool AUTOPUSH_DEBUG_LOGS = false; // Auto-push verbose logs

// -----------------------------------------------------------------------------
// Debug logging macros - compile to nothing when flags are false
// -----------------------------------------------------------------------------
#define DEBUG_LOGF(...) do { if (DEBUG_LOGS) SerialLog.printf(__VA_ARGS__); } while (0)
#define DEBUG_LOGLN(msg) do { if (DEBUG_LOGS) SerialLog.println(msg); } while (0)
#define AUTO_PUSH_LOGF(...) do { if (AUTOPUSH_DEBUG_LOGS) SerialLog.printf(__VA_ARGS__); } while (0)
#define AUTO_PUSH_LOGLN(msg) do { if (AUTOPUSH_DEBUG_LOGS) SerialLog.println(msg); } while (0)

// -----------------------------------------------------------------------------
// Performance timing infrastructure
// These globals accumulate timing data for periodic reporting
// -----------------------------------------------------------------------------
inline unsigned long perfTimingAccum = 0;
inline unsigned long perfTimingCount = 0;
inline unsigned long perfTimingMax = 0;
inline unsigned long perfLastReport = 0;

// Display latency tracking for SD logging
inline unsigned long displayLatencySum = 0;
inline unsigned long displayLatencyCount = 0;
inline unsigned long displayLatencyMax = 0;
inline unsigned long displayLatencyLastLog = 0;
inline constexpr unsigned long DISPLAY_LOG_INTERVAL_MS = 10000;  // Log summary every 10s
inline constexpr unsigned long DISPLAY_SLOW_THRESHOLD_US = 16000; // 16ms = 60fps budget

// Forward declare debugLogger for V1_DISPLAY_END macro
class DebugLogger;
extern DebugLogger debugLogger;

// -----------------------------------------------------------------------------
// Performance timing macros
// V1_PERF_START/END: measure and report hot-path durations (console only)
// V1_DISPLAY_END: measure display latency and log to SD when enabled
// -----------------------------------------------------------------------------
#define V1_PERF_START() unsigned long _perfStart = micros()

#define V1_PERF_END(label) do { \
    if (PERF_TIMING_LOGS) { \
        unsigned long _perfDur = micros() - _perfStart; \
        perfTimingAccum += _perfDur; \
        perfTimingCount++; \
        if (_perfDur > perfTimingMax) perfTimingMax = _perfDur; \
        if (millis() - perfLastReport > 5000) { \
            SerialLog.printf("[PERF] %s: avg=%luus max=%luus (n=%lu)\n", \
                label, perfTimingAccum/perfTimingCount, perfTimingMax, perfTimingCount); \
            perfTimingAccum = 0; perfTimingCount = 0; perfTimingMax = 0; \
            perfLastReport = millis(); \
        } \
    } \
} while(0)

// Display latency macro - includes SD logging when debug logger enabled
// Note: Requires debug_logger.h to be included and debugLogger to be available
// Rate-limited SLOW logs to max 1/sec to prevent spam during stalls
#define V1_DISPLAY_END_WITH_LOGGER(label, logger) do { \
    static unsigned long _lastSlowLogMs = 0; \
    unsigned long _dur = micros() - _perfStart; \
    displayLatencySum += _dur; \
    displayLatencyCount++; \
    if (_dur > displayLatencyMax) displayLatencyMax = _dur; \
    unsigned long _nowSlowChk = millis(); \
    if (_dur > DISPLAY_SLOW_THRESHOLD_US && (logger).isEnabledFor(DebugLogCategory::Display) && (_nowSlowChk - _lastSlowLogMs >= 1000)) { \
        (logger).logf(DebugLogCategory::Display, "[SLOW] %s: %lums", label, _dur / 1000); \
        _lastSlowLogMs = _nowSlowChk; \
    } \
    unsigned long _now = millis(); \
    if ((_now - displayLatencyLastLog) > DISPLAY_LOG_INTERVAL_MS && displayLatencyCount > 0) { \
        if ((logger).isEnabledFor(DebugLogCategory::Display)) { \
            (logger).logf(DebugLogCategory::Display, "Display: avg=%luus max=%luus n=%lu", \
                displayLatencySum / displayLatencyCount, displayLatencyMax, displayLatencyCount); \
        } \
        displayLatencySum = 0; displayLatencyCount = 0; displayLatencyMax = 0; \
        displayLatencyLastLog = _now; \
    } \
    if (PERF_TIMING_LOGS) { \
        perfTimingAccum += _dur; \
        perfTimingCount++; \
        if (_dur > perfTimingMax) perfTimingMax = _dur; \
        if (_now - perfLastReport > 5000) { \
            SerialLog.printf("[PERF] %s: avg=%luus max=%luus (n=%lu)\n", \
                label, perfTimingAccum/perfTimingCount, perfTimingMax, perfTimingCount); \
            perfTimingAccum = 0; perfTimingCount = 0; perfTimingMax = 0; \
            perfLastReport = _now; \
        } \
    } \
} while(0)

// Simplified version that uses the global debugLogger
#define V1_DISPLAY_END(label) V1_DISPLAY_END_WITH_LOGGER(label, debugLogger)

// display_flush.h — Shared DISPLAY_FLUSH macro for display_*.cpp files
// Wraps tft->flush() with perf instrumentation on Arduino_GFX builds.
#ifndef DISPLAY_FLUSH_H
#define DISPLAY_FLUSH_H

#include "perf_metrics.h"

#if defined(DISPLAY_USE_ARDUINO_GFX)
#define DISPLAY_FLUSH() do { \
    if (tft) { \
        uint32_t _start = PERF_TIMESTAMP_US(); \
        tft->flush(); \
        perfRecordFlushUs(PERF_TIMESTAMP_US() - _start); \
    } \
} while(0)
#else
#define DISPLAY_FLUSH() ((void)0)
#endif

#endif // DISPLAY_FLUSH_H

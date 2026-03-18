// display_flush.h — Shared DISPLAY_FLUSH macro for display_*.cpp files
// Wraps tft->flush() with perf instrumentation on Arduino_GFX builds.
#ifndef DISPLAY_FLUSH_H
#define DISPLAY_FLUSH_H

#include "perf_metrics.h"

#define DISPLAY_FLUSH() do { \
    if (tft) { \
        uint32_t _start = PERF_TIMESTAMP_US(); \
        uint32_t _areaPx = static_cast<uint32_t>(tft->width()) * static_cast<uint32_t>(tft->height()); \
        tft->flush(); \
        perfRecordFlushUs(PERF_TIMESTAMP_US() - _start, _areaPx, true); \
    } \
} while(0)

#endif // DISPLAY_FLUSH_H

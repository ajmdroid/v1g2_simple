/**
 * Display logging macro — shared across display_*.cpp files.
 *
 * Logs to Serial (when verbose flag is true) AND the debug logger under the
 * Display category.  Extracted from display.cpp to avoid duplication in split
 * translation units.
 */

#ifndef DISPLAY_LOG_H
#define DISPLAY_LOG_H

#include "debug_logger.h"

static constexpr bool DISPLAY_DEBUG_LOGS = false;  // Set true for verbose Serial logging

#if defined(DISABLE_DEBUG_LOGGER)
#define DISPLAY_LOG(...) do { } while(0)
#else
#define DISPLAY_LOG(...) do { \
    if (DISPLAY_DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    DBG_LOGF(DebugLogCategory::Display, __VA_ARGS__); \
} while(0)
#endif

#endif // DISPLAY_LOG_H

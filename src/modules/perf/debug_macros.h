#pragma once

/**
 * Debug Macros - Serial alias and compile-time debug switches.
 */

#include <Arduino.h>

// Serial alias for consistent logging
#define SerialLog Serial

// Debug log switches (set to true to enable verbose output)
static constexpr bool DEBUG_LOGS = false;
static constexpr bool AUTOPUSH_DEBUG_LOGS = false;

// Debug logging macros - compile to nothing when flags are false
#define DEBUG_LOGF(...) do { if (DEBUG_LOGS) SerialLog.printf(__VA_ARGS__); } while (0)
#define DEBUG_LOGLN(msg) do { if (DEBUG_LOGS) SerialLog.println(msg); } while (0)
#define AUTO_PUSH_LOGF(...) do { if (AUTOPUSH_DEBUG_LOGS) SerialLog.printf(__VA_ARGS__); } while (0)
#define AUTO_PUSH_LOGLN(msg) do { if (AUTOPUSH_DEBUG_LOGS) SerialLog.println(msg); } while (0)

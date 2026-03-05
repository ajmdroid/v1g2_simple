#pragma once

/**
 * Debug Macros - Serial alias and compile-time debug switches.
 */

#include <Arduino.h>

// Serial alias for consistent logging
#define SerialLog Serial

// Debug logging macros — permanent no-ops
#define DEBUG_LOGF(...) do { } while (0)
#define DEBUG_LOGLN(msg) do { } while (0)
#define AUTO_PUSH_LOGF(...) do { } while (0)
#define AUTO_PUSH_LOGLN(msg) do { } while (0)

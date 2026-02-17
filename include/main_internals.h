/**
 * Main internals — shared across main.cpp TU split.
 *
 * Provides: declarations for boot helpers and persistence helpers
 * extracted from main.cpp.  Each companion .cpp includes this header.
 */

#ifndef MAIN_INTERNALS_H
#define MAIN_INTERNALS_H

#include <cstdint>
#include "esp_system.h"      // esp_reset_reason_t
#include <ArduinoJson.h>     // JsonDocument (for normalizeLegacyLockoutRadiusScale)

// ---- Boot helper declarations (main_boot.cpp) ----

/// Map ESP reset reason enum to human-readable string.
const char* resetReasonToString(esp_reset_reason_t reason);

/// Normalize legacy lockout zone radiusE5 values from 10x scale to 1x.
/// Returns number of zones migrated.
uint32_t normalizeLegacyLockoutRadiusScale(JsonDocument& doc);

/// Log crash recovery breadcrumbs (heap stats, coredump) to Serial + LittleFS.
void logPanicBreadcrumbs();

/// Check NVS health and attempt cleanup if >80% full.
void nvsHealthCheck();

/// Increment and return persistent boot ID counter.
uint32_t nextBootId();

/// Show fatal error on display (if available), wait, then restart.
void fatalBootError(const char* message, bool displayAvailable);

// ---- Persistence helper declarations (main_persist.cpp) ----

/// Periodic best-effort save of lockout zones to SD/LittleFS (Tier 7).
void processLockoutStoreSave(uint32_t nowMs);

/// Periodic best-effort save of learner pending candidates (Tier 7).
void processLearnerPendingSave(uint32_t nowMs);

#endif // MAIN_INTERNALS_H

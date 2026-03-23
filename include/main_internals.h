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

class QuietCoordinatorModule;

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

// ---- Setup orchestration helper declarations (main_setup_helpers.cpp) ----

/// Callback invoked immediately when BLE subscribe completes.
void onV1ConnectImmediate();

/// Callback invoked once the BLE connect burst has settled.
void onV1Connected();

/// Mount storage + initialize profile/device stores and restore dependent settings.
void initializeStorageAndProfiles();

/// Prepare persistence/runtime services for a power-off sequence before the final hardware tail runs.
void prepareForShutdown(void* context);

/// Apply persisted lockout policy and hydrate lockout zones from storage.
void applyLockoutPolicyAndLoadZonesFromStorage();

/// Initialize perf/observation CSV loggers and return the boot session id.
uint32_t initializeBootPerformanceLoggers();

/// Restore persisted learner pending candidates from storage.
void restorePendingLearnerCandidates();

/// Initialize touch hardware and apply persisted display/audio controls.
void initializeTouchAndDisplayControls();

/// Configure auto-push + touch interaction modules after storage/BLE setup.
void configureUiInteractionModules(QuietCoordinatorModule& quietCoordinator);

/// Emit boot summary and WiFi startup policy logs.
void logBootSummaryAndWifiStartup(uint32_t bootId, esp_reset_reason_t resetReason);

/// Early setup diagnostics: serial settle, GPIO hold release, panic/NVS checks.
void initializeEarlyBootDiagnostics();

// ---- Persistence helper declarations (main_persist.cpp) ----

/// Periodic best-effort save of lockout zones to SD/LittleFS (Tier 7).
void processLockoutStoreSave(uint32_t nowMs);

/// Periodic best-effort save of learner pending candidates (Tier 7).
void processLearnerPendingSave(uint32_t nowMs);

/// Periodic best-effort save of deferred V1 device-store updates (Tier 7).
void processV1DeviceStoreSave(uint32_t nowMs);

#endif // MAIN_INTERNALS_H

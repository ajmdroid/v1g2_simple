/**
 * WiFi Manager internals — shared across wifi_manager TU split.
 *
 * Provides: extern declarations, WIFI_LOG macro, promoted helper declarations.
 * Each .cpp includes this plus its own module-specific headers.
 */

#ifndef WIFI_MANAGER_INTERNALS_H
#define WIFI_MANAGER_INTERNALS_H

#include "wifi_manager.h"
#include "debug_logger.h"
#include "ble_client.h"
#include "modules/system/system_event_bus.h"

class DisplayPipelineModule;

// ---- External globals used across wifi_manager TU split ----
extern V1BLEClient bleClient;
extern SystemEventBus systemEventBus;
extern DisplayPipelineModule displayPipelineModule;

// Preview helpers for display demo flows (color).
extern void requestColorPreviewHold(uint32_t durationMs);
extern bool isDisplayPreviewRunning();
extern bool isColorPreviewRunning();
extern void cancelDisplayPreview();
extern void cancelColorPreview();

// ---- Debug / logging infrastructure ----
static constexpr bool WIFI_DEBUG_LOGS = false;  // Set true for verbose Serial logging
static constexpr bool WIFI_DEBUG_FS_DUMP = false; // Set true to dump LittleFS root on WiFi start

#if defined(DISABLE_DEBUG_LOGGER)
#define WIFI_LOG(...) do { } while(0)
#else
#define WIFI_LOG(...) do { \
    if (WIFI_DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    DBG_LOGF(DebugLogCategory::Wifi, __VA_ARGS__); \
} while(0)
#endif

// ---- Promoted helper declarations ----

/// Map WifiClientState enum to API-facing string name.
const char* wifiClientStateApiName(WifiClientState state);

/// Serve a file from LittleFS with ETag/gzip support.
bool serveLittleFSFileHelper(WebServer& server, const char* path, const char* contentType);

/// Dump LittleFS root directory listing (debug diagnostic).
void dumpLittleFSRoot();

#endif // WIFI_MANAGER_INTERNALS_H

/**
 * WiFi Manager internals — shared across wifi_manager TU split.
 *
 * Provides: extern declarations, WIFI_LOG macro, promoted helper declarations.
 * Each .cpp includes this plus its own module-specific headers.
 */

#pragma once

#include "wifi_manager.h"
#include "ble_client.h"
#include "modules/system/system_event_bus.h"

// --- External globals used across wifi_manager TU split ---
extern V1BLEClient bleClient;
extern SystemEventBus systemEventBus;

// --- Debug / logging infrastructure ---
static constexpr bool WIFI_DEBUG_LOGS = false;  // Set true for verbose Serial logging
static constexpr bool WIFI_DEBUG_FS_DUMP = false; // Set true to dump LittleFS root on WiFi start

#define WIFI_LOG(...) do { } while(0)

// --- Promoted helper declarations ---

/// Map WifiClientState enum to API-facing string name.
const char* wifiClientStateApiName(WifiClientState state);

/// Serve a file from LittleFS with ETag/gzip support.
bool serveLittleFSFileHelper(WebServer& server, const char* path, const char* contentType);

/// Dump LittleFS root directory listing (debug diagnostic).
void dumpLittleFSRoot();


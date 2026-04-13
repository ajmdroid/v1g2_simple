#pragma once

#include <WebServer.h>

// Forward declarations for begin() dependencies.
class V1BLEClient;
class WiFiManager;

/**
 * OTA API Service — namespace-based service following the DebugApiService
 * pattern. Provides HTTP endpoints for version checking, update status,
 * and triggering firmware/filesystem OTA from GitHub Releases.
 *
 * Dependencies are wired once via begin(). Handlers are registered by
 * WiFiManager::setupWebServer() in wifi_routes.cpp.
 *
 * All state (cached version check, download progress, error info) is
 * file-scoped in ota_api_service.cpp.
 */
namespace OtaApiService {

/// Wire dependencies. Called once during setup() before any handlers fire.
/// All pointers must remain valid for the lifetime of the service.
///
/// pumpServer/pumpServerCtx: optional callback to service the WebServer
/// during long-running downloads, enabling cancel requests to arrive.
void begin(V1BLEClient* ble, WiFiManager* wifi,
           void (*pumpServer)(void* ctx) = nullptr, void* pumpServerCtx = nullptr);

/// GET /api/version — lightweight version endpoint.
void handleApiVersion(WebServer& server);

/// GET /api/ota/status — full OTA subsystem state.
void handleApiOtaStatus(WebServer& server);

/// POST /api/ota/check — trigger a version check against GitHub.
void handleApiOtaCheck(WebServer& server,
                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

/// POST /api/ota/start — begin the OTA update process.
/// Disconnects BLE, downloads from GitHub, flashes, and restarts.
void handleApiOtaStart(WebServer& server,
                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

/// POST /api/ota/cancel — abort an in-progress download.
/// Only effective during the download phase (not after flash write begins).
void handleApiOtaCancel(WebServer& server);

/// Called from loop() to drive non-blocking OTA state machine.
/// Returns immediately when idle. During an active update, drives
/// the download-verify-flash-restart sequence.
void process(uint32_t nowMs);

}  // namespace OtaApiService

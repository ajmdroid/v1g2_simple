/**
 * WiFi Routes — setupWebServer() route registrations.
 * Extracted from wifi_manager.cpp for maintainability.
 */

#include "wifi_manager_internals.h"
#include "perf_metrics.h"
#include "settings.h"
#include "storage_manager.h"
#include "obd_handler.h"
#include "modules/obd/obd_api_service.h"
#include "modules/gps/gps_api_service.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_lockout_safety.h"
#include "modules/gps/gps_observation_log.h"
#include "modules/camera/camera_api_service.h"
#include "modules/camera/camera_runtime_module.h"
#include "modules/lockout/lockout_api_service.h"
#include "modules/lockout/lockout_index.h"
#include "modules/lockout/lockout_learner.h"
#include "modules/debug/debug_api_service.h"
#include "modules/wifi/backup_api_service.h"
#include "modules/wifi/wifi_client_api_service.h"
#include "modules/wifi/wifi_control_api_service.h"
#include "modules/wifi/wifi_display_colors_api_service.h"
#include "modules/wifi/wifi_portal_api_service.h"
#include "modules/wifi/wifi_settings_api_service.h"
#include "modules/wifi/wifi_status_api_service.h"
#include "modules/wifi/wifi_time_api_service.h"
#include "modules/wifi/wifi_autopush_api_service.h"
#include "modules/wifi/wifi_v1_profile_api_service.h"
#include "modules/lockout/lockout_store.h"
#include "modules/lockout/signal_observation_log.h"
#include "modules/lockout/signal_observation_sd_logger.h"
#include "modules/speed/speed_source_selector.h"
#include "time_service.h"
#include "battery_manager.h"
#include <LittleFS.h>

void WiFiManager::setupWebServer() {
    // Initialize LittleFS for serving web UI files
    if (!LittleFS.begin(false)) {
        WIFI_LOG("[SetupMode] ERROR: LittleFS mount failed (not formatting automatically)\n");
        return;
    }
    WIFI_LOG("[SetupMode] LittleFS mounted\n");
    // Dump LittleFS root for diagnostics (opt-in to avoid startup stall)
    if (WIFI_DEBUG_FS_DUMP) {
        dumpLittleFSRoot();
    }
    
    // New UI served from LittleFS
    // Redirect /ui to root for backward compatibility
    server.on("/ui", HTTP_GET, [this]() { 
        WifiPortalApiService::handleApiRedirectToRoot(server);
    });
    
    // Serve static assets from _app directory
    server.on("/_app/env.js", HTTP_GET, [this]() { serveLittleFSFile("/_app/env.js", "application/javascript"); });
    server.on("/_app/version.json", HTTP_GET, [this]() { serveLittleFSFile("/_app/version.json", "application/json"); });
    
    // Root serves /index.html (Svelte app)
    server.on("/", HTTP_GET, [this]() { 
        markUiActivity();  // Track UI activity
        if (serveLittleFSFile("/index.html", "text/html")) {
            Serial.printf("[HTTP] 200 / -> /index.html\n");
            return;
        }
        // LittleFS missing - tell user to reflash
        Serial.println("[HTTP] 500 / -> LittleFS missing");
        server.send(500, "text/plain", "Web UI not found. Please reflash with ./build.sh --all");
    });
    
    // Catch-all for _app/immutable/* files (if Svelte files are uploaded)
    server.onNotFound([this]() {
        markUiActivity();  // Track UI activity
        String uri = server.uri();
        
        // Serve _app files from LittleFS
        if (uri.startsWith("/_app/")) {
            String contentType = "application/octet-stream";
            if (uri.endsWith(".js")) contentType = "application/javascript";
            else if (uri.endsWith(".css")) contentType = "text/css";
            else if (uri.endsWith(".json")) contentType = "application/json";
            
            if (serveLittleFSFile(uri.c_str(), contentType.c_str())) {
                return;
            }
        }
        
        // Fall through to original not found handler
        handleNotFound();
    });
    
    auto rateLimitCallback = [this]() { return checkRateLimit(); };
    auto markUiActivityCallback = [this]() { markUiActivity(); };
    // New API endpoints (PHASE A)
    server.on("/api/status", HTTP_GET, [this, rateLimitCallback]() {
        WifiStatusApiService::handleApiStatus(
            server,
            makeStatusRuntime(),
            cachedStatusJson,
            lastStatusJsonTime,
            STATUS_CACHE_TTL_MS,
            []() { return millis(); },
            rateLimitCallback);
    });
    server.on("/api/profile/push", HTTP_POST, [this, rateLimitCallback]() { 
        WifiControlApiService::handleApiProfilePush(
            server,
            bleClient.isConnected(),
            requestProfilePush,
            rateLimitCallback); 
    });
    server.on("/api/time/set", HTTP_POST, [this, rateLimitCallback]() {
        WifiTimeApiService::handleApiTimeSet(
            server,
            makeTimeRuntime(),
            TimeService::SOURCE_CLIENT_AP,
            [this]() { lastStatusJsonTime = 0; },
            rateLimitCallback);
    });
    
    // Legacy status endpoint
    server.on("/status", HTTP_GET, [this]() {
        WifiStatusApiService::handleApiLegacyStatus(
            server,
            makeStatusRuntime(),
            cachedStatusJson,
            lastStatusJsonTime,
            STATUS_CACHE_TTL_MS,
            []() { return millis(); });
    });
    server.on("/api/settings", HTTP_GET, [this]() {
        WifiSettingsApiService::handleApiSettingsGet(server, makeSettingsRuntime());
    });  // JSON settings for new UI
    server.on("/api/settings", HTTP_POST, [this, rateLimitCallback]() {
        WifiSettingsApiService::handleApiSettingsSave(
            server,
            makeSettingsRuntime(),
            rateLimitCallback);
    });  // Consistent API endpoint
    
    // Legacy HTML page routes - redirect to root (SvelteKit handles routing)
    server.on("/settings", HTTP_GET, [this]() { 
        WifiPortalApiService::handleApiDeprecatedRedirectToRoot(
            server,
            "Use /api/settings");
    });
    server.on("/settings", HTTP_POST, [this, rateLimitCallback]() {
        WifiSettingsApiService::handleApiLegacySettingsSave(
            server,
            makeSettingsRuntime(),
            rateLimitCallback,
            [this]() { server.sendHeader("X-API-Deprecated", "Use /api/settings"); },
            []() { Serial.println("[HTTP] WARN: Legacy POST /settings used; prefer /api/settings"); });
    });  // Legacy compat
    server.on("/darkmode", HTTP_POST, [this, rateLimitCallback]() {
        WifiControlApiService::handleApiDarkMode(
            server,
            sendV1Command,
            rateLimitCallback);
    });
    server.on("/mute", HTTP_POST, [this, rateLimitCallback]() {
        WifiControlApiService::handleApiMute(
            server,
            sendV1Command,
            rateLimitCallback);
    });
    
    // Lightweight health and captive-portal helpers
    server.on("/ping", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiPing(
            server,
            [this]() { markUiActivity(); });
    });
    // Android/ChromeOS captive portal probes
    server.on("/generate_204", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiGenerate204(
            server,
            [this]() { markUiActivity(); });
    });
    server.on("/gen_204", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiGen204(
            server,
            [this]() { markUiActivity(); });
    });
    // iOS/macOS captive portal
    server.on("/hotspot-detect.html", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiHotspotDetect(
            server,
            [this]() { markUiActivity(); });
    });
    // Windows captive portal variants
    server.on("/fwlink", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiFwlink(server);
    });
    server.on("/ncsi.txt", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiNcsiTxt(server);
    });
    
    // V1 Settings/Profiles routes
    server.on("/v1settings", HTTP_GET, [this]() { 
        WifiPortalApiService::handleApiRedirectToRoot(server);
    });
    server.on("/api/v1/profiles", HTTP_GET, [this]() {
        WifiV1ProfileApiService::handleApiProfilesList(server, makeV1ProfileRuntime());
    });
    server.on("/api/v1/profile", HTTP_GET, [this]() {
        WifiV1ProfileApiService::handleApiProfileGet(server, makeV1ProfileRuntime());
    });
    server.on("/api/v1/profile", HTTP_POST, [this, rateLimitCallback]() {
        WifiV1ProfileApiService::handleApiProfileSave(
            server,
            makeV1ProfileRuntime(),
            rateLimitCallback);
    });
    server.on("/api/v1/profile/delete", HTTP_POST, [this, rateLimitCallback]() {
        WifiV1ProfileApiService::handleApiProfileDelete(
            server,
            makeV1ProfileRuntime(),
            rateLimitCallback);
    });
    server.on("/api/v1/pull", HTTP_POST, [this, rateLimitCallback]() {
        WifiV1ProfileApiService::handleApiSettingsPull(
            server,
            makeV1ProfileRuntime(),
            rateLimitCallback);
    });
    server.on("/api/v1/push", HTTP_POST, [this, rateLimitCallback]() {
        WifiV1ProfileApiService::handleApiSettingsPush(
            server,
            makeV1ProfileRuntime(),
            rateLimitCallback);
    });
    server.on("/api/v1/current", HTTP_GET, [this]() {
        WifiV1ProfileApiService::handleApiCurrentSettings(server, makeV1ProfileRuntime());
    });
    
    // Auto-Push routes
    server.on("/autopush", HTTP_GET, [this]() { 
        WifiPortalApiService::handleApiRedirectToRoot(server);
    });
    server.on("/api/autopush/slots", HTTP_GET, [this]() {
        WifiAutoPushApiService::handleApiSlots(server, makeAutoPushRuntime());
    });
    server.on("/api/autopush/slot", HTTP_POST, [this, rateLimitCallback]() {
        WifiAutoPushApiService::handleApiSlotSave(
            server,
            makeAutoPushRuntime(),
            rateLimitCallback);
    });
    server.on("/api/autopush/activate", HTTP_POST, [this, rateLimitCallback]() {
        WifiAutoPushApiService::handleApiActivate(
            server,
            makeAutoPushRuntime(),
            rateLimitCallback);
    });
    server.on("/api/autopush/push", HTTP_POST, [this, rateLimitCallback]() {
        WifiAutoPushApiService::handleApiPushNow(
            server,
            makeAutoPushRuntime(),
            rateLimitCallback);
    });
    server.on("/api/autopush/status", HTTP_GET, [this]() {
        WifiAutoPushApiService::handleApiStatus(server, makeAutoPushRuntime());
    });
    
    // Display Colors routes
    server.on("/displaycolors", HTTP_GET, [this]() { 
        WifiPortalApiService::handleApiRedirectToRoot(server);
    });
    server.on("/api/displaycolors", HTTP_GET, [this]() {
        WifiDisplayColorsApiService::handleApiGet(server, makeDisplayColorsRuntime());
    });
    server.on("/api/displaycolors", HTTP_POST, [this, rateLimitCallback]() {
        WifiDisplayColorsApiService::handleApiSave(
            server,
            makeDisplayColorsRuntime(),
            rateLimitCallback);
    });
    server.on("/api/displaycolors/reset", HTTP_POST, [this, rateLimitCallback]() {
        WifiDisplayColorsApiService::handleApiReset(
            server,
            makeDisplayColorsRuntime(),
            rateLimitCallback);
    });
    server.on("/api/displaycolors/preview", HTTP_POST, [this, rateLimitCallback]() { 
        WifiDisplayColorsApiService::handleApiPreview(
            server,
            makeDisplayColorsRuntime(),
            rateLimitCallback);
    });
    server.on("/api/displaycolors/clear", HTTP_POST, [this, rateLimitCallback]() { 
        WifiDisplayColorsApiService::handleApiClear(
            server,
            makeDisplayColorsRuntime(),
            rateLimitCallback);
    });
    
    // Settings backup/restore API routes
    server.on("/api/settings/backup", HTTP_GET, [this]() {
        BackupApiService::handleApiBackup(
            server,
            [this]() { markUiActivity(); });
    });
    server.on("/api/settings/restore", HTTP_POST, [this, rateLimitCallback]() {
        BackupApiService::handleApiRestore(
            server,
            rateLimitCallback,
            [this]() { markUiActivity(); });
    });
    
    // Debug API routes (performance metrics)
    server.on("/api/debug/metrics", HTTP_GET, [this]() {
        DebugApiService::handleApiMetrics(server);
    });
    server.on("/api/debug/panic", HTTP_GET, [this]() {
        DebugApiService::handleApiPanic(server);
    });
    server.on("/api/debug/enable", HTTP_POST, [this, rateLimitCallback]() {
        DebugApiService::handleApiDebugEnable(
            server,
            rateLimitCallback);
    });
    server.on("/api/debug/perf-files", HTTP_GET, [this, rateLimitCallback]() {
        DebugApiService::handleApiPerfFilesList(
            server,
            rateLimitCallback,
            [this]() { markUiActivity(); });
    });
    server.on("/api/debug/perf-files/download", HTTP_GET, [this, rateLimitCallback]() {
        DebugApiService::handleApiPerfFilesDownload(
            server,
            rateLimitCallback,
            [this]() { markUiActivity(); });
    });
    server.on("/api/debug/perf-files/delete", HTTP_POST, [this, rateLimitCallback]() {
        DebugApiService::handleApiPerfFilesDelete(
            server,
            rateLimitCallback,
            [this]() { markUiActivity(); });
    });
    
    // WiFi client (STA) API routes - connect to external network
    server.on("/api/wifi/status", HTTP_GET, [this]() {
        WifiClientApiService::handleApiStatus(
            server,
            makeWifiClientRuntime(),
            [this]() { markUiActivity(); });
    });
    server.on("/api/wifi/scan", HTTP_POST, [this, rateLimitCallback]() {
        WifiClientApiService::handleApiScan(
            server,
            makeWifiClientRuntime(),
            rateLimitCallback,
            [this]() { markUiActivity(); });
    });
    server.on("/api/wifi/connect", HTTP_POST, [this, rateLimitCallback]() {
        WifiClientApiService::handleApiConnect(
            server,
            makeWifiClientRuntime(),
            rateLimitCallback,
            [this]() { markUiActivity(); });
    });
    server.on("/api/wifi/disconnect", HTTP_POST, [this, rateLimitCallback]() {
        WifiClientApiService::handleApiDisconnect(
            server,
            makeWifiClientRuntime(),
            rateLimitCallback,
            [this]() { markUiActivity(); });
    });
    server.on("/api/wifi/forget", HTTP_POST, [this, rateLimitCallback]() {
        WifiClientApiService::handleApiForget(
            server,
            makeWifiClientRuntime(),
            rateLimitCallback,
            [this]() { markUiActivity(); });
    });
    server.on("/api/wifi/enable", HTTP_POST, [this, rateLimitCallback]() {
        WifiClientApiService::handleApiEnable(
            server,
            makeWifiClientRuntime(),
            rateLimitCallback,
            [this]() { markUiActivity(); });
    });

    // OBD integration API routes
    server.on("/api/obd/status", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiStatus(
            server,
            obdHandler,
            bleClient,
            settingsManager.get(),
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/obd/scan", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiScan(
            server,
            obdHandler,
            bleClient,
            rateLimitCallback,
            markUiActivityCallback,
            [this]() {
                if (!settingsManager.get().obdEnabled) {
                    server.send(409, "application/json", "{\"success\":false,\"message\":\"OBD service disabled\"}");
                    return false;
                }
                return true;
            });
    });
    server.on("/api/obd/scan/stop", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiScanStop(
            server,
            obdHandler,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/obd/devices", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiDevices(
            server,
            obdHandler,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/obd/devices/clear", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiDevicesClear(
            server,
            obdHandler,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/obd/connect", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiConnect(
            server,
            obdHandler,
            bleClient,
            rateLimitCallback,
            markUiActivityCallback,
            [this]() {
                if (!settingsManager.get().obdEnabled) {
                    server.send(409, "application/json", "{\"success\":false,\"message\":\"OBD service disabled\"}");
                    return false;
                }
                return true;
            });
    });
    server.on("/api/obd/disconnect", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiDisconnect(
            server,
            obdHandler,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/obd/config", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiConfig(
            server,
            obdHandler,
            settingsManager,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/obd/remembered", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiRemembered(
            server,
            obdHandler,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/obd/remembered/autoconnect", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiRememberedAutoConnect(
            server,
            obdHandler,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/obd/forget", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiForget(
            server,
            obdHandler,
            rateLimitCallback,
            markUiActivityCallback);
    });

    // GPS scaffold API routes
    server.on("/api/gps/status", HTTP_GET, [this, markUiActivityCallback]() {
        GpsApiService::handleApiStatus(
            server,
            gpsRuntimeModule,
            speedSourceSelector,
            settingsManager,
            gpsObservationLog,
            lockoutLearner,
            perfCounters,
            systemEventBus,
            markUiActivityCallback);
    });
    server.on("/api/gps/observations", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        GpsApiService::handleApiObservations(
            server,
            gpsObservationLog,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/gps/config", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        GpsApiService::handleApiConfig(
            server,
            settingsManager,
            gpsRuntimeModule,
            speedSourceSelector,
            lockoutLearner,
            gpsObservationLog,
            perfCounters,
            systemEventBus,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/cameras/status", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        CameraApiService::handleApiStatus(
            server,
            cameraRuntimeModule,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/cameras/catalog", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        CameraApiService::handleApiCatalog(
            server,
            storageManager,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/cameras/events", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        CameraApiService::handleApiEvents(
            server,
            cameraRuntimeModule,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/cameras/demo", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        CameraApiService::handleApiDemo(
            server,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/cameras/demo/clear", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        CameraApiService::handleApiDemoClear(
            server,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/lockouts/zones", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiZones(
            server,
            lockoutIndex,
            lockoutLearner,
            settingsManager,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/lockouts/summary", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiSummary(
            server,
            signalObservationLog,
            signalObservationSdLogger,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/lockouts/events", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiEvents(
            server,
            signalObservationLog,
            signalObservationSdLogger,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/lockouts/zones/delete", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiZoneDelete(
            server,
            lockoutIndex,
            lockoutStore,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/lockouts/zones/create", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiZoneCreate(
            server,
            lockoutIndex,
            lockoutStore,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/lockouts/zones/update", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiZoneUpdate(
            server,
            lockoutIndex,
            lockoutStore,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/lockouts/zones/export", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiZoneExport(
            server,
            lockoutStore,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/lockouts/zones/import", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiZoneImport(
            server,
            lockoutIndex,
            lockoutStore,
            rateLimitCallback,
            markUiActivityCallback);
    });
    server.on("/api/lockout/zones", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiZones(
            server,
            lockoutIndex,
            lockoutLearner,
            settingsManager,
            rateLimitCallback,
            markUiActivityCallback,
            [this]() { server.sendHeader("X-API-Deprecated", "Use /api/lockouts/zones"); });
    });
    server.on("/api/lockout/summary", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiSummary(
            server,
            signalObservationLog,
            signalObservationSdLogger,
            rateLimitCallback,
            markUiActivityCallback,
            [this]() { server.sendHeader("X-API-Deprecated", "Use /api/lockouts/summary"); });
    });
    server.on("/api/lockout/events", HTTP_GET, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiEvents(
            server,
            signalObservationLog,
            signalObservationSdLogger,
            rateLimitCallback,
            markUiActivityCallback,
            [this]() { server.sendHeader("X-API-Deprecated", "Use /api/lockouts/events"); });
    });
    server.on("/api/lockout/zones/delete", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiZoneDelete(
            server,
            lockoutIndex,
            lockoutStore,
            rateLimitCallback,
            markUiActivityCallback,
            [this]() { server.sendHeader("X-API-Deprecated", "Use /api/lockouts/zones/delete"); });
    });
    
    // Note: onNotFound is set earlier to handle LittleFS static files
}

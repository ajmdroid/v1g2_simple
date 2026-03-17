/**
 * WiFi Routes — setupWebServer() route registrations.
 * Extracted from wifi_manager.cpp for maintainability.
 */

#include "wifi_manager_internals.h"
#include "perf_metrics.h"
#include "settings.h"
#include "storage_manager.h"
#include "modules/gps/gps_api_service.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_lockout_safety.h"
#include "modules/gps/gps_observation_log.h"
#include "modules/lockout/lockout_api_service.h"
#include "modules/lockout/lockout_index.h"
#include "modules/lockout/lockout_learner.h"
#include "modules/debug/debug_api_service.h"
#include "modules/wifi/backup_api_service.h"
#include "modules/wifi/wifi_audio_api_service.h"
#include "modules/wifi/wifi_client_api_service.h"
#include "modules/wifi/wifi_control_api_service.h"
#include "modules/wifi/wifi_display_colors_api_service.h"
#include "modules/wifi/wifi_portal_api_service.h"
#include "modules/wifi/wifi_settings_api_service.h"
#include "modules/wifi/wifi_status_api_service.h"
#include "modules/wifi/wifi_autopush_api_service.h"
#include "modules/wifi/wifi_v1_profile_api_service.h"
#include "modules/wifi/wifi_v1_devices_api_service.h"
#include "modules/lockout/lockout_store.h"
#include "modules/lockout/signal_observation_log.h"
#include "modules/lockout/signal_observation_sd_logger.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/obd/obd_api_service.h"
#include "modules/obd/obd_runtime_module.h"
#include "battery_manager.h"
#include <LittleFS.h>

void WiFiManager::setupWebServer() {
    // Initialize LittleFS for serving web UI files
    if (!LittleFS.begin(false, "/littlefs", 10, "storage")) {
        WIFI_LOG("[SetupMode] ERROR: LittleFS mount failed (not formatting automatically)\n");
        return;
    }
    WIFI_LOG("[SetupMode] LittleFS mounted\n");
    // Dump LittleFS root for diagnostics (opt-in to avoid startup stall)
    if (WIFI_DEBUG_FS_DUMP) {
        dumpLittleFSRoot();
    }

    // WebServer::stop() only closes the listening socket; registered handlers
    // persist on the server instance across WiFi restarts.
    if (webRoutesInitialized) {
        return;
    }
    
    // New UI served from LittleFS
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
    server.on("/api/device/settings", HTTP_GET, [this]() {
        WifiSettingsApiService::handleApiDeviceSettingsGet(server, makeSettingsRuntime());
    });
    server.on("/api/device/settings", HTTP_POST, [this, rateLimitCallback]() {
        WifiSettingsApiService::handleApiDeviceSettingsSave(
            server,
            makeSettingsRuntime(),
            rateLimitCallback);
    });
    
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
    server.on("/api/v1/devices", HTTP_GET, [this]() {
        WifiV1DevicesApiService::handleApiDevicesList(server, makeV1DevicesRuntime());
    });
    server.on("/api/v1/devices/name", HTTP_POST, [this, rateLimitCallback]() {
        WifiV1DevicesApiService::handleApiDeviceNameSave(
            server,
            makeV1DevicesRuntime(),
            rateLimitCallback);
    });
    server.on("/api/v1/devices/profile", HTTP_POST, [this, rateLimitCallback]() {
        WifiV1DevicesApiService::handleApiDeviceProfileSave(
            server,
            makeV1DevicesRuntime(),
            rateLimitCallback);
    });
    server.on("/api/v1/devices/delete", HTTP_POST, [this, rateLimitCallback]() {
        WifiV1DevicesApiService::handleApiDeviceDelete(
            server,
            makeV1DevicesRuntime(),
            rateLimitCallback);
    });
    
    // Auto-Push routes
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
    
    // Display settings routes
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

    // Audio settings routes
    server.on("/api/audio/settings", HTTP_GET, [this]() {
        WifiAudioApiService::handleApiGet(server, makeAudioRuntime());
    });
    server.on("/api/audio/settings", HTTP_POST, [this, rateLimitCallback]() {
        WifiAudioApiService::handleApiSave(
            server,
            makeAudioRuntime(),
            rateLimitCallback);
    });
    
    // Settings backup/restore API routes
    server.on("/api/settings/backup", HTTP_GET, [this]() {
        BackupApiService::handleApiBackup(
            server,
            cachedBackupSnapshot,
            [this]() { markUiActivity(); },
            []() { return static_cast<uint32_t>(millis()); });
    });
    server.on("/api/settings/backup-now", HTTP_POST, [this, rateLimitCallback]() {
        BackupApiService::handleApiBackupNow(
            server,
            rateLimitCallback,
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
    server.on("/api/debug/v1-scenario/list", HTTP_GET, [this]() {
        DebugApiService::handleApiV1ScenarioList(server);
    });
    server.on("/api/debug/v1-scenario/status", HTTP_GET, [this]() {
        DebugApiService::handleApiV1ScenarioStatus(server);
    });
    server.on("/api/debug/v1-scenario/load", HTTP_POST, [this, rateLimitCallback]() {
        DebugApiService::handleApiV1ScenarioLoad(
            server,
            rateLimitCallback);
    });
    server.on("/api/debug/v1-scenario/start", HTTP_POST, [this, rateLimitCallback]() {
        DebugApiService::handleApiV1ScenarioStart(
            server,
            rateLimitCallback);
    });
    server.on("/api/debug/v1-scenario/stop", HTTP_POST, [this, rateLimitCallback]() {
        DebugApiService::handleApiV1ScenarioStop(
            server,
            rateLimitCallback);
    });
    server.on("/api/debug/enable", HTTP_POST, [this, rateLimitCallback]() {
        DebugApiService::handleApiDebugEnable(
            server,
            rateLimitCallback);
    });
    server.on("/api/debug/metrics/reset", HTTP_POST, [this, rateLimitCallback]() {
        DebugApiService::handleApiMetricsReset(
            server,
            rateLimitCallback);
    });
    server.on("/api/debug/proxy-advertising", HTTP_POST, [this, rateLimitCallback]() {
        DebugApiService::handleApiProxyAdvertisingControl(
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
    server.on("/api/gps/config", HTTP_GET, [this, markUiActivityCallback]() {
        GpsApiService::handleApiConfigGet(
            server,
            settingsManager,
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
    server.on("/api/lockouts/pending/clear", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        LockoutApiService::handleApiPendingClear(
            server,
            lockoutLearner,
            rateLimitCallback,
            markUiActivityCallback);
    });

    // OBD API routes
    server.on("/api/obd/status", HTTP_GET, [this, markUiActivityCallback]() {
        ObdApiService::handleApiStatus(server, obdRuntimeModule, markUiActivityCallback);
    });
    server.on("/api/obd/config", HTTP_GET, [this, markUiActivityCallback]() {
        ObdApiService::handleApiConfigGet(server, settingsManager, markUiActivityCallback);
    });
    server.on("/api/obd/scan", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiScan(server, obdRuntimeModule, rateLimitCallback, markUiActivityCallback);
    });
    server.on("/api/obd/forget", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiForget(server, obdRuntimeModule, settingsManager, rateLimitCallback, markUiActivityCallback);
    });
    server.on("/api/obd/config", HTTP_POST, [this, rateLimitCallback, markUiActivityCallback]() {
        ObdApiService::handleApiConfig(server,
                                      obdRuntimeModule,
                                      settingsManager,
                                      [this](bool enabled) {
                                          speedSourceSelector.syncEnabledInputs(
                                              settingsManager.get().gpsEnabled,
                                              enabled);
                                      },
                                      rateLimitCallback,
                                      markUiActivityCallback);
    });
    
    // Note: onNotFound is set earlier to handle LittleFS static files
    webRoutesInitialized = true;
}

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
#include "modules/wifi/wifi_static_path_guard.h"
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

extern GpsRuntimeModule          gpsRuntimeModule;
extern GpsObservationLog         gpsObservationLog;
extern SpeedSourceSelector       speedSourceSelector;
extern LockoutLearner            lockoutLearner;
extern LockoutIndex              lockoutIndex;
extern LockoutStore              lockoutStore;
extern SignalObservationLog      signalObservationLog;
extern SignalObservationSdLogger signalObservationSdLogger;
extern ObdRuntimeModule          obdRuntimeModule;

bool WiFiManager::setupWebServer() {
    // Initialize LittleFS for serving web UI files
    if (!LittleFS.begin(false, "/littlefs", 10, "storage")) {
        WIFI_LOG("[SetupMode] ERROR: LittleFS mount failed (not formatting automatically)\n");
        return false;
    }
    WIFI_LOG("[SetupMode] LittleFS mounted\n");
    // Dump LittleFS root for diagnostics (opt-in to avoid startup stall)
    if (WIFI_DEBUG_FS_DUMP) {
        dumpLittleFSRoot();
    }

    // WebServer::stop() only closes the listening socket; registered handlers
    // persist on the server instance across WiFi restarts.
    if (webRoutesInitialized) {
        return true;
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

        if (!WifiStaticPathGuard::isSafe(uri.c_str())) {
            Serial.printf("[HTTP] REJECT unsafe path %s\n", uri.c_str());
            server.send(404, "text/plain", "Not found");
            return;
        }
        
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
    
    // New API endpoints (PHASE A)
    server.on("/api/status", HTTP_GET, [this]() {
        WifiStatusApiService::handleApiStatus(
            server,
            makeStatusRuntime(),
            cachedStatusJson,
            lastStatusJsonTime,
            STATUS_CACHE_TTL_MS,
            [](void* /*ctx*/) -> unsigned long { return millis(); }, nullptr,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/profile/push", HTTP_POST, [this]() {
        WifiControlApiService::handleApiProfilePush(
            server,
            bleClient.isConnected(),
            [](void* ctx) -> WifiControlApiService::ProfilePushResult {
                auto* self = static_cast<WiFiManager*>(ctx);
                return self->requestProfilePush ? self->requestProfilePush(self->requestProfilePushCtx) : WifiControlApiService::ProfilePushResult::HANDLER_UNAVAILABLE;
            },
            this,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    
    // Legacy status endpoint
    server.on("/status", HTTP_GET, [this]() {
        WifiStatusApiService::handleApiLegacyStatus(
            server,
            makeStatusRuntime(),
            cachedStatusJson,
            lastStatusJsonTime,
            STATUS_CACHE_TTL_MS,
            [](void* /*ctx*/) -> unsigned long { return millis(); }, nullptr);
    });
    server.on("/api/device/settings", HTTP_GET, [this]() {
        WifiSettingsApiService::handleApiDeviceSettingsGet(server, makeSettingsRuntime());
    });
    server.on("/api/device/settings", HTTP_POST, [this]() {
        WifiSettingsApiService::handleApiDeviceSettingsSave(server, makeSettingsRuntime());
    });
    
    server.on("/darkmode", HTTP_POST, [this]() {
        WifiControlApiService::handleApiDarkMode(
            server,
            [](const char* cmd, bool val, void* ctx) {
                auto* self = static_cast<WiFiManager*>(ctx);
                return self->sendV1Command ? self->sendV1Command(cmd, val, self->sendV1CommandCtx) : false;
            },
            this,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    server.on("/mute", HTTP_POST, [this]() {
        WifiControlApiService::handleApiMute(
            server,
            [](const char* cmd, bool val, void* ctx) {
                auto* self = static_cast<WiFiManager*>(ctx);
                return self->sendV1Command ? self->sendV1Command(cmd, val, self->sendV1CommandCtx) : false;
            },
            this,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    
    // Lightweight health and captive-portal helpers
    server.on("/ping", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiPing(
            server,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    // Android/ChromeOS captive portal probes
    server.on("/generate_204", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiGenerate204(
            server,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    server.on("/gen_204", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiGen204(
            server,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    // iOS/macOS captive portal
    server.on("/hotspot-detect.html", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiHotspotDetect(
            server,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
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
    server.on("/api/v1/profile", HTTP_POST, [this]() {
        WifiV1ProfileApiService::handleApiProfileSave(
            server,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/v1/profile/delete", HTTP_POST, [this]() {
        WifiV1ProfileApiService::handleApiProfileDelete(
            server,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/v1/pull", HTTP_POST, [this]() {
        WifiV1ProfileApiService::handleApiSettingsPull(
            server,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/v1/push", HTTP_POST, [this]() {
        WifiV1ProfileApiService::handleApiSettingsPush(
            server,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/v1/current", HTTP_GET, [this]() {
        WifiV1ProfileApiService::handleApiCurrentSettings(server, makeV1ProfileRuntime());
    });
    server.on("/api/v1/devices", HTTP_GET, [this]() {
        WifiV1DevicesApiService::handleApiDevicesList(server, makeV1DevicesRuntime());
    });
    server.on("/api/v1/devices/name", HTTP_POST, [this]() {
        WifiV1DevicesApiService::handleApiDeviceNameSave(
            server,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/v1/devices/profile", HTTP_POST, [this]() {
        WifiV1DevicesApiService::handleApiDeviceProfileSave(
            server,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/v1/devices/delete", HTTP_POST, [this]() {
        WifiV1DevicesApiService::handleApiDeviceDelete(
            server,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    
    // Auto-Push routes
    server.on("/api/autopush/slots", HTTP_GET, [this]() {
        WifiAutoPushApiService::handleApiSlots(server, makeAutoPushRuntime());
    });
    server.on("/api/autopush/slot", HTTP_POST, [this]() {
        WifiAutoPushApiService::handleApiSlotSave(
            server,
            makeAutoPushRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/autopush/activate", HTTP_POST, [this]() {
        WifiAutoPushApiService::handleApiActivate(
            server,
            makeAutoPushRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/autopush/push", HTTP_POST, [this]() {
        WifiAutoPushApiService::handleApiPushNow(
            server,
            makeAutoPushRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/autopush/status", HTTP_GET, [this]() {
        WifiAutoPushApiService::handleApiStatus(server, makeAutoPushRuntime());
    });
    
    // Display settings routes
    server.on("/api/display/settings", HTTP_GET, [this]() {
        WifiDisplayColorsApiService::handleApiGet(server, makeDisplayColorsRuntime());
    });
    server.on("/api/display/settings", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiSave(
            server,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/display/settings/reset", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiReset(
            server,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/display/preview", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiPreview(
            server,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/display/preview/clear", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiClear(
            server,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });

    // Audio settings routes
    server.on("/api/audio/settings", HTTP_GET, [this]() {
        WifiAudioApiService::handleApiGet(server, makeAudioRuntime());
    });
    server.on("/api/audio/settings", HTTP_POST, [this]() {
        WifiAudioApiService::handleApiSave(server, makeAudioRuntime());
    });
    
    // Settings backup/restore API routes
    server.on("/api/settings/backup", HTTP_GET, [this]() {
        BackupApiService::handleApiBackup(
            server,
            cachedBackupSnapshot,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this,
            [](void* /*ctx*/) { return static_cast<uint32_t>(millis()); }, nullptr);
    });
    server.on("/api/settings/backup-now", HTTP_POST, [this]() {
        BackupApiService::handleApiBackupNow(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/settings/restore", HTTP_POST, [this]() {
        BackupApiService::handleApiRestore(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
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
    server.on("/api/debug/v1-scenario/load", HTTP_POST, [this]() {
        DebugApiService::handleApiV1ScenarioLoad(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/debug/v1-scenario/start", HTTP_POST, [this]() {
        DebugApiService::handleApiV1ScenarioStart(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/debug/v1-scenario/stop", HTTP_POST, [this]() {
        DebugApiService::handleApiV1ScenarioStop(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/debug/enable", HTTP_POST, [this]() {
        DebugApiService::handleApiDebugEnable(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/debug/metrics/reset", HTTP_POST, [this]() {
        DebugApiService::handleApiMetricsReset(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/debug/proxy-advertising", HTTP_POST, [this]() {
        DebugApiService::handleApiProxyAdvertisingControl(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server.on("/api/debug/perf-files", HTTP_GET, [this]() {
        DebugApiService::handleApiPerfFilesList(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/debug/perf-files/download", HTTP_GET, [this]() {
        DebugApiService::handleApiPerfFilesDownload(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/debug/perf-files/delete", HTTP_POST, [this]() {
        DebugApiService::handleApiPerfFilesDelete(
            server,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    
    // WiFi client (STA) API routes - connect to external network
    server.on("/api/wifi/status", HTTP_GET, [this]() {
        WifiClientApiService::handleApiStatus(
            server,
            makeWifiClientRuntime(),
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/wifi/scan", HTTP_POST, [this]() {
        WifiClientApiService::handleApiScan(
            server,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/wifi/connect", HTTP_POST, [this]() {
        WifiClientApiService::handleApiConnect(
            server,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/wifi/disconnect", HTTP_POST, [this]() {
        WifiClientApiService::handleApiDisconnect(
            server,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/wifi/forget", HTTP_POST, [this]() {
        WifiClientApiService::handleApiForget(
            server,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/wifi/enable", HTTP_POST, [this]() {
        WifiClientApiService::handleApiEnable(
            server,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });

    // GPS scaffold API routes
    server.on("/api/gps/status", HTTP_GET, [this]() {
        GpsApiService::handleApiStatus(
            server,
            gpsRuntimeModule,
            speedSourceSelector,
            settingsManager,
            gpsObservationLog,
            lockoutLearner,
            perfCounters,
            systemEventBus,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/gps/observations", HTTP_GET, [this]() {
        GpsApiService::handleApiObservations(
            server,
            gpsObservationLog,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/gps/config", HTTP_GET, [this]() {
        GpsApiService::handleApiConfigGet(
            server,
            settingsManager,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/gps/config", HTTP_POST, [this]() {
        GpsApiService::handleApiConfig(
            server,
            settingsManager,
            gpsRuntimeModule,
            speedSourceSelector,
            lockoutLearner,
            gpsObservationLog,
            perfCounters,
            systemEventBus,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/lockouts/zones", HTTP_GET, [this]() {
        LockoutApiService::handleApiZones(
            server,
            lockoutIndex,
            lockoutLearner,
            lockoutStore,
            settingsManager,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/lockouts/summary", HTTP_GET, [this]() {
        LockoutApiService::handleApiSummary(
            server,
            signalObservationLog,
            signalObservationSdLogger,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/lockouts/events", HTTP_GET, [this]() {
        LockoutApiService::handleApiEvents(
            server,
            signalObservationLog,
            signalObservationSdLogger,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/lockouts/zones/delete", HTTP_POST, [this]() {
        LockoutApiService::handleApiZoneDelete(
            server,
            lockoutIndex,
            lockoutStore,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/lockouts/zones/create", HTTP_POST, [this]() {
        LockoutApiService::handleApiZoneCreate(
            server,
            lockoutIndex,
            lockoutStore,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/lockouts/zones/update", HTTP_POST, [this]() {
        LockoutApiService::handleApiZoneUpdate(
            server,
            lockoutIndex,
            lockoutStore,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/lockouts/zones/export", HTTP_GET, [this]() {
        LockoutApiService::handleApiZoneExport(
            server,
            lockoutStore,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/lockouts/zones/import", HTTP_POST, [this]() {
        LockoutApiService::handleApiZoneImport(
            server,
            lockoutIndex,
            lockoutStore,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/lockouts/pending/clear", HTTP_POST, [this]() {
        LockoutApiService::handleApiPendingClear(
            server,
            lockoutLearner,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });

    // OBD API routes
    server.on("/api/obd/status", HTTP_GET, [this]() {
        ObdApiService::handleApiStatus(server, obdRuntimeModule,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/obd/devices", HTTP_GET, [this]() {
        ObdApiService::handleApiDevicesList(server, obdRuntimeModule, settingsManager,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/obd/config", HTTP_GET, [this]() {
        ObdApiService::handleApiConfigGet(server, settingsManager,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/obd/devices/name", HTTP_POST, [this]() {
        ObdApiService::handleApiDeviceNameSave(server, settingsManager,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/obd/scan", HTTP_POST, [this]() {
        ObdApiService::handleApiScan(server, obdRuntimeModule,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/obd/forget", HTTP_POST, [this]() {
        ObdApiService::handleApiForget(server, obdRuntimeModule, settingsManager,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server.on("/api/obd/config", HTTP_POST, [this]() {
        ObdApiService::handleApiConfig(server,
                                      obdRuntimeModule,
                                      settingsManager,
                                      speedSourceSelector,
                                      [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
                                      [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    
    // Note: onNotFound is set earlier to handle LittleFS static files
    webRoutesInitialized = true;
    return true;
}

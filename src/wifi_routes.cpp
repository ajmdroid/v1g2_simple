/**
 * WiFi Routes — setupWebServer() route registrations.
 * Extracted from wifi_manager.cpp for maintainability.
 */

#include "wifi_manager_internals.h"
#include "perf_metrics.h"
#include "settings.h"
#include "storage_manager.h"
#include "modules/debug/debug_api_service.h"
#include "modules/debug/debug_perf_files_service.h"
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
#include "modules/speed/speed_source_selector.h"
#include "modules/obd/obd_api_service.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/ota/ota_api_service.h"
#include "battery_manager.h"
#include <LittleFS.h>

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
    if (webRoutesInitialized_) {
        return true;
    }

    // New UI served from LittleFS
    // Serve static assets from _app directory
    server_.on("/_app/env.js", HTTP_GET, [this]() { serveLittleFSFile("/_app/env.js", "application/javascript"); });
    server_.on("/_app/version.json", HTTP_GET, [this]() { serveLittleFSFile("/_app/version.json", "application/json"); });

    // Root serves /index.html (Svelte app)
    server_.on("/", HTTP_GET, [this]() {
        markUiActivity();  // Track UI activity
        if (serveLittleFSFile("/index.html", "text/html")) {
            Serial.printf("[HTTP] 200 / -> /index.html\n");
            return;
        }
        // LittleFS missing - tell user to reflash
        Serial.println("[HTTP] 500 / -> LittleFS missing");
        server_.send(500, "application/json", "{\"ok\":false,\"error\":\"Web UI not found. Please reflash with ./build.sh --all\"}");
    });

    // Catch-all for _app/immutable/* files (if Svelte files are uploaded)
    server_.onNotFound([this]() {
        markUiActivity();  // Track UI activity
        String uri = server_.uri();

        if (!WifiStaticPathGuard::isSafe(uri.c_str())) {
            Serial.printf("[HTTP] REJECT unsafe path %s\n", uri.c_str());
            server_.send(404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}");
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
    server_.on("/api/status", HTTP_GET, [this]() {
        WifiStatusApiService::handleApiStatus(
            server_,
            makeStatusRuntime(),
            cachedStatusJson_,
            lastStatusJsonTime_,
            STATUS_CACHE_TTL_MS,
            [](void* /*ctx*/) -> unsigned long { return millis(); }, nullptr,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/profile/push", HTTP_POST, [this]() {
        WifiControlApiService::handleApiProfilePush(
            server_,
            bleClient.isConnected(),
            [](void* ctx) -> WifiControlApiService::ProfilePushResult {
                auto* self = static_cast<WiFiManager*>(ctx);
                return self->requestProfilePush_ ? self->requestProfilePush_(self->requestProfilePushCtx_) : WifiControlApiService::ProfilePushResult::HANDLER_UNAVAILABLE;
            },
            this,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });

    // Legacy status endpoint
    server_.on("/status", HTTP_GET, [this]() {
        WifiStatusApiService::handleApiLegacyStatus(
            server_,
            makeStatusRuntime(),
            cachedStatusJson_,
            lastStatusJsonTime_,
            STATUS_CACHE_TTL_MS,
            [](void* /*ctx*/) -> unsigned long { return millis(); }, nullptr);
    });
    server_.on("/api/device/settings", HTTP_GET, [this]() {
        WifiSettingsApiService::handleApiDeviceSettingsGet(server_, makeSettingsRuntime());
    });
    server_.on("/api/device/settings", HTTP_POST, [this]() {
        WifiSettingsApiService::handleApiDeviceSettingsSave(server_, makeSettingsRuntime());
    });

    server_.on("/darkmode", HTTP_POST, [this]() {
        WifiControlApiService::handleApiDarkMode(
            server_,
            [](const char* cmd, bool val, void* ctx) {
                auto* self = static_cast<WiFiManager*>(ctx);
                return self->sendV1Command_ ? self->sendV1Command_(cmd, val, self->sendV1CommandCtx_) : false;
            },
            this,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });
    server_.on("/mute", HTTP_POST, [this]() {
        WifiControlApiService::handleApiMute(
            server_,
            [](const char* cmd, bool val, void* ctx) {
                auto* self = static_cast<WiFiManager*>(ctx);
                return self->sendV1Command_ ? self->sendV1Command_(cmd, val, self->sendV1CommandCtx_) : false;
            },
            this,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); },
            this);
    });

    // Lightweight health and captive-portal helpers
    server_.on("/ping", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiPing(
            server_,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    // Android/ChromeOS captive portal probes
    server_.on("/generate_204", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiGenerate204(
            server_,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    server_.on("/gen_204", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiGen204(
            server_,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    // iOS/macOS captive portal
    server_.on("/hotspot-detect.html", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiHotspotDetect(
            server_,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); },
            this);
    });
    // Windows captive portal variants
    server_.on("/fwlink", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiFwlink(server_);
    });
    server_.on("/ncsi.txt", HTTP_GET, [this]() {
        WifiPortalApiService::handleApiNcsiTxt(server_);
    });

    // V1 Settings/Profiles routes
    server_.on("/api/v1/profiles", HTTP_GET, [this]() {
        WifiV1ProfileApiService::handleApiProfilesList(server_, makeV1ProfileRuntime());
    });
    server_.on("/api/v1/profile", HTTP_GET, [this]() {
        WifiV1ProfileApiService::handleApiProfileGet(server_, makeV1ProfileRuntime());
    });
    server_.on("/api/v1/profile", HTTP_POST, [this]() {
        WifiV1ProfileApiService::handleApiProfileSave(
            server_,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/profile/delete", HTTP_POST, [this]() {
        WifiV1ProfileApiService::handleApiProfileDelete(
            server_,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/pull", HTTP_POST, [this]() {
        WifiV1ProfileApiService::handleApiSettingsPull(
            server_,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/push", HTTP_POST, [this]() {
        WifiV1ProfileApiService::handleApiSettingsPush(
            server_,
            makeV1ProfileRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/current", HTTP_GET, [this]() {
        WifiV1ProfileApiService::handleApiCurrentSettings(server_, makeV1ProfileRuntime());
    });
    server_.on("/api/v1/devices", HTTP_GET, [this]() {
        WifiV1DevicesApiService::handleApiDevicesList(server_, makeV1DevicesRuntime());
    });
    server_.on("/api/v1/devices/name", HTTP_POST, [this]() {
        WifiV1DevicesApiService::handleApiDeviceNameSave(
            server_,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/devices/profile", HTTP_POST, [this]() {
        WifiV1DevicesApiService::handleApiDeviceProfileSave(
            server_,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/v1/devices/delete", HTTP_POST, [this]() {
        WifiV1DevicesApiService::handleApiDeviceDelete(
            server_,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });

    // Auto-Push routes
    server_.on("/api/autopush/slots", HTTP_GET, [this]() {
        WifiAutoPushApiService::handleApiSlots(server_, makeAutoPushRuntime());
    });
    server_.on("/api/autopush/slot", HTTP_POST, [this]() {
        WifiAutoPushApiService::handleApiSlotSave(
            server_,
            makeAutoPushRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/autopush/activate", HTTP_POST, [this]() {
        WifiAutoPushApiService::handleApiActivate(
            server_,
            makeAutoPushRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/autopush/push", HTTP_POST, [this]() {
        WifiAutoPushApiService::handleApiPushNow(
            server_,
            makeAutoPushRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/autopush/status", HTTP_GET, [this]() {
        WifiAutoPushApiService::handleApiStatus(server_, makeAutoPushRuntime());
    });

    // Display settings routes
    server_.on("/api/display/settings", HTTP_GET, [this]() {
        WifiDisplayColorsApiService::handleApiGet(server_, makeDisplayColorsRuntime());
    });
    server_.on("/api/display/settings", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiSave(
            server_,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/display/settings/reset", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiReset(
            server_,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/display/preview", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiPreview(
            server_,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/display/preview/clear", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiClear(
            server_,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });

    // Audio settings routes
    server_.on("/api/audio/settings", HTTP_GET, [this]() {
        WifiAudioApiService::handleApiGet(server_, makeAudioRuntime());
    });
    server_.on("/api/audio/settings", HTTP_POST, [this]() {
        WifiAudioApiService::handleApiSave(server_, makeAudioRuntime());
    });

    // Settings backup/restore API routes
    server_.on("/api/settings/backup", HTTP_GET, [this]() {
        BackupApiService::handleApiBackup(
            server_,
            cachedBackupSnapshot_,
            makeBackupRuntime(),
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this,
            [](void* /*ctx*/) { return static_cast<uint32_t>(millis()); }, nullptr);
    });
    server_.on("/api/settings/backup-now", HTTP_POST, [this]() {
        BackupApiService::handleApiBackupNow(
            server_,
            makeBackupRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/settings/restore", HTTP_POST, [this]() {
        BackupApiService::handleApiRestore(
            server_,
            makeBackupRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });

    // Debug API routes (performance metrics)
    server_.on("/api/debug/metrics", HTTP_GET, [this]() {
        DebugApiService::handleApiMetrics(server_);
    });
    server_.on("/api/debug/panic", HTTP_GET, [this]() {
        DebugApiService::handleApiPanic(server_);
    });
    server_.on("/api/debug/v1-scenario/list", HTTP_GET, [this]() {
        DebugApiService::handleApiV1ScenarioList(server_);
    });
    server_.on("/api/debug/v1-scenario/status", HTTP_GET, [this]() {
        DebugApiService::handleApiV1ScenarioStatus(server_);
    });
    server_.on("/api/debug/v1-scenario/load", HTTP_POST, [this]() {
        DebugApiService::handleApiV1ScenarioLoad(
            server_,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/debug/v1-scenario/start", HTTP_POST, [this]() {
        DebugApiService::handleApiV1ScenarioStart(
            server_,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/debug/v1-scenario/stop", HTTP_POST, [this]() {
        DebugApiService::handleApiV1ScenarioStop(
            server_,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/debug/enable", HTTP_POST, [this]() {
        DebugApiService::handleApiDebugEnable(
            server_,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/debug/metrics/reset", HTTP_POST, [this]() {
        DebugApiService::handleApiMetricsReset(
            server_,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/debug/proxy-advertising", HTTP_POST, [this]() {
        DebugApiService::handleApiProxyAdvertisingControl(
            server_,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/debug/perf-files", HTTP_GET, [this]() {
        DebugApiService::handleApiPerfFilesList(
            server_,
            makePerfFilesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/debug/perf-files/download", HTTP_GET, [this]() {
        DebugApiService::handleApiPerfFilesDownload(
            server_,
            makePerfFilesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/debug/perf-files/delete", HTTP_POST, [this]() {
        DebugApiService::handleApiPerfFilesDelete(
            server_,
            makePerfFilesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });

    // WiFi client (STA) API routes - connect to external network
    server_.on("/api/wifi/status", HTTP_GET, [this]() {
        WifiClientApiService::handleApiStatus(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/scan", HTTP_POST, [this]() {
        WifiClientApiService::handleApiScan(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/connect", HTTP_POST, [this]() {
        WifiClientApiService::handleApiConnect(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/disconnect", HTTP_POST, [this]() {
        WifiClientApiService::handleApiDisconnect(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/forget", HTTP_POST, [this]() {
        WifiClientApiService::handleApiForget(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/wifi/enable", HTTP_POST, [this]() {
        WifiClientApiService::handleApiEnable(
            server_,
            makeWifiClientRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });

    // OBD API routes
    server_.on("/api/obd/status", HTTP_GET, [this]() {
        ObdApiService::handleApiStatus(server_, *obdRuntime_,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/obd/devices", HTTP_GET, [this]() {
        ObdApiService::handleApiDevicesList(server_, *obdRuntime_, settingsManager,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/obd/config", HTTP_GET, [this]() {
        ObdApiService::handleApiConfigGet(server_, settingsManager,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/obd/devices/name", HTTP_POST, [this]() {
        ObdApiService::handleApiDeviceNameSave(server_, settingsManager,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/obd/scan", HTTP_POST, [this]() {
        ObdApiService::handleApiScan(server_, *obdRuntime_,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/obd/forget", HTTP_POST, [this]() {
        ObdApiService::handleApiForget(server_, *obdRuntime_, settingsManager,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
            [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });
    server_.on("/api/obd/config", HTTP_POST, [this]() {
        ObdApiService::handleApiConfig(server_,
                                      *obdRuntime_,
                                      settingsManager,
                                      *speedSelector_,
                                      [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this,
                                      [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); }, this);
    });

    // OTA update routes
    server_.on("/api/version", HTTP_GET, [this]() {
        OtaApiService::handleApiVersion(server_);
    });
    server_.on("/api/ota/status", HTTP_GET, [this]() {
        OtaApiService::handleApiOtaStatus(server_);
    });
    server_.on("/api/ota/check", HTTP_POST, [this]() {
        OtaApiService::handleApiOtaCheck(server_,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });
    server_.on("/api/ota/start", HTTP_POST, [this]() {
        OtaApiService::handleApiOtaStart(server_,
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });

    // Note: onNotFound is set earlier to handle LittleFS static files
    webRoutesInitialized_ = true;
    return true;
}

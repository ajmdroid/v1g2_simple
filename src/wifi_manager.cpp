/**
 * WiFi Manager for V1 Gen2 Display
 * AP+STA: always-on access point serving the local UI/API
 *         plus optional station mode to connect to external network
 */

#include "wifi_manager.h"
#include "perf_metrics.h"
#include "settings.h"
#include "settings_sanitize.h"
#include "display.h"
#include "storage_manager.h"
#include "debug_logger.h"
#include "v1_profiles.h"
#include "ble_client.h"
#include "perf_sd_logger.h"
#include "audio_beep.h"
#include "battery_manager.h"
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
#include "modules/wifi/wifi_settings_api_service.h"
#include "modules/wifi/wifi_status_api_service.h"
#include "modules/wifi/wifi_time_api_service.h"
#include "modules/wifi/wifi_autopush_api_service.h"
#include "modules/wifi/wifi_v1_profile_api_service.h"
#include "modules/lockout/lockout_store.h"
#include "modules/lockout/lockout_band_policy.h"
#include "modules/lockout/signal_observation_log.h"
#include "modules/lockout/signal_observation_sd_logger.h"
#include "modules/speed/speed_source_selector.h"
#include "time_service.h"
#include "modules/system/system_event_bus.h"
#include "../include/config.h"
#include "../include/band_utils.h"
#include "../include/color_themes.h"
#include <HTTPClient.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <map>
#include <vector>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "esp_wifi.h"

// External BLE client for V1 commands
extern V1BLEClient bleClient;
extern SystemEventBus systemEventBus;
// Preview helpers for display demo flows (color + camera).
extern void requestColorPreviewHold(uint32_t durationMs);
extern void requestCameraPreviewCycleHold(uint32_t durationMs);
extern void requestCameraPreviewSingleHold(uint8_t cameraType, uint32_t durationMs, bool muted);
extern bool isDisplayPreviewRunning();
extern bool isColorPreviewRunning();
extern void cancelDisplayPreview();
extern void cancelColorPreview();

// Enable to dump LittleFS root on WiFi start (debug only); keep false for release
static constexpr bool WIFI_DEBUG_FS_DUMP = false;
static constexpr bool WIFI_DEBUG_LOGS = false;  // Set true for verbose Serial logging

// WiFi logging macro - logs to Serial AND debugLogger when WiFi category enabled
#if defined(DISABLE_DEBUG_LOGGER)
#define WIFI_LOG(...) do { } while(0)
#else
#define WIFI_LOG(...) do { \
    if (WIFI_DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    DBG_LOGF(DebugLogCategory::Wifi, __VA_ARGS__); \
} while(0)
#endif

// Optional AP auto-timeout (milliseconds). Set to 0 to keep always-on behavior.
static constexpr unsigned long WIFI_AP_AUTO_TIMEOUT_MS = 0;            // e.g., 10 * 60 * 1000 for 10 minutes
static constexpr unsigned long WIFI_AP_INACTIVITY_GRACE_MS = 60 * 1000; // Require no UI activity/clients for this long before stopping

// Dump LittleFS root directory for diagnostics
static void dumpLittleFSRoot() {
    if (!LittleFS.begin(true)) {
        Serial.println("[SetupMode] ERROR: Failed to mount LittleFS for root dump");
        return;
    }
    
    Serial.println("[SetupMode] Dumping LittleFS root...");
    Serial.println("[SetupMode] Files in LittleFS root:");
    
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println("[SetupMode] ERROR: Could not open root directory");
        if (root) root.close();
        return;
    }
    
    File file = root.openNextFile();
    bool hasFiles = false;
    while (file) {
        hasFiles = true;
        Serial.printf("[SetupMode]   %s (%u bytes)\n", file.name(), file.size());
        file = root.openNextFile();
    }
    
    if (!hasFiles) {
        Serial.println("[SetupMode]   (empty)");
    }
    
    root.close();
}

static uint8_t clampU8Value(int value, int minVal, int maxVal) {
    return clampU8(value, minVal, maxVal);
}

static bool shouldUseApSta(const V1Settings& settings) {
    return settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0;
}

static const char* wifiClientStateApiName(WifiClientState state) {
    switch (state) {
        case WIFI_CLIENT_DISABLED: return "disabled";
        case WIFI_CLIENT_DISCONNECTED: return "disconnected";
        case WIFI_CLIENT_CONNECTING: return "connecting";
        case WIFI_CLIENT_CONNECTED: return "connected";
        case WIFI_CLIENT_FAILED: return "failed";
        default: return "unknown";
    }
}

static void getWifiStartThresholds(bool apStaMode, uint32_t& minFree, uint32_t& minBlock) {
    minFree = apStaMode ? WiFiManager::WIFI_START_MIN_FREE_AP_STA
                        : WiFiManager::WIFI_START_MIN_FREE_AP_ONLY;
    minBlock = apStaMode ? WiFiManager::WIFI_START_MIN_BLOCK_AP_STA
                         : WiFiManager::WIFI_START_MIN_BLOCK_AP_ONLY;
}

static void getWifiRuntimeThresholds(bool apStaMode, uint32_t& minFree, uint32_t& minBlock) {
    minFree = apStaMode ? WiFiManager::WIFI_RUNTIME_MIN_FREE_AP_STA
                        : WiFiManager::WIFI_RUNTIME_MIN_FREE_AP_ONLY;
    minBlock = apStaMode ? WiFiManager::WIFI_RUNTIME_MIN_BLOCK_AP_STA
                         : WiFiManager::WIFI_RUNTIME_MIN_BLOCK_AP_ONLY;
}

// Helper to serve files from LittleFS (with gzip support)
bool serveLittleFSFileHelper(WebServer& server, const char* path, const char* contentType) {
    uint32_t startUs = PERF_TIMESTAMP_US();
    // Try compressed version first (only if client accepts gzip)
    String acceptEncoding = server.header("Accept-Encoding");
    bool clientAcceptsGzip = acceptEncoding.indexOf("gzip") >= 0;
    
    if (clientAcceptsGzip) {
        String gzPath = String(path) + ".gz";
        if (LittleFS.exists(gzPath.c_str())) {
            File file = LittleFS.open(gzPath.c_str(), "r");
            if (file) {
                size_t fileSize = file.size();
                String etag = String("\"") + String(path) + ".gz-" + String(fileSize) + String("\"");
                if (server.header("If-None-Match") == etag) {
                    server.sendHeader("ETag", etag);
                    server.send(304, contentType, "");
                    file.close();
                    return true;
                }
                server.setContentLength(fileSize);
                server.sendHeader("Content-Encoding", "gzip");
                server.sendHeader("Cache-Control", "max-age=86400");
                server.sendHeader("ETag", etag);
                server.send(200, contentType, "");
                Serial.printf("[HTTP] 200 %s -> %s.gz (%u bytes)\n", path, path, fileSize);
                
                // Stream file content
                uint8_t buf[1024];
                while (file.available()) {
                    size_t len = file.read(buf, sizeof(buf));
                    server.client().write(buf, len);
                    yield();  // Allow FreeRTOS to schedule other tasks (BLE queue drain)
                }
                file.close();
                perfRecordFsServeUs(PERF_TIMESTAMP_US() - startUs);
                return true;
            }
        }
    }
    
    // Fall back to uncompressed
    File file = LittleFS.open(path, "r");
    if (!file) {
        Serial.printf("[HTTP] MISS %s (file not found)\n", path);
        return false;
    }
    size_t fileSize = file.size();
    String etag = String("\"") + String(path) + "-" + String(fileSize) + String("\"");
    if (server.header("If-None-Match") == etag) {
        server.sendHeader("ETag", etag);
        server.send(304, contentType, "");
        file.close();
        return true;
    }
    server.sendHeader("Cache-Control", "max-age=86400");
    server.sendHeader("ETag", etag);
    server.streamFile(file, contentType);
    Serial.printf("[HTTP] 200 %s (%u bytes)\n", path, fileSize);
    file.close();
    perfRecordFsServeUs(PERF_TIMESTAMP_US() - startUs);
    return true;
}

// Global instance
WiFiManager wifiManager;

WiFiManager::WiFiManager() : server(80), setupModeState(SETUP_MODE_OFF), setupModeStartTime(0), rateLimitWindowStart(0), rateLimitRequestCount(0) {
}

// Rate limiting: returns true if request is allowed, false if rate limited
bool WiFiManager::checkRateLimit() {
    unsigned long now = millis();
    
    // Mark UI activity on every request
    markUiActivity();
    
    // Reset window if expired
    if (now - rateLimitWindowStart > RATE_LIMIT_WINDOW_MS) {
        rateLimitWindowStart = now;
        rateLimitRequestCount = 0;
    }
    
    rateLimitRequestCount++;
    
    if (rateLimitRequestCount > RATE_LIMIT_MAX_REQUESTS) {
        unsigned long retryAfterSec = 1;
        if (now >= rateLimitWindowStart && (now - rateLimitWindowStart) < RATE_LIMIT_WINDOW_MS) {
            retryAfterSec = ((RATE_LIMIT_WINDOW_MS - (now - rateLimitWindowStart)) + 999) / 1000;
        }
        server.sendHeader("Retry-After", String(retryAfterSec));
        server.send(429, "application/json",
                    "{\"success\":false,\"message\":\"Too many requests\"}");
        return false;
    }
    
    return true;
}

// Web activity tracking for WiFi priority mode
void WiFiManager::markUiActivity() {
    lastUiActivityMs = millis();
}

bool WiFiManager::isUiActive(unsigned long timeoutMs) const {
    if (lastUiActivityMs == 0) return false;
    return (millis() - lastUiActivityMs) < timeoutMs;
}

unsigned long WiFiManager::lowDmaCooldownRemainingMs() const {
    if (lowDmaCooldownUntilMs == 0) {
        return 0;
    }

    unsigned long now = millis();
    long remaining = static_cast<long>(lowDmaCooldownUntilMs - now);
    return (remaining > 0) ? static_cast<unsigned long>(remaining) : 0;
}

bool WiFiManager::canStartSetupMode(uint32_t* freeInternal, uint32_t* largestInternal) const {
    const uint32_t freeNow = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largestNow = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (freeInternal) {
        *freeInternal = freeNow;
    }
    if (largestInternal) {
        *largestInternal = largestNow;
    }

    if (lowDmaCooldownRemainingMs() > 0) {
        return false;
    }

    const V1Settings& settings = settingsManager.get();
    uint32_t minFree = 0;
    uint32_t minBlock = 0;
    getWifiStartThresholds(shouldUseApSta(settings), minFree, minBlock);
    return freeNow >= minFree && largestNow >= minBlock;
}

// Ensure last client seen timestamp advances when UI is accessed
// (called on every HTTP request via checkRateLimit/markUiActivity)

bool WiFiManager::startSetupMode() {
    timeService.begin();  // Ensure persisted/system time is restored before serving UI.

    // Always-on AP; idempotent start
    if (setupModeState == SETUP_MODE_AP_ON) {
        WIFI_LOG("[SetupMode] Already active\n");
        return true;
    }

    WIFI_LOG("[SetupMode] Starting AP (always-on mode)...\n");
    const V1Settings& settings = settingsManager.get();
    const bool apStaMode = shouldUseApSta(settings);

    // Check internal SRAM before WiFi init. AP+STA requires more headroom than AP-only.
    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t minFree = 0;
    uint32_t minBlock = 0;
    getWifiStartThresholds(apStaMode, minFree, minBlock);
    const unsigned long cooldownMs = lowDmaCooldownRemainingMs();

    Serial.printf("[SetupMode] Start preflight: mode=%s freeInternal=%lu largestInternal=%lu needFree>=%lu needLargest>=%lu cooldownMs=%lu\n",
                  apStaMode ? "AP+STA" : "AP",
                  (unsigned long)freeInternal,
                  (unsigned long)largestInternal,
                  (unsigned long)minFree,
                  (unsigned long)minBlock,
                  (unsigned long)cooldownMs);

    if (cooldownMs > 0) {
        Serial.printf("[SetupMode] ABORT: low_dma cooldown active (%lu ms remaining)\n",
                      (unsigned long)cooldownMs);
        WIFI_LOG("[SetupMode] ABORT: low_dma cooldown active (%lu ms remaining)\n",
                 (unsigned long)cooldownMs);
        return false;
    }

    if (freeInternal < minFree || largestInternal < minBlock) {
        Serial.printf("[SetupMode] ABORT: Insufficient internal SRAM (need free>=%lu largest>=%lu, have free=%lu largest=%lu)\n",
                      (unsigned long)minFree,
                      (unsigned long)minBlock,
                      (unsigned long)freeInternal,
                      (unsigned long)largestInternal);
        WIFI_LOG("[SetupMode] ABORT: Insufficient internal SRAM (need free>=%lu largest>=%lu, have free=%lu largest=%lu)\n",
                 (unsigned long)minFree,
                 (unsigned long)minBlock,
                 (unsigned long)freeInternal,
                 (unsigned long)largestInternal);
        return false;  // Graceful fail instead of crash
    }

    setupModeStartTime = millis();
    lastClientSeenMs = setupModeStartTime;
    lowDmaSinceMs = 0;
    lowDmaCooldownUntilMs = 0;

    // Check if WiFi client is enabled - use AP+STA mode
    if (apStaMode) {
        WIFI_LOG("[SetupMode] WiFi client enabled, using AP+STA mode\n");
        WiFi.mode(WIFI_AP_STA);
        wifiClientState = WIFI_CLIENT_DISCONNECTED;
    } else {
        WiFi.mode(WIFI_AP);
        wifiClientState = WIFI_CLIENT_DISABLED;
    }

    setupAP();
    setupWebServer();

    // Collect headers for GZIP support and caching
    const char* headerKeys[] = {"Accept-Encoding", "If-None-Match"};
    server.collectHeaders(headerKeys, 2);

    server.begin();
    setupModeState = SETUP_MODE_AP_ON;

    WIFI_LOG("[SetupMode] AP started - connect to SSID shown on display\n");
    WIFI_LOG("[SetupMode] Web UI at http://%s\n", WiFi.softAPIP().toString().c_str());
    uint8_t timeoutMins = settingsManager.getApTimeoutMinutes();
    if (timeoutMins == 0) {
        WIFI_LOG("[SetupMode] AP will remain on (no timeout)\n");
    } else {
        WIFI_LOG("[SetupMode] AP auto-timeout set to %d minutes\n", timeoutMins);
    }

    return true;
}

bool WiFiManager::stopSetupMode(bool manual, const char* reason) {
    if (setupModeState != SETUP_MODE_AP_ON) {
        return false;
    }

    const char* stopReason = reason;
    if (!stopReason || stopReason[0] == '\0') {
        stopReason = manual ? "manual" : "unknown";
    }
    const bool emergencyLowDma = (strcmp(stopReason, "low_dma") == 0);
    const uint32_t stopStartMs = millis();
    uint32_t freeInternalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t largestInternalBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[SetupMode] Stopping WiFi: reason=%s manual=%d freeDma=%lu largestDma=%lu\n",
                  stopReason,
                  manual ? 1 : 0,
                  (unsigned long)freeInternalBefore,
                  (unsigned long)largestInternalBefore);

    if (emergencyLowDma) {
        // Emergency path: prioritize low-latency return to BLE/display loop.
        // Skip per-step delays and expensive disconnect/erase operations.
        WIFI_LOG("[SetupMode] Emergency low_dma shutdown (fast path)\n");
        lowDmaSinceMs = 0;
        lowDmaCooldownUntilMs = millis() + WIFI_LOW_DMA_RETRY_COOLDOWN_MS;
        server.stop();
        WiFi.mode(WIFI_OFF);
    } else {
        WIFI_LOG("[SetupMode] Stopping WiFi (strict OFF contract)...\n");
        lowDmaSinceMs = 0;

        // ========== 1. STOP ALL SERVICES ==========
        // Stop HTTP server first (no more requests)
        server.stop();
        WIFI_LOG("[SetupMode] HTTP server stopped\n");
        vTaskDelay(pdMS_TO_TICKS(1));  // Yield for display

        // ========== 2. STOP RADIO CLEANLY ==========
        // Disconnect STA if connected (with erase=true to clear stored credentials from radio)
        if (wifiClientState == WIFI_CLIENT_CONNECTED || wifiClientState == WIFI_CLIENT_CONNECTING) {
            WiFi.disconnect(true);  // true = also clear stored config in radio
            WIFI_LOG("[SetupMode] STA disconnected\n");
            vTaskDelay(pdMS_TO_TICKS(1));  // Yield for display
        }

        // Disconnect AP (with wifiOff=true to prepare for mode change)
        WiFi.softAPdisconnect(true);
        WIFI_LOG("[SetupMode] AP disconnected\n");
        vTaskDelay(pdMS_TO_TICKS(1));  // Yield for display

        // Set mode to OFF (tells Arduino WiFi class we're done)
        WiFi.mode(WIFI_OFF);

        // WiFi.mode(WIFI_OFF) handles radio shutdown via Arduino layer.
        // Note: Do NOT call esp_wifi_stop() or esp_wifi_deinit() - they conflict
        // with Arduino's WiFi class causing "netstack cb reg failed" errors and
        // blocking delays. Arduino layer manages init/deinit automatically.
        WIFI_LOG("[SetupMode] Radio stopped via WiFi.mode(WIFI_OFF)\n");
    }
    
    // ========== 3. RESET ALL STATE ==========
    setupModeState = SETUP_MODE_OFF;
    wifiClientState = WIFI_CLIENT_DISABLED;
    wifiScanRunning = false;
    wifiConnectStartMs = 0;
    pendingConnectSSID = "";
    pendingConnectPassword = "";
    lastUiActivityMs = 0;
    lastClientSeenMs = 0;

    // ========== 4. OBSERVABILITY ==========
    // Single-line status for post-mortem debugging (confirms radio truly OFF)
    uint32_t freeInternalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t largestInternalAfter = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t stopDurMs = millis() - stopStartMs;
    Serial.printf("[SetupMode] WiFi OFF: reason=%s manual=%d radio=%d http=%d freeDma=%lu largestDma=%lu durMs=%lu\n",
                  stopReason,
                  manual ? 1 : 0,
                  0,
                  0,
                  (unsigned long)freeInternalAfter,
                  (unsigned long)largestInternalAfter,
                  (unsigned long)stopDurMs);
    
    return true;
}

bool WiFiManager::toggleSetupMode(bool manual) {
    if (setupModeState == SETUP_MODE_AP_ON) {
        return stopSetupMode(manual, manual ? "manual" : "toggle");
    }
    return startSetupMode();
}

void WiFiManager::setupAP() {
    // Use saved SSID/password when available; fall back to defaults if missing/too short
    const V1Settings& settings = settingsManager.get();
    String apSSID = settings.apSSID.length() ? settings.apSSID : "V1-Simple";
    String apPass = (settings.apPassword.length() >= 8) ? settings.apPassword : "setupv1g2";  // WPA2 requires 8+
    
    WIFI_LOG("[SetupMode] Starting AP: %s (pass: ****)\n", apSSID.c_str());
    
    // Configure AP IP
    IPAddress apIP(192, 168, 35, 5);
    IPAddress gateway(192, 168, 35, 5);
    IPAddress subnet(255, 255, 255, 0);
    
    if (!WiFi.softAPConfig(apIP, gateway, subnet)) {
        // NOTE: Intentional fallthrough - softAP will still work with default IP (192.168.4.1)
        // Device remains functional. Reviewed January 20, 2026.
        WIFI_LOG("[SetupMode] softAPConfig failed! Will use default IP 192.168.4.1\n");
    }
    
    if (!WiFi.softAP(apSSID.c_str(), apPass.c_str())) {
        WIFI_LOG("[SetupMode] softAP failed!\n");
        return;
    }
    
    WIFI_LOG("[SetupMode] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
}

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
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
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
    
    auto makeStatusRuntime = [this]() {
        WifiStatusApiService::StatusRuntime runtime{
            [this]() { return setupModeState == SETUP_MODE_AP_ON; },
            [this]() { return wifiClientState == WIFI_CLIENT_CONNECTED; },
            []() { return WiFi.localIP().toString(); },
            [this]() { return getAPIPAddress(); },
            []() { return WiFi.SSID(); },
            []() { return WiFi.RSSI(); },
            [this]() { return settingsManager.get().wifiClientEnabled; },
            [this]() { return settingsManager.get().wifiClientSSID; },
            [this]() { return settingsManager.get().apSSID; },
            []() { return millis() / 1000; },
            []() { return ESP.getFreeHeap(); },
            []() { return String("v1g2"); },
            []() { return String(FIRMWARE_VERSION); },
            [this]() { return timeService.timeValid(); },
            [this]() { return timeService.timeSource(); },
            [this]() { return timeService.timeConfidence(); },
            [this]() { return timeService.tzOffsetMinutes(); },
            [this]() { return timeService.nowEpochMsOr0(); },
            [this]() { return timeService.epochAgeMsOr0(); },
            [this]() { return batteryManager.getVoltageMillivolts(); },
            [this]() { return batteryManager.getPercentage(); },
            [this]() { return batteryManager.isOnBattery(); },
            [this]() { return batteryManager.hasBattery(); },
            [this]() { return bleClient.isConnected(); },
            getStatusJson,
            getAlertJson,
        };
        return runtime;
    };

    auto makeSettingsRuntime = [this]() {
        return WifiSettingsApiService::Runtime{
            [this]() -> const V1Settings& {
                return settingsManager.get();
            },
            [this]() -> V1Settings& {
                return settingsManager.mutableSettings();
            },
            [this](const String& ssid, const String& password) {
                settingsManager.updateAPCredentials(ssid, password);
            },
            [this](uint8_t brightness) {
                settingsManager.updateBrightness(brightness);
            },
            [this](DisplayStyle style) {
                settingsManager.updateDisplayStyle(style);
            },
            [this]() {
                display.forceNextRedraw();
            },
            [this](bool enabled) {
                obdHandler.setVwDataEnabled(enabled);
            },
            [this]() {
                obdHandler.stopScan();
            },
            [this]() {
                obdHandler.disconnect();
            },
            [this](bool enabled) {
                gpsRuntimeModule.setEnabled(enabled);
            },
            [this](bool enabled) {
                speedSourceSelector.setGpsEnabled(enabled);
            },
            [this](bool enabled) {
                cameraRuntimeModule.setEnabled(enabled);
            },
            [this](bool enabled) {
                lockoutSetKaLearningEnabled(enabled);
            },
            [this]() {
                settingsManager.save();
            },
        };
    };
    auto settingsRateLimitCallback = [this]() { return checkRateLimit(); };
    auto makeTimeRuntime = [this]() {
        return WifiTimeApiService::TimeRuntime{
            [this]() { return timeService.timeValid(); },
            [this]() { return timeService.nowEpochMsOr0(); },
            [this]() { return timeService.tzOffsetMinutes(); },
            [this]() { return timeService.timeSource(); },
            [this](int64_t epochMs, int32_t tzOffsetMin, uint8_t source) {
                timeService.setEpochBaseMs(epochMs, tzOffsetMin, static_cast<TimeService::Source>(source));
            },
            [this]() { return timeService.timeConfidence(); },
            [this]() { return timeService.nowMonoMs(); },
            [this]() { return timeService.epochAgeMsOr0(); },
        };
    };

    // New API endpoints (PHASE A)
    server.on("/api/status", HTTP_GET, [this, makeStatusRuntime]() {
        WifiStatusApiService::handleApiStatus(
            server,
            makeStatusRuntime(),
            cachedStatusJson,
            lastStatusJsonTime,
            STATUS_CACHE_TTL_MS,
            []() { return millis(); },
            [this]() { return checkRateLimit(); });
    });
    server.on("/api/profile/push", HTTP_POST, [this]() { 
        WifiControlApiService::handleApiProfilePush(
            server,
            bleClient.isConnected(),
            requestProfilePush,
            [this]() { return checkRateLimit(); }); 
    });
    server.on("/api/time/set", HTTP_POST, [this, makeTimeRuntime]() {
        WifiTimeApiService::handleApiTimeSet(
            server,
            makeTimeRuntime(),
            TimeService::SOURCE_CLIENT_AP,
            [this]() { lastStatusJsonTime = 0; },
            [this]() { return checkRateLimit(); });
    });
    
    // Legacy status endpoint
    server.on("/status", HTTP_GET, [this, makeStatusRuntime]() {
        WifiStatusApiService::handleLegacyStatus(
            server,
            makeStatusRuntime(),
            cachedStatusJson,
            lastStatusJsonTime,
            STATUS_CACHE_TTL_MS,
            []() { return millis(); });
    });
    server.on("/api/settings", HTTP_GET, [this, makeSettingsRuntime]() {
        WifiSettingsApiService::handleSettingsGet(server, makeSettingsRuntime());
    });  // JSON settings for new UI
    server.on("/api/settings", HTTP_POST, [this, makeSettingsRuntime, settingsRateLimitCallback]() {
        WifiSettingsApiService::handleSettingsSave(
            server,
            makeSettingsRuntime(),
            settingsRateLimitCallback);
    });  // Consistent API endpoint
    
    // Legacy HTML page routes - redirect to root (SvelteKit handles routing)
    server.on("/settings", HTTP_GET, [this]() { 
        server.sendHeader("X-API-Deprecated", "Use /api/settings");
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    server.on("/settings", HTTP_POST, [this, makeSettingsRuntime]() {
        if (!checkRateLimit()) return;
        Serial.println("[HTTP] WARN: Legacy POST /settings used; prefer /api/settings");
        server.sendHeader("X-API-Deprecated", "Use /api/settings");
        WifiSettingsApiService::handleSettingsSave(
            server,
            makeSettingsRuntime(),
            [this]() { return checkRateLimit(); });
    });  // Legacy compat
    server.on("/darkmode", HTTP_POST, [this]() {
        WifiControlApiService::handleDarkMode(
            server,
            sendV1Command,
            [this]() { return checkRateLimit(); });
    });
    server.on("/mute", HTTP_POST, [this]() {
        WifiControlApiService::handleMute(
            server,
            sendV1Command,
            [this]() { return checkRateLimit(); });
    });
    
    // Lightweight health and captive-portal helpers
    server.on("/ping", HTTP_GET, [this]() {
        markUiActivity();
        Serial.println("[HTTP] GET /ping");
        server.send(200, "text/plain", "OK");
    });
    // Android/ChromeOS captive portal probes
    server.on("/generate_204", HTTP_GET, [this]() {
        markUiActivity();
        Serial.println("[HTTP] GET /generate_204");
        server.send(204, "text/plain", "");
    });
    server.on("/gen_204", HTTP_GET, [this]() {
        markUiActivity();
        Serial.println("[HTTP] GET /gen_204");
        server.send(204, "text/plain", "");
    });
    // iOS/macOS captive portal
    server.on("/hotspot-detect.html", HTTP_GET, [this]() {
        markUiActivity();
        Serial.println("[HTTP] GET /hotspot-detect.html");
        server.sendHeader("Location", "/settings", true);
        server.send(302, "text/html", "");
    });
    // Windows captive portal variants
    server.on("/fwlink", HTTP_GET, [this]() {
        Serial.println("[HTTP] GET /fwlink");
        server.sendHeader("Location", "/settings", true);
        server.send(302, "text/html", "");
    });
    server.on("/ncsi.txt", HTTP_GET, [this]() {
        Serial.println("[HTTP] GET /ncsi.txt");
        server.send(200, "text/plain", "Microsoft NCSI");
    });
    
    // V1 Settings/Profiles routes
    server.on("/v1settings", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    auto makeV1ProfileRuntime = [this]() {
        return WifiV1ProfileApiService::Runtime{
            []() { return v1ProfileManager.listProfiles(); },
            [](const String& name, WifiV1ProfileApiService::ProfileSummary& summary) {
                V1Profile profile;
                if (!v1ProfileManager.loadProfile(name, profile)) {
                    return false;
                }
                summary.name = profile.name;
                summary.description = profile.description;
                summary.displayOn = profile.displayOn;
                return true;
            },
            [](const String& name, String& json) {
                V1Profile profile;
                if (!v1ProfileManager.loadProfile(name, profile)) {
                    return false;
                }
                json = v1ProfileManager.profileToJson(profile);
                return true;
            },
            [](const String& name, uint8_t outBytes[6], bool& displayOn) {
                V1Profile profile;
                if (!v1ProfileManager.loadProfile(name, profile)) {
                    return false;
                }
                memcpy(outBytes, profile.settings.bytes, 6);
                displayOn = profile.displayOn;
                return true;
            },
            [](const JsonObject& settingsObj, uint8_t outBytes[6]) {
                V1UserSettings settings;
                if (!v1ProfileManager.jsonToSettings(settingsObj, settings)) {
                    return false;
                }
                memcpy(outBytes, settings.bytes, 6);
                return true;
            },
            [](const String& name,
               const String& description,
               bool displayOn,
               const uint8_t inBytes[6],
               String& error) {
                V1Profile profile;
                profile.name = name;
                profile.description = description;
                profile.displayOn = displayOn;
                memcpy(profile.settings.bytes, inBytes, 6);
                ProfileSaveResult result = v1ProfileManager.saveProfile(profile);
                if (!result.success) {
                    error = result.error;
                    return false;
                }
                return true;
            },
            [](const String& name) { return v1ProfileManager.deleteProfile(name); },
            []() { return bleClient.requestUserBytes(); },
            [](const uint8_t inBytes[6]) {
                return bleClient.writeUserBytesVerified(inBytes, 3) == V1BLEClient::VERIFY_OK;
            },
            [](bool displayOn) { bleClient.setDisplayOn(displayOn); },
            []() { return v1ProfileManager.hasCurrentSettings(); },
            []() { return v1ProfileManager.settingsToJson(v1ProfileManager.getCurrentSettings()); },
            []() { return bleClient.isConnected(); },
            [this]() { settingsManager.backupToSD(); },
        };
    };
    auto rateLimitCallback = [this]() { return checkRateLimit(); };
    server.on("/api/v1/profiles", HTTP_GET, [this, makeV1ProfileRuntime]() {
        WifiV1ProfileApiService::handleProfilesList(server, makeV1ProfileRuntime());
    });
    server.on("/api/v1/profile", HTTP_GET, [this, makeV1ProfileRuntime]() {
        WifiV1ProfileApiService::handleProfileGet(server, makeV1ProfileRuntime());
    });
    server.on("/api/v1/profile", HTTP_POST, [this, makeV1ProfileRuntime, rateLimitCallback]() {
        WifiV1ProfileApiService::handleProfileSave(
            server,
            makeV1ProfileRuntime(),
            rateLimitCallback);
    });
    server.on("/api/v1/profile/delete", HTTP_POST, [this, makeV1ProfileRuntime, rateLimitCallback]() {
        WifiV1ProfileApiService::handleProfileDelete(
            server,
            makeV1ProfileRuntime(),
            rateLimitCallback);
    });
    server.on("/api/v1/pull", HTTP_POST, [this, makeV1ProfileRuntime, rateLimitCallback]() {
        WifiV1ProfileApiService::handleSettingsPull(
            server,
            makeV1ProfileRuntime(),
            rateLimitCallback);
    });
    server.on("/api/v1/push", HTTP_POST, [this, makeV1ProfileRuntime, rateLimitCallback]() {
        WifiV1ProfileApiService::handleSettingsPush(
            server,
            makeV1ProfileRuntime(),
            rateLimitCallback);
    });
    server.on("/api/v1/current", HTTP_GET, [this, makeV1ProfileRuntime]() {
        WifiV1ProfileApiService::handleCurrentSettings(server, makeV1ProfileRuntime());
    });
    
    // Auto-Push routes
    auto makeAutoPushRuntime = [this]() {
        return WifiAutoPushApiService::Runtime{
            [this](WifiAutoPushApiService::SlotsSnapshot& snapshot) {
                const V1Settings& s = settingsManager.get();
                snapshot.enabled = s.autoPushEnabled;
                snapshot.activeSlot = s.activeSlot;

                snapshot.slots[0].name = s.slot0Name;
                snapshot.slots[0].profile = s.slot0_default.profileName;
                snapshot.slots[0].mode = s.slot0_default.mode;
                snapshot.slots[0].color = s.slot0Color;
                snapshot.slots[0].volume = s.slot0Volume;
                snapshot.slots[0].muteVolume = s.slot0MuteVolume;
                snapshot.slots[0].darkMode = s.slot0DarkMode;
                snapshot.slots[0].muteToZero = s.slot0MuteToZero;
                snapshot.slots[0].alertPersist = s.slot0AlertPersist;
                snapshot.slots[0].priorityArrowOnly = s.slot0PriorityArrow;

                snapshot.slots[1].name = s.slot1Name;
                snapshot.slots[1].profile = s.slot1_highway.profileName;
                snapshot.slots[1].mode = s.slot1_highway.mode;
                snapshot.slots[1].color = s.slot1Color;
                snapshot.slots[1].volume = s.slot1Volume;
                snapshot.slots[1].muteVolume = s.slot1MuteVolume;
                snapshot.slots[1].darkMode = s.slot1DarkMode;
                snapshot.slots[1].muteToZero = s.slot1MuteToZero;
                snapshot.slots[1].alertPersist = s.slot1AlertPersist;
                snapshot.slots[1].priorityArrowOnly = s.slot1PriorityArrow;

                snapshot.slots[2].name = s.slot2Name;
                snapshot.slots[2].profile = s.slot2_comfort.profileName;
                snapshot.slots[2].mode = s.slot2_comfort.mode;
                snapshot.slots[2].color = s.slot2Color;
                snapshot.slots[2].volume = s.slot2Volume;
                snapshot.slots[2].muteVolume = s.slot2MuteVolume;
                snapshot.slots[2].darkMode = s.slot2DarkMode;
                snapshot.slots[2].muteToZero = s.slot2MuteToZero;
                snapshot.slots[2].alertPersist = s.slot2AlertPersist;
                snapshot.slots[2].priorityArrowOnly = s.slot2PriorityArrow;
            },
            [this](String& json) {
                if (!getPushStatusJson) {
                    return false;
                }
                json = getPushStatusJson();
                return true;
            },
            [this](int slot, const String& name) {
                settingsManager.setSlotName(slot, name);
            },
            [this](int slot, uint16_t color) {
                settingsManager.setSlotColor(slot, color);
            },
            [this](int slot) {
                return settingsManager.getSlotVolume(slot);
            },
            [this](int slot) {
                return settingsManager.getSlotMuteVolume(slot);
            },
            [this](int slot, uint8_t volume, uint8_t muteVolume) {
                settingsManager.setSlotVolumes(slot, volume, muteVolume);
            },
            [this](int slot, bool darkMode) {
                settingsManager.setSlotDarkMode(slot, darkMode);
            },
            [this](int slot, bool muteToZero) {
                settingsManager.setSlotMuteToZero(slot, muteToZero);
            },
            [this](int slot, uint8_t alertPersistSec) {
                settingsManager.setSlotAlertPersistSec(slot, alertPersistSec);
            },
            [this](int slot, bool priorityArrowOnly) {
                settingsManager.setSlotPriorityArrowOnly(slot, priorityArrowOnly);
            },
            [this](int slot, const String& profile, int mode) {
                settingsManager.setSlot(slot, profile, normalizeV1ModeValue(mode));
            },
            [this]() {
                return static_cast<int>(settingsManager.get().activeSlot);
            },
            [this](int slot) {
                display.drawProfileIndicator(slot);
            },
            [this](int slot) {
                settingsManager.setActiveSlot(slot);
            },
            [this](bool enabled) {
                settingsManager.setAutoPushEnabled(enabled);
            },
            [this](const WifiAutoPushApiService::PushNowRequest& request) {
                if (!bleClient.isConnected()) {
                    return WifiAutoPushApiService::PushNowQueueResult::V1_NOT_CONNECTED;
                }

                if (pushNowState.step != PushNowStep::IDLE) {
                    return WifiAutoPushApiService::PushNowQueueResult::ALREADY_IN_PROGRESS;
                }

                String profileName;
                V1Mode mode = V1_MODE_UNKNOWN;

                if (request.hasProfileOverride) {
                    profileName = sanitizeProfileNameValue(request.profileName);
                    if (request.hasModeOverride) {
                        mode = normalizeV1ModeValue(request.mode);
                    }
                } else {
                    const V1Settings& s = settingsManager.get();
                    AutoPushSlot pushSlot;

                    switch (request.slot) {
                        case 0: pushSlot = s.slot0_default; break;
                        case 1: pushSlot = s.slot1_highway; break;
                        case 2: pushSlot = s.slot2_comfort; break;
                        default: break;
                    }

                    profileName = sanitizeProfileNameValue(pushSlot.profileName);
                    mode = normalizeV1ModeValue(static_cast<int>(pushSlot.mode));
                }

                if (profileName.length() == 0) {
                    return WifiAutoPushApiService::PushNowQueueResult::NO_PROFILE_CONFIGURED;
                }

                V1Profile profile;
                if (!v1ProfileManager.loadProfile(profileName, profile)) {
                    return WifiAutoPushApiService::PushNowQueueResult::PROFILE_LOAD_FAILED;
                }

                bool slotDarkMode = settingsManager.getSlotDarkMode(request.slot);
                uint8_t mainVol = settingsManager.getSlotVolume(request.slot);
                uint8_t muteVol = settingsManager.getSlotMuteVolume(request.slot);

                Serial.printf("[PushNow] Slot %d volumes - main: %d, mute: %d\n",
                              request.slot,
                              mainVol,
                              muteVol);

                settingsManager.setActiveSlot(request.slot);
                display.drawProfileIndicator(request.slot);

                pushNowState.slot = request.slot;
                memcpy(pushNowState.profileBytes,
                       profile.settings.bytes,
                       sizeof(pushNowState.profileBytes));
                pushNowState.displayOn = !slotDarkMode;  // Dark mode=true => display off
                pushNowState.applyMode = (mode != V1_MODE_UNKNOWN);
                pushNowState.mode = mode;
                pushNowState.applyVolume = (mainVol != 0xFF && muteVol != 0xFF);
                pushNowState.mainVol = mainVol;
                pushNowState.muteVol = muteVol;
                pushNowState.retries = 0;
                pushNowState.step = PushNowStep::WRITE_PROFILE;
                pushNowState.nextAtMs = millis();

                Serial.printf("[PushNow] Queued slot=%d profile='%s' mode=%d displayOn=%d volume=%s\n",
                              request.slot,
                              profileName.c_str(),
                              static_cast<int>(mode),
                              pushNowState.displayOn ? 1 : 0,
                              pushNowState.applyVolume ? "set" : "skip");

                return WifiAutoPushApiService::PushNowQueueResult::QUEUED;
            },
        };
    };
    server.on("/autopush", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    server.on("/api/autopush/slots", HTTP_GET, [this, makeAutoPushRuntime]() {
        WifiAutoPushApiService::handleSlots(server, makeAutoPushRuntime());
    });
    server.on("/api/autopush/slot", HTTP_POST, [this, makeAutoPushRuntime, rateLimitCallback]() {
        WifiAutoPushApiService::handleSlotSave(
            server,
            makeAutoPushRuntime(),
            rateLimitCallback);
    });
    server.on("/api/autopush/activate", HTTP_POST, [this, makeAutoPushRuntime, rateLimitCallback]() {
        WifiAutoPushApiService::handleActivate(
            server,
            makeAutoPushRuntime(),
            rateLimitCallback);
    });
    server.on("/api/autopush/push", HTTP_POST, [this, makeAutoPushRuntime, rateLimitCallback]() {
        WifiAutoPushApiService::handlePushNow(
            server,
            makeAutoPushRuntime(),
            rateLimitCallback);
    });
    server.on("/api/autopush/status", HTTP_GET, [this, makeAutoPushRuntime]() {
        WifiAutoPushApiService::handleStatus(server, makeAutoPushRuntime());
    });
    
    // Display Colors routes
    auto makeDisplayColorsRuntime = [this]() {
        return WifiDisplayColorsApiService::Runtime{
            [this]() -> const V1Settings& {
                return settingsManager.get();
            },
            [this]() -> V1Settings& {
                return settingsManager.mutableSettings();
            },
            [this]() {
                obdHandler.stopScan();
            },
            [this]() {
                obdHandler.disconnect();
            },
            [this](bool enabled) {
                gpsRuntimeModule.setEnabled(enabled);
            },
            [this](bool enabled) {
                speedSourceSelector.setGpsEnabled(enabled);
            },
            [this](bool enabled) {
                cameraRuntimeModule.setEnabled(enabled);
            },
            [this](uint8_t brightness) {
                display.setBrightness(brightness);
            },
            [](uint8_t volume) {
                audio_set_volume(volume);
            },
            [this]() {
                display.showDemo();
            },
            [](uint32_t durationMs) {
                requestColorPreviewHold(durationMs);
            },
            []() {
                return isColorPreviewRunning();
            },
            []() {
                cancelColorPreview();
            },
            [this]() {
                settingsManager.save();
            },
        };
    };
    server.on("/displaycolors", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    server.on("/api/displaycolors", HTTP_GET, [this, makeDisplayColorsRuntime]() {
        WifiDisplayColorsApiService::handleGet(server, makeDisplayColorsRuntime());
    });
    server.on("/api/displaycolors", HTTP_POST, [this, makeDisplayColorsRuntime, rateLimitCallback]() {
        WifiDisplayColorsApiService::handleSave(
            server,
            makeDisplayColorsRuntime(),
            rateLimitCallback);
    });
    server.on("/api/displaycolors/reset", HTTP_POST, [this, makeDisplayColorsRuntime, rateLimitCallback]() {
        WifiDisplayColorsApiService::handleReset(
            server,
            makeDisplayColorsRuntime(),
            rateLimitCallback);
    });
    server.on("/api/displaycolors/preview", HTTP_POST, [this, makeDisplayColorsRuntime]() { 
        if (!checkRateLimit()) return;
        WifiDisplayColorsApiService::handlePreview(server, makeDisplayColorsRuntime());
    });
    server.on("/api/displaycolors/clear", HTTP_POST, [this, makeDisplayColorsRuntime]() { 
        if (!checkRateLimit()) return;
        WifiDisplayColorsApiService::handleClear(server, makeDisplayColorsRuntime());
    });
    
    // Settings backup/restore API routes
    server.on("/api/settings/backup", HTTP_GET, [this]() {
        markUiActivity();
        BackupApiService::sendBackup(server);
    });
    server.on("/api/settings/restore", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        BackupApiService::handleRestore(server);
    });
    
    // Debug API routes (performance metrics)
    server.on("/api/debug/metrics", HTTP_GET, [this]() {
        DebugApiService::handleApiMetrics(server);
    });
    server.on("/api/debug/panic", HTTP_GET, [this]() {
        DebugApiService::handleApiPanic(server);
    });
    server.on("/api/debug/enable", HTTP_POST, [this]() {
        DebugApiService::handleApiDebugEnable(
            server,
            [this]() { return checkRateLimit(); });
    });
    server.on("/api/debug/perf-files", HTTP_GET, [this]() {
        DebugApiService::handleApiPerfFilesList(
            server,
            [this]() { return checkRateLimit(); },
            [this]() { markUiActivity(); });
    });
    server.on("/api/debug/perf-files/download", HTTP_GET, [this]() {
        DebugApiService::handleApiPerfFilesDownload(
            server,
            [this]() { return checkRateLimit(); },
            [this]() { markUiActivity(); });
    });
    server.on("/api/debug/perf-files/delete", HTTP_POST, [this]() {
        DebugApiService::handleApiPerfFilesDelete(
            server,
            [this]() { return checkRateLimit(); },
            [this]() { markUiActivity(); });
    });
    
    // WiFi client (STA) API routes - connect to external network
    auto makeWifiClientRuntime = [this]() {
        return WifiClientApiService::Runtime{
            [this]() { return settingsManager.get().wifiClientEnabled; },
            [this]() { return settingsManager.get().wifiClientSSID; },
            [this]() { return wifiClientStateApiName(wifiClientState); },
            [this]() { return wifiScanRunning; },
            [this]() { return wifiClientState == WIFI_CLIENT_CONNECTED; },
            []() {
                WifiClientApiService::ConnectedNetworkPayload payload;
                payload.ssid = WiFi.SSID();
                payload.ip = WiFi.localIP().toString();
                payload.rssi = WiFi.RSSI();
                return payload;
            },
            []() { return WiFi.scanComplete() == WIFI_SCAN_RUNNING; },
            []() { return WiFi.scanComplete() > 0; },
            [this]() {
                std::vector<ScannedNetwork> networks = this->getScannedNetworks();
                std::vector<WifiClientApiService::ScannedNetworkPayload> payloads;
                payloads.reserve(networks.size());
                for (const auto& net : networks) {
                    WifiClientApiService::ScannedNetworkPayload payload;
                    payload.ssid = net.ssid;
                    payload.rssi = net.rssi;
                    payload.secure = !net.isOpen();
                    payloads.push_back(payload);
                }
                return payloads;
            },
            [this]() { return startWifiScan(); },
            [this](const String& ssid, const String& password) {
                return connectToNetwork(ssid, password);
            },
            [this]() { disconnectFromNetwork(); },
            [this]() { settingsManager.clearWifiClientCredentials(); },
            [this](bool enabled) { settingsManager.setWifiClientEnabled(enabled); },
            [this]() { return settingsManager.getWifiClientPassword(); },
            [this]() { wifiClientState = WIFI_CLIENT_DISABLED; },
            [this]() { wifiClientState = WIFI_CLIENT_DISCONNECTED; },
            []() { WiFi.mode(WIFI_AP); },
        };
    };
    server.on("/api/wifi/status", HTTP_GET, [this, makeWifiClientRuntime]() {
        markUiActivity();
        WifiClientApiService::handleStatus(server, makeWifiClientRuntime());
    });
    server.on("/api/wifi/scan", HTTP_POST, [this, makeWifiClientRuntime]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        WifiClientApiService::handleScan(server, makeWifiClientRuntime());
    });
    server.on("/api/wifi/connect", HTTP_POST, [this, makeWifiClientRuntime]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        WifiClientApiService::handleConnect(server, makeWifiClientRuntime());
    });
    server.on("/api/wifi/disconnect", HTTP_POST, [this, makeWifiClientRuntime]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        WifiClientApiService::handleDisconnect(server, makeWifiClientRuntime());
    });
    server.on("/api/wifi/forget", HTTP_POST, [this, makeWifiClientRuntime]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        WifiClientApiService::handleForget(server, makeWifiClientRuntime());
    });
    server.on("/api/wifi/enable", HTTP_POST, [this, makeWifiClientRuntime]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        WifiClientApiService::handleEnable(server, makeWifiClientRuntime());
    });

    // OBD integration API routes
    server.on("/api/obd/status", HTTP_GET, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        ObdApiService::sendStatus(server, obdHandler, bleClient, settingsManager.get());
    });
    server.on("/api/obd/scan", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        if (!settingsManager.get().obdEnabled) {
            server.send(409, "application/json", "{\"success\":false,\"message\":\"OBD service disabled\"}");
            return;
        }
        ObdApiService::handleScan(server, obdHandler, bleClient);
    });
    server.on("/api/obd/scan/stop", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        ObdApiService::handleScanStop(server, obdHandler);
    });
    server.on("/api/obd/devices", HTTP_GET, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        ObdApiService::sendDevices(server, obdHandler);
    });
    server.on("/api/obd/devices/clear", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        ObdApiService::handleDevicesClear(server, obdHandler);
    });
    server.on("/api/obd/connect", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        if (!settingsManager.get().obdEnabled) {
            server.send(409, "application/json", "{\"success\":false,\"message\":\"OBD service disabled\"}");
            return;
        }
        ObdApiService::handleConnect(server, obdHandler, bleClient);
    });
    server.on("/api/obd/disconnect", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        ObdApiService::handleDisconnect(server, obdHandler);
    });
    server.on("/api/obd/config", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        ObdApiService::handleConfig(server, obdHandler, settingsManager);
    });
    server.on("/api/obd/remembered", HTTP_GET, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        ObdApiService::sendRemembered(server, obdHandler);
    });
    server.on("/api/obd/remembered/autoconnect", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        ObdApiService::handleRememberedAutoConnect(server, obdHandler);
    });
    server.on("/api/obd/forget", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        ObdApiService::handleForget(server, obdHandler);
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
            [this]() { markUiActivity(); });
    });
    server.on("/api/gps/observations", HTTP_GET, [this]() {
        GpsApiService::handleApiObservations(
            server,
            gpsObservationLog,
            [this]() { return checkRateLimit(); },
            [this]() { markUiActivity(); });
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
            [this]() { return checkRateLimit(); },
            [this]() { markUiActivity(); });
    });
    server.on("/api/cameras/status", HTTP_GET, [this]() {
        CameraApiService::handleApiStatus(
            server,
            cameraRuntimeModule,
            [this]() { return checkRateLimit(); },
            [this]() { markUiActivity(); });
    });
    server.on("/api/cameras/catalog", HTTP_GET, [this]() {
        CameraApiService::handleApiCatalog(
            server,
            storageManager,
            [this]() { return checkRateLimit(); },
            [this]() { markUiActivity(); });
    });
    server.on("/api/cameras/events", HTTP_GET, [this]() {
        CameraApiService::handleApiEvents(
            server,
            cameraRuntimeModule,
            [this]() { return checkRateLimit(); },
            [this]() { markUiActivity(); });
    });
    server.on("/api/cameras/demo", HTTP_POST, [this]() {
        CameraApiService::handleApiDemo(
            server,
            [this]() { return checkRateLimit(); },
            [this]() { markUiActivity(); });
    });
    server.on("/api/cameras/demo/clear", HTTP_POST, [this]() {
        CameraApiService::handleApiDemoClear(
            server,
            [this]() { return checkRateLimit(); },
            [this]() { markUiActivity(); });
    });
    server.on("/api/lockouts/zones", HTTP_GET, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        LockoutApiService::sendZones(server, lockoutIndex, lockoutLearner,
                                     settingsManager);
    });
    server.on("/api/lockouts/summary", HTTP_GET, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        LockoutApiService::sendSummary(server, signalObservationLog,
                                       signalObservationSdLogger);
    });
    server.on("/api/lockouts/events", HTTP_GET, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        LockoutApiService::sendEvents(server, signalObservationLog,
                                      signalObservationSdLogger);
    });
    server.on("/api/lockouts/zones/delete", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        markUiActivity();
        LockoutApiService::handleZoneDelete(server, lockoutIndex, lockoutStore);
    });
    server.on("/api/lockout/zones", HTTP_GET, [this]() {
        server.sendHeader("X-API-Deprecated", "Use /api/lockouts/zones");
        if (!checkRateLimit()) return;
        markUiActivity();
        LockoutApiService::sendZones(server, lockoutIndex, lockoutLearner,
                                     settingsManager);
    });
    server.on("/api/lockout/summary", HTTP_GET, [this]() {
        server.sendHeader("X-API-Deprecated", "Use /api/lockouts/summary");
        if (!checkRateLimit()) return;
        markUiActivity();
        LockoutApiService::sendSummary(server, signalObservationLog,
                                       signalObservationSdLogger);
    });
    server.on("/api/lockout/events", HTTP_GET, [this]() {
        server.sendHeader("X-API-Deprecated", "Use /api/lockouts/events");
        if (!checkRateLimit()) return;
        markUiActivity();
        LockoutApiService::sendEvents(server, signalObservationLog,
                                      signalObservationSdLogger);
    });
    server.on("/api/lockout/zones/delete", HTTP_POST, [this]() {
        server.sendHeader("X-API-Deprecated", "Use /api/lockouts/zones/delete");
        if (!checkRateLimit()) return;
        markUiActivity();
        LockoutApiService::handleZoneDelete(server, lockoutIndex, lockoutStore);
    });
    
    // Note: onNotFound is set earlier to handle LittleFS static files
}

void WiFiManager::checkAutoTimeout() {
    uint8_t timeoutMins = settingsManager.getApTimeoutMinutes();
    if (timeoutMins == 0) return;  // Disabled (always on)
    if (setupModeState != SETUP_MODE_AP_ON) return;

    unsigned long timeoutMs = (unsigned long)timeoutMins * 60UL * 1000UL;
    unsigned long now = millis();
    int staCount = WiFi.softAPgetStationNum();
    if (staCount > 0) {
        lastClientSeenMs = now;
    }

    unsigned long lastActivity = lastUiActivityMs;
    if (lastClientSeenMs > lastActivity) {
        lastActivity = lastClientSeenMs;
    }

    bool timeoutElapsed = (now - setupModeStartTime) >= timeoutMs;
    bool inactiveEnough = (lastActivity == 0) ? ((now - setupModeStartTime) >= WIFI_AP_INACTIVITY_GRACE_MS)
                                              : ((now - lastActivity) >= WIFI_AP_INACTIVITY_GRACE_MS);

    if (timeoutElapsed && inactiveEnough && staCount == 0) {
        Serial.println("[SetupMode] Auto-timeout reached - stopping AP");
        stopSetupMode(false, "timeout");
    }
}

void WiFiManager::process() {
    if (setupModeState != SETUP_MODE_AP_ON) {
        lowDmaSinceMs = 0;
        return;  // No WiFi processing when Setup Mode is off
    }

    // Runtime SRAM guard with persistence + mode-aware thresholds:
    // AP+STA needs more memory than AP-only, and short dips should not force shutdown.
    const wifi_mode_t mode = WiFi.getMode();
    const bool apStaMode = (mode == WIFI_AP_STA || mode == WIFI_STA);
    uint32_t criticalFree = 0;
    uint32_t criticalBlock = 0;
    getWifiRuntimeThresholds(apStaMode, criticalFree, criticalBlock);

    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const bool lowHeap = (freeInternal < criticalFree) || (largestInternal < criticalBlock);

    if (lowHeap) {
        const unsigned long now = millis();
        if (lowDmaSinceMs == 0) {
            lowDmaSinceMs = now;
            Serial.printf("[WiFi] WARN: Internal SRAM low (mode=%s free=%lu block=%lu need>=%lu/%lu) - grace %lu ms\n",
                          apStaMode ? "AP+STA" : "AP",
                          (unsigned long)freeInternal,
                          (unsigned long)largestInternal,
                          (unsigned long)criticalFree,
                          (unsigned long)criticalBlock,
                          (unsigned long)WIFI_LOW_DMA_PERSIST_MS);
        } else if ((now - lowDmaSinceMs) >= WIFI_LOW_DMA_PERSIST_MS) {
            WIFI_LOG("[WiFi] CRITICAL: Internal SRAM low for %lu ms (free=%lu, block=%lu) - emergency shutdown\n",
                     (unsigned long)(now - lowDmaSinceMs),
                     (unsigned long)freeInternal,
                     (unsigned long)largestInternal);
            Serial.printf("[WiFi] CRITICAL: Internal SRAM low for %lu ms (free=%lu block=%lu) - stopping WiFi\n",
                          (unsigned long)(now - lowDmaSinceMs),
                          (unsigned long)freeInternal,
                          (unsigned long)largestInternal);

            // In AP+STA mode, drop STA first to preserve local AP/UI control while
            // still shedding WiFi memory pressure quickly.
            if (apStaMode) {
                Serial.println("[WiFi] ACTION: dropping STA due to sustained low SRAM (keeping AP online)");
                WiFi.disconnect(false);
                WiFi.mode(WIFI_AP);
                wifiClientState = WIFI_CLIENT_DISCONNECTED;
                wifiConnectPhase = WifiConnectPhase::IDLE;
                wifiConnectPhaseStartMs = 0;
                wifiConnectStartMs = 0;
                pendingConnectSSID = "";
                pendingConnectPassword = "";
                lowDmaCooldownUntilMs = now + WIFI_LOW_DMA_RETRY_COOLDOWN_MS;
                lowDmaSinceMs = 0;
                debugLogger.notifyWifiTransition(true);
                return;
            }

            stopSetupMode(false, "low_dma");  // Graceful shutdown to free memory
            return;
        }
    } else if (lowDmaSinceMs != 0) {
        const unsigned long lowDuration = millis() - lowDmaSinceMs;
        Serial.printf("[WiFi] RECOVERED: Internal SRAM back above threshold after %lu ms\n",
                      (unsigned long)lowDuration);
        lowDmaSinceMs = 0;
    }
    
    // Handle web requests
    if (setupModeState != SETUP_MODE_AP_ON) {
        return;
    }

    server.handleClient();
    processWifiClientConnectPhase();
    processPendingPushNow();
    checkAutoTimeout();
    
    // Check WiFi client (STA) connection status
    checkWifiClientStatus();
}

String WiFiManager::getAPIPAddress() const {
    if (setupModeState == SETUP_MODE_AP_ON) {
        return WiFi.softAPIP().toString();
    }
    return "";
}

String WiFiManager::getIPAddress() const {
    if (wifiClientState == WIFI_CLIENT_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "";
}

String WiFiManager::getConnectedSSID() const {
    if (wifiClientState == WIFI_CLIENT_CONNECTED) {
        return WiFi.SSID();
    }
    return "";
}

bool WiFiManager::startWifiScan() {
    if (wifiScanRunning) {
        Serial.println("[WiFiClient] Scan already in progress");
        return false;
    }
    
    Serial.println("[WiFiClient] Starting async network scan...");
    WiFi.scanDelete();  // Clear previous results
    
    // Start async scan (non-blocking)
    int result = WiFi.scanNetworks(true, false, false, 300);  // async=true, show_hidden=false, passive=false, max_ms_per_chan=300
    if (result == WIFI_SCAN_RUNNING) {
        wifiScanRunning = true;
        return true;
    }
    
    Serial.printf("[WiFiClient] Scan failed to start: %d\n", result);
    return false;
}

std::vector<ScannedNetwork> WiFiManager::getScannedNetworks() {
    std::vector<ScannedNetwork> networks;
    
    int16_t scanResult = WiFi.scanComplete();
    if (scanResult == WIFI_SCAN_RUNNING) {
        // Still scanning
        return networks;  // Empty
    }
    
    wifiScanRunning = false;
    
    if (scanResult == WIFI_SCAN_FAILED || scanResult < 0) {
        Serial.printf("[WiFiClient] Scan failed: %d\n", scanResult);
        return networks;
    }
    
    Serial.printf("[WiFiClient] Scan found %d networks\n", scanResult);
    
    // Deduplicate by SSID (keep strongest signal)
    std::map<String, ScannedNetwork> uniqueNetworks;
    
    for (int i = 0; i < scanResult; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;  // Skip hidden networks
        
        int32_t rssi = WiFi.RSSI(i);
        uint8_t encType = WiFi.encryptionType(i);
        
        auto it = uniqueNetworks.find(ssid);
        if (it == uniqueNetworks.end() || rssi > it->second.rssi) {
            uniqueNetworks[ssid] = {ssid, rssi, encType};
        }
    }
    
    // Convert to vector and sort by signal strength
    for (const auto& pair : uniqueNetworks) {
        networks.push_back(pair.second);
    }
    
    std::sort(networks.begin(), networks.end(), [](const ScannedNetwork& a, const ScannedNetwork& b) {
        return a.rssi > b.rssi;  // Strongest first
    });
    
    WiFi.scanDelete();  // Free memory
    return networks;
}

bool WiFiManager::connectToNetwork(const String& ssid, const String& password) {
    if (ssid.length() == 0) {
        Serial.println("[WiFiClient] Cannot connect: empty SSID");
        return false;
    }

    // WiFi transitioning - defer SD writes to avoid NVS flash contention
    debugLogger.notifyWifiTransition(false);

    // Stage a non-blocking connect sequence to avoid stalling loop().
    pendingConnectSSID = ssid;
    pendingConnectPassword = password;
    wifiConnectStartMs = 0;
    wifiClientState = WIFI_CLIENT_CONNECTING;
    wifiConnectPhase = WifiConnectPhase::PREPARE_OFF;
    wifiConnectPhaseStartMs = millis();
    PERF_INC(wifiConnectDeferred);
    return true;
}

void WiFiManager::disconnectFromNetwork() {
    Serial.println("[WiFiClient] Disconnecting from network");
    wifiConnectPhase = WifiConnectPhase::IDLE;
    wifiConnectPhaseStartMs = 0;
    wifiConnectStartMs = 0;
    WiFi.disconnect(false);  // Don't turn off station mode
    wifiClientState = WIFI_CLIENT_DISCONNECTED;
    pendingConnectSSID = "";
    pendingConnectPassword = "";
}

void WiFiManager::processWifiClientConnectPhase() {
    if (wifiConnectPhase == WifiConnectPhase::IDLE) {
        return;
    }

    unsigned long now = millis();
    switch (wifiConnectPhase) {
        case WifiConnectPhase::PREPARE_OFF:
            if (setupModeState == SETUP_MODE_AP_ON) {
                // Keep AP online and use a direct STA begin path.
                // Repeated STA resets in AP+STA mode have proven brittle on some routers.
                Serial.println("[WiFiClient] Preserving AP, preparing STA connect...");
                if (WiFi.getMode() != WIFI_AP_STA) {
                    WiFi.mode(WIFI_AP_STA);
                    wifiConnectPhaseStartMs = now;
                    wifiConnectPhase = WifiConnectPhase::WAIT_AP_STA;
                } else {
                    wifiConnectPhase = WifiConnectPhase::BEGIN_CONNECT;
                }
            } else {
                if (WiFi.getMode() != WIFI_OFF) {
                    Serial.println("[WiFiClient] Cleaning up WiFi before reconnect...");
                    WiFi.disconnect(true, true);  // Disconnect and erase credentials from RAM
                    WiFi.mode(WIFI_OFF);          // Fully shut down WiFi driver
                }
                wifiConnectPhaseStartMs = now;
                wifiConnectPhase = WifiConnectPhase::WAIT_OFF;
            }
            break;

        case WifiConnectPhase::WAIT_OFF:
            if (now - wifiConnectPhaseStartMs >= WIFI_MODE_SWITCH_SETTLE_MS) {
                wifiConnectPhase = WifiConnectPhase::ENABLE_AP_STA;
            }
            break;

        case WifiConnectPhase::ENABLE_AP_STA:
            Serial.println("[WiFiClient] Initializing WiFi in AP+STA mode");
            WiFi.mode(WIFI_AP_STA);
            wifiConnectPhaseStartMs = now;
            wifiConnectPhase = WifiConnectPhase::WAIT_AP_STA;
            break;

        case WifiConnectPhase::WAIT_AP_STA:
            if (now - wifiConnectPhaseStartMs >= WIFI_MODE_SWITCH_SETTLE_MS) {
                wifiConnectPhase = WifiConnectPhase::BEGIN_CONNECT;
            }
            break;

        case WifiConnectPhase::BEGIN_CONNECT:
            if (pendingConnectSSID.length() == 0) {
                wifiConnectPhase = WifiConnectPhase::IDLE;
                wifiClientState = WIFI_CLIENT_FAILED;
                debugLogger.notifyWifiTransition(true);
                break;
            }
            // Improve coexistence stability while connecting alongside BLE links.
            WiFi.setSleep(false);
            WiFi.setAutoReconnect(true);
            Serial.printf("[WiFiClient] Connecting to: %s\n", pendingConnectSSID.c_str());
            WiFi.begin(pendingConnectSSID.c_str(), pendingConnectPassword.c_str());
            wifiConnectStartMs = now;
            wifiConnectPhase = WifiConnectPhase::IDLE;
            break;

        case WifiConnectPhase::IDLE:
        default:
            break;
    }
}

void WiFiManager::processPendingPushNow() {
    if (pushNowState.step == PushNowStep::IDLE) {
        return;
    }

    if (!bleClient.isConnected()) {
        Serial.println("[PushNow] Aborted: V1 disconnected");
        pushNowState.step = PushNowStep::IDLE;
        return;
    }

    unsigned long now = millis();
    if (now < pushNowState.nextAtMs) {
        return;
    }

    auto scheduleRetry = [&](const char* op) {
        if (pushNowState.retries < PUSH_NOW_MAX_RETRIES) {
            pushNowState.retries++;
            PERF_INC(pushNowRetries);
            pushNowState.nextAtMs = now + PUSH_NOW_RETRY_DELAY_MS;
            Serial.printf("[PushNow] %s deferred, retry %u/%u\n",
                          op,
                          static_cast<unsigned int>(pushNowState.retries),
                          static_cast<unsigned int>(PUSH_NOW_MAX_RETRIES));
            return;
        }
        Serial.printf("[PushNow] ERROR: %s failed after %u retries\n",
                      op,
                      static_cast<unsigned int>(PUSH_NOW_MAX_RETRIES));
        PERF_INC(pushNowFailures);
        pushNowState.step = PushNowStep::IDLE;
    };

    switch (pushNowState.step) {
        case PushNowStep::WRITE_PROFILE:
            if (bleClient.writeUserBytes(pushNowState.profileBytes)) {
                bleClient.startUserBytesVerification(pushNowState.profileBytes);
                bleClient.requestUserBytes();
                pushNowState.step = PushNowStep::SET_DISPLAY;
                pushNowState.retries = 0;
                pushNowState.nextAtMs = now + PUSH_NOW_RETRY_DELAY_MS;
                return;
            }
            scheduleRetry("writeUserBytes");
            return;

        case PushNowStep::SET_DISPLAY:
            if (bleClient.setDisplayOn(pushNowState.displayOn)) {
                pushNowState.step = PushNowStep::SET_MODE;
                pushNowState.retries = 0;
                pushNowState.nextAtMs = now + PUSH_NOW_RETRY_DELAY_MS;
                return;
            }
            scheduleRetry("setDisplayOn");
            return;

        case PushNowStep::SET_MODE:
            if (!pushNowState.applyMode || bleClient.setMode(static_cast<uint8_t>(pushNowState.mode))) {
                pushNowState.step = PushNowStep::SET_VOLUME;
                pushNowState.retries = 0;
                pushNowState.nextAtMs = now + PUSH_NOW_RETRY_DELAY_MS;
                return;
            }
            scheduleRetry("setMode");
            return;

        case PushNowStep::SET_VOLUME:
            if (!pushNowState.applyVolume || bleClient.setVolume(pushNowState.mainVol, pushNowState.muteVol)) {
                Serial.println("[PushNow] Complete");
                pushNowState.step = PushNowStep::IDLE;
                return;
            }
            scheduleRetry("setVolume");
            return;

        case PushNowStep::IDLE:
        default:
            return;
    }
}

void WiFiManager::checkWifiClientStatus() {
    // Skip if WiFi client is disabled
    if (wifiClientState == WIFI_CLIENT_DISABLED) {
        return;
    }
    
    wl_status_t status = WiFi.status();
    
    switch (wifiClientState) {
        case WIFI_CLIENT_CONNECTING: {
            // Non-blocking mode transition is still in progress.
            if (wifiConnectPhase != WifiConnectPhase::IDLE || wifiConnectStartMs == 0) {
                break;
            }

            if (status == WL_CONNECTED) {
                wifiClientState = WIFI_CLIENT_CONNECTED;
                wifiConnectStartMs = 0;
                Serial.printf("[WiFiClient] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
                
                // Reset failure counter on successful connection
                wifiReconnectFailures = 0;
                
                // WiFi stable - resume SD writes (NVS contention window closed)
                debugLogger.notifyWifiTransition(true);
                
                // Save credentials on successful connection
                if (pendingConnectSSID.length() > 0) {
                    settingsManager.setWifiClientCredentials(pendingConnectSSID, pendingConnectPassword);
                    pendingConnectSSID = "";
                    pendingConnectPassword = "";
                }
            } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
                wifiClientState = WIFI_CLIENT_FAILED;
                Serial.printf("[WiFiClient] Connection failed: %d\n", status);
                wifiConnectStartMs = 0;
                
                // WiFi stable (failed but not transitioning) - resume SD writes
                debugLogger.notifyWifiTransition(true);
                
                pendingConnectSSID = "";
                pendingConnectPassword = "";
            } else if (millis() - wifiConnectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
                wifiClientState = WIFI_CLIENT_FAILED;
                Serial.println("[WiFiClient] Connection timeout");
                WiFi.disconnect(false);
                wifiConnectStartMs = 0;
                
                // WiFi stable (timed out) - resume SD writes
                debugLogger.notifyWifiTransition(true);
                
                pendingConnectSSID = "";
                pendingConnectPassword = "";
            }
            break;
        }
        
        case WIFI_CLIENT_CONNECTED: {
            if (status != WL_CONNECTED) {
                wifiClientState = WIFI_CLIENT_DISCONNECTED;
                Serial.println("[WiFiClient] Lost connection");
                
                // WiFi transitioning - defer SD writes to avoid NVS flash contention
                debugLogger.notifyWifiTransition(false);
            }
            break;
        }
        
        case WIFI_CLIENT_DISCONNECTED:
        case WIFI_CLIENT_FAILED: {
            if (lowDmaCooldownRemainingMs() > 0) {
                break;
            }

            // Defer background STA reconnect attempts during early boot until V1 is
            // connected. This protects BLE acquisition from AP+STA mode churn.
            bool v1Connected = isV1Connected ? isV1Connected() : bleClient.isConnected();
            bool withinBootGrace = (setupModeStartTime != 0) &&
                                   ((millis() - setupModeStartTime) < WIFI_RECONNECT_DEFER_NO_V1_MS);
            if (!v1Connected && withinBootGrace) {
                if (!wifiReconnectDeferredLogged) {
                    Serial.printf("[WiFiClient] Auto-reconnect deferred (waiting for V1 or %lu ms grace)\n",
                                  (unsigned long)WIFI_RECONNECT_DEFER_NO_V1_MS);
                    wifiReconnectDeferredLogged = true;
                }
                break;
            }
            if (wifiReconnectDeferredLogged) {
                Serial.println("[WiFiClient] Auto-reconnect resumed");
                wifiReconnectDeferredLogged = false;
            }

            // Auto-reconnect if we have saved credentials (with failure limit)
            const V1Settings& settings = settingsManager.get();
            if (settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0) {
                // Check if we've exceeded max failures - prevents memory exhaustion
                if (wifiReconnectFailures >= WIFI_MAX_RECONNECT_FAILURES) {
                    // Already gave up - don't log spam, just stay in failed state
                    break;
                }
                
                // Only try auto-reconnect every 30 seconds (first attempt is immediate).
                static unsigned long lastReconnectAttempt = 0;
                unsigned long nowMs = millis();
                if (lastReconnectAttempt == 0 || (nowMs - lastReconnectAttempt) > WIFI_RECONNECT_INTERVAL_MS) {
                    String savedPassword = settingsManager.getWifiClientPassword();
                    lastReconnectAttempt = nowMs;
                    wifiReconnectFailures++;
                    
                    if (wifiReconnectFailures >= WIFI_MAX_RECONNECT_FAILURES) {
                        Serial.printf("[WiFiClient] Giving up after %d failed attempts. Use BOOT button to retry.\n",
                                      wifiReconnectFailures);
                        // Stay in FAILED state, user must toggle WiFi to retry
                        break;
                    }
                    
                    Serial.printf("[WiFiClient] Auto-reconnect attempt %d/%d...\n",
                                  wifiReconnectFailures, WIFI_MAX_RECONNECT_FAILURES);
                    connectToNetwork(settings.wifiClientSSID, savedPassword);
                }
            }
            break;
        }
        
        default:
            break;
    }
}

// ==================== API Endpoints ====================

void WiFiManager::handleNotFound() {
    String uri = server.uri();
    
    // Try to serve HTML pages from LittleFS (SvelteKit pre-rendered pages)
    if (uri.endsWith(".html") || uri.indexOf('.') == -1) {
        String path = uri;
        if (uri.indexOf('.') == -1) {
            // No extension - try adding .html
            path = uri + ".html";
        }
        if (serveLittleFSFile(path.c_str(), "text/html")) {
            return;
        }
    }
    
    // Try to serve static files (js, css, json, etc.)
    String contentType = "application/octet-stream";
    if (uri.endsWith(".js")) contentType = "application/javascript";
    else if (uri.endsWith(".css")) contentType = "text/css";
    else if (uri.endsWith(".json")) contentType = "application/json";
    else if (uri.endsWith(".html")) contentType = "text/html";
    else if (uri.endsWith(".svg")) contentType = "image/svg+xml";
    else if (uri.endsWith(".png")) contentType = "image/png";
    else if (uri.endsWith(".ico")) contentType = "image/x-icon";
    
    if (serveLittleFSFile(uri.c_str(), contentType.c_str())) {
        return;
    }
    
    Serial.printf("[HTTP] 404 %s\n", uri.c_str());
    server.send(404, "text/plain", "Not found");
}

bool WiFiManager::serveLittleFSFile(const char* path, const char* contentType) {
    return serveLittleFSFileHelper(server, path, contentType);
}


// ============= Auto-Push Handlers =============

// ============= Display Colors Handlers =============

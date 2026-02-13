/**
 * WiFi Manager for V1 Gen2 Display
 * AP+STA: always-on access point serving the local UI/API
 *         plus optional station mode to connect to external network
 */

#include "wifi_manager.h"
#include "perf_metrics.h"
#include "settings.h"
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
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_lockout_safety.h"
#include "modules/gps/gps_observation_log.h"
#include "modules/speed/speed_source_selector.h"
#include "time_service.h"
#include "modules/system/system_event_bus.h"
#include "../include/config.h"
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
// Preview hold helper to keep color demo visible briefly
extern void requestColorPreviewHold(uint32_t durationMs);
extern bool isColorPreviewRunning();
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

static bool parseUint64Strict(const String& input, uint64_t& out) {
    if (input.length() == 0) {
        return false;
    }
    char* end = nullptr;
    const char* raw = input.c_str();
    unsigned long long v = strtoull(raw, &end, 10);
    if (end == raw || *end != '\0') {
        return false;
    }
    out = static_cast<uint64_t>(v);
    return true;
}

static uint8_t clampU8Value(int value, int minVal, int maxVal) {
    return static_cast<uint8_t>(std::max(minVal, std::min(value, maxVal)));
}

static uint16_t clampU16Value(int value, int minVal, int maxVal) {
    return static_cast<uint16_t>(std::max(minVal, std::min(value, maxVal)));
}

static uint8_t clampSlotVolumeValue(int value) {
    if (value == 0xFF) {
        return 0xFF;
    }
    return clampU8Value(value, 0, 9);
}

static uint8_t clampApTimeoutValue(int value) {
    if (value == 0) {
        return 0;
    }
    return clampU8Value(value, 5, 60);
}

static V1Mode normalizeV1ModeValue(int raw) {
    switch (raw) {
        case V1_MODE_UNKNOWN:
        case V1_MODE_ALL_BOGEYS:
        case V1_MODE_LOGIC:
        case V1_MODE_ADVANCED_LOGIC:
            return static_cast<V1Mode>(raw);
        default:
            return V1_MODE_UNKNOWN;
    }
}

static constexpr size_t MAX_WIFI_SSID_LEN = 32;
static constexpr size_t MAX_AP_PASSWORD_LEN = 63;
static constexpr size_t MAX_PROXY_NAME_LEN = 32;
static constexpr size_t MAX_SLOT_NAME_LEN = 20;
static constexpr size_t MAX_PROFILE_NAME_LEN = 64;
static constexpr size_t MAX_PROFILE_DESCRIPTION_LEN = 160;

static String clampStringLength(const String& value, size_t maxLen) {
    if (value.length() <= maxLen) {
        return value;
    }
    return value.substring(0, maxLen);
}

static String sanitizeApSsidValue(const String& raw) {
    String value = clampStringLength(raw, MAX_WIFI_SSID_LEN);
    if (value.length() == 0) {
        return "V1-Simple";
    }
    return value;
}

static String sanitizeWifiClientSsidValue(const String& raw) {
    return clampStringLength(raw, MAX_WIFI_SSID_LEN);
}

static String sanitizeProxyNameValue(const String& raw) {
    String value = clampStringLength(raw, MAX_PROXY_NAME_LEN);
    if (value.length() == 0) {
        return "V1-Proxy";
    }
    return value;
}

static String sanitizeSlotNameValue(const String& raw) {
    String value = clampStringLength(raw, MAX_SLOT_NAME_LEN);
    value.toUpperCase();
    return value;
}

static String sanitizeProfileNameValue(const String& raw) {
    return clampStringLength(raw, MAX_PROFILE_NAME_LEN);
}

static String sanitizeProfileDescriptionValue(const String& raw) {
    return clampStringLength(raw, MAX_PROFILE_DESCRIPTION_LEN);
}

static String fileNameFromPath(const String& path) {
    int slash = path.lastIndexOf('/');
    if (slash >= 0) {
        return path.substring(slash + 1);
    }
    return path;
}

static bool isValidPerfFileName(const String& name) {
    if (name.length() == 0 || name.length() > 64) {
        return false;
    }
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0) {
        return false;
    }
    if (name == "perf.csv") {
        return true;  // Legacy fallback filename.
    }
    if (!name.startsWith("perf_boot_") || !name.endsWith(".csv")) {
        return false;
    }
    int digitStart = strlen("perf_boot_");
    int digitEnd = name.length() - 4;  // Exclude ".csv"
    if (digitEnd <= digitStart) {
        return false;
    }
    for (int i = digitStart; i < digitEnd; ++i) {
        char c = name.charAt(i);
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

static bool perfFilePathFromName(const String& name, String& outPath) {
    if (!isValidPerfFileName(name)) {
        return false;
    }
    outPath = "/perf/" + name;
    return true;
}

static bool shouldUseApSta(const V1Settings& settings) {
    return settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0;
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
        server.send(429, "text/plain", "Too Many Requests");
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
    
    // New API endpoints (PHASE A)
    server.on("/api/status", HTTP_GET, [this]() { 
        if (!checkRateLimit()) return;
        handleStatus(); 
    });
    server.on("/api/profile/push", HTTP_POST, [this]() { 
        if (!checkRateLimit()) return;
        handleApiProfilePush(); 
    });
    server.on("/api/time/set", HTTP_POST, [this]() {
        if (!checkRateLimit()) return;
        handleTimeSet();
    });
    
    // Legacy status endpoint
    server.on("/status", HTTP_GET, [this]() { handleStatus(); });
    server.on("/api/settings", HTTP_GET, [this]() { handleSettingsApi(); });  // JSON settings for new UI
    server.on("/api/settings", HTTP_POST, [this]() { handleSettingsSave(); });  // Consistent API endpoint
    
    // Legacy HTML page routes - redirect to root (SvelteKit handles routing)
    server.on("/settings", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    server.on("/settings", HTTP_POST, [this]() { handleSettingsSave(); });  // Legacy compat
    server.on("/darkmode", HTTP_POST, [this]() { handleDarkMode(); });
    server.on("/mute", HTTP_POST, [this]() { handleMute(); });
    
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
    server.on("/api/v1/profiles", HTTP_GET, [this]() { handleV1ProfilesList(); });
    server.on("/api/v1/profile", HTTP_GET, [this]() { handleV1ProfileGet(); });
    server.on("/api/v1/profile", HTTP_POST, [this]() { handleV1ProfileSave(); });
    server.on("/api/v1/profile/delete", HTTP_POST, [this]() { handleV1ProfileDelete(); });
    server.on("/api/v1/pull", HTTP_POST, [this]() { handleV1SettingsPull(); });
    server.on("/api/v1/push", HTTP_POST, [this]() { handleV1SettingsPush(); });
    server.on("/api/v1/current", HTTP_GET, [this]() { handleV1CurrentSettings(); });
    
    // Auto-Push routes
    server.on("/autopush", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    server.on("/api/autopush/slots", HTTP_GET, [this]() { handleAutoPushSlotsApi(); });
    server.on("/api/autopush/slot", HTTP_POST, [this]() { handleAutoPushSlotSave(); });
    server.on("/api/autopush/activate", HTTP_POST, [this]() { handleAutoPushActivate(); });
    server.on("/api/autopush/push", HTTP_POST, [this]() { handleAutoPushPushNow(); });
    server.on("/api/autopush/status", HTTP_GET, [this]() { handleAutoPushStatus(); });
    
    // Display Colors routes
    server.on("/displaycolors", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    server.on("/api/displaycolors", HTTP_GET, [this]() { handleDisplayColorsApi(); });
    server.on("/api/displaycolors", HTTP_POST, [this]() { handleDisplayColorsSave(); });
    server.on("/api/displaycolors/reset", HTTP_POST, [this]() { handleDisplayColorsReset(); });
    server.on("/api/displaycolors/preview", HTTP_POST, [this]() { 
        if (!checkRateLimit()) return;
        if (isColorPreviewRunning()) {
            Serial.println("[HTTP] POST /api/displaycolors/preview - toggling off");
            cancelColorPreview();
            // main.cpp loop handles display restore based on V1 connection state
            server.send(200, "application/json", "{\"success\":true,\"active\":false}");
        } else {
            Serial.println("[HTTP] POST /api/displaycolors/preview - starting");
            display.showDemo();
            requestColorPreviewHold(5500);
            server.send(200, "application/json", "{\"success\":true,\"active\":true}");
        }
    });
    server.on("/api/displaycolors/clear", HTTP_POST, [this]() { 
        if (!checkRateLimit()) return;
        Serial.println("[HTTP] POST /api/displaycolors/clear - cancelling preview");
        cancelColorPreview();
        // main.cpp loop handles display restore based on V1 connection state
        server.send(200, "application/json", "{\"success\":true,\"active\":false}");
    });
    
    // Settings backup/restore API routes
    server.on("/api/settings/backup", HTTP_GET, [this]() { handleSettingsBackup(); });
    server.on("/api/settings/restore", HTTP_POST, [this]() { handleSettingsRestore(); });
    
    // Debug API routes (performance metrics)
    server.on("/api/debug/metrics", HTTP_GET, [this]() { handleDebugMetrics(); });
    server.on("/api/debug/panic", HTTP_GET, [this]() { handleDebugPanic(); });
    server.on("/api/debug/enable", HTTP_POST, [this]() { handleDebugEnable(); });
    server.on("/api/debug/perf-files", HTTP_GET, [this]() { handleDebugPerfFilesList(); });
    server.on("/api/debug/perf-files/download", HTTP_GET, [this]() { handleDebugPerfFileDownload(); });
    server.on("/api/debug/perf-files/delete", HTTP_POST, [this]() { handleDebugPerfFileDelete(); });
    
    // WiFi client (STA) API routes - connect to external network
    server.on("/api/wifi/status", HTTP_GET, [this]() { handleWifiClientStatus(); });
    server.on("/api/wifi/scan", HTTP_POST, [this]() { handleWifiClientScan(); });
    server.on("/api/wifi/connect", HTTP_POST, [this]() { handleWifiClientConnect(); });
    server.on("/api/wifi/disconnect", HTTP_POST, [this]() { handleWifiClientDisconnect(); });
    server.on("/api/wifi/forget", HTTP_POST, [this]() { handleWifiClientForget(); });
    server.on("/api/wifi/enable", HTTP_POST, [this]() { handleWifiClientEnable(); });

    // OBD integration API routes
    server.on("/api/obd/status", HTTP_GET, [this]() { handleObdStatus(); });
    server.on("/api/obd/scan", HTTP_POST, [this]() { handleObdScan(); });
    server.on("/api/obd/scan/stop", HTTP_POST, [this]() { handleObdScanStop(); });
    server.on("/api/obd/devices", HTTP_GET, [this]() { handleObdDevices(); });
    server.on("/api/obd/devices/clear", HTTP_POST, [this]() { handleObdDevicesClear(); });
    server.on("/api/obd/connect", HTTP_POST, [this]() { handleObdConnect(); });
    server.on("/api/obd/disconnect", HTTP_POST, [this]() { handleObdDisconnect(); });
    server.on("/api/obd/config", HTTP_POST, [this]() { handleObdConfig(); });
    server.on("/api/obd/remembered", HTTP_GET, [this]() { handleObdRemembered(); });
    server.on("/api/obd/remembered/autoconnect", HTTP_POST, [this]() { handleObdRememberedAutoConnect(); });
    server.on("/api/obd/forget", HTTP_POST, [this]() { handleObdForget(); });

    // GPS scaffold API routes
    server.on("/api/gps/status", HTTP_GET, [this]() { handleGpsStatus(); });
    server.on("/api/gps/observations", HTTP_GET, [this]() { handleGpsObservations(); });
    server.on("/api/gps/config", HTTP_POST, [this]() { handleGpsConfig(); });
    
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

void WiFiManager::handleStatus() {
    // Option 2 optimization: Cache status JSON for 500ms to avoid repeated serialization
    unsigned long now = millis();
    bool cacheValid = (now - lastStatusJsonTime) < STATUS_CACHE_TTL_MS;
    
    if (!cacheValid) {
        // Cache expired or uninitialized - rebuild JSON
        const V1Settings& settings = settingsManager.get();
        
        JsonDocument doc;
        
        // WiFi info (matches Svelte dashboard expectations)
        JsonObject wifi = doc["wifi"].to<JsonObject>();
        wifi["setup_mode"] = (setupModeState == SETUP_MODE_AP_ON);
        wifi["ap_active"] = (setupModeState == SETUP_MODE_AP_ON);
        wifi["sta_connected"] = (wifiClientState == WIFI_CLIENT_CONNECTED);
        wifi["sta_ip"] = (wifiClientState == WIFI_CLIENT_CONNECTED) ? WiFi.localIP().toString() : "";
        wifi["ap_ip"] = getAPIPAddress();
        wifi["ssid"] = (wifiClientState == WIFI_CLIENT_CONNECTED) ? WiFi.SSID() : settings.apSSID;
        wifi["rssi"] = (wifiClientState == WIFI_CLIENT_CONNECTED) ? WiFi.RSSI() : 0;
        wifi["sta_enabled"] = settings.wifiClientEnabled;
        wifi["sta_ssid"] = settings.wifiClientSSID;
        
        // Device info
        JsonObject device = doc["device"].to<JsonObject>();
        device["uptime"] = millis() / 1000;
        device["heap_free"] = ESP.getFreeHeap();
        device["hostname"] = "v1g2";
        device["firmware_version"] = FIRMWARE_VERSION;

        // Safe clock status (explicitly set from trusted source, no background sync).
        JsonObject time = doc["time"].to<JsonObject>();
        const bool timeValid = timeService.timeValid();
        time["valid"] = timeValid;
        time["source"] = timeService.timeSource();
        time["confidence"] = timeService.timeConfidence();
        time["tzOffsetMin"] = timeService.tzOffsetMinutes();
        time["tzOffsetMinutes"] = timeService.tzOffsetMinutes();
        if (timeValid) {
            time["epochMs"] = timeService.nowEpochMsOr0();
            time["ageMs"] = timeService.epochAgeMsOr0();
        }

        // Battery info
        JsonObject battery = doc["battery"].to<JsonObject>();
        battery["voltage_mv"] = batteryManager.getVoltageMillivolts();
        battery["percentage"] = batteryManager.getPercentage();
        battery["on_battery"] = batteryManager.isOnBattery();
        battery["has_battery"] = batteryManager.hasBattery();
        
        // BLE/V1 connection state
        doc["v1_connected"] = bleClient.isConnected();
        
        // Append callback data if available (legacy support)
        if (getStatusJson) {
            JsonDocument statusDoc;
            deserializeJson(statusDoc, getStatusJson());
            for (JsonPair kv : statusDoc.as<JsonObject>()) {
                doc[kv.key()] = kv.value();
            }
        }
        if (getAlertJson) {
            JsonDocument alertDoc;
            deserializeJson(alertDoc, getAlertJson());
            doc["alert"] = alertDoc;
        }
        
        serializeJson(doc, cachedStatusJson);
        lastStatusJsonTime = now;
    }
    
    server.send(200, "application/json", cachedStatusJson);
}

// ==================== API Endpoints ====================

void WiFiManager::handleApiProfilePush() {
    // Queue profile push action (non-blocking)
    // This endpoint triggers the push executor to apply active profile
    
    if (!checkRateLimit()) return;
    
    // Check if V1 is connected
    if (!bleClient.isConnected()) {
        server.send(503, "application/json", 
                   "{\"error\":\"V1 not connected\"}");
        return;
    }
    
    // Invoke the registered callback to kick off the auto-push state machine
    bool queued = false;
    if (requestProfilePush) {
        queued = requestProfilePush();
    }
    
    JsonDocument doc;
    doc["ok"] = queued;
    if (queued) {
        doc["message"] = "Profile push queued - check display for progress";
    } else {
        doc["error"] = "Push handler unavailable";
    }
    
    String json;
    serializeJson(doc, json);
    server.send(queued ? 200 : 500, "application/json", json);
}

void WiFiManager::handleTimeSet() {
    uint64_t unixMs = 0;
    int32_t tzOffsetMin = 0;
    bool haveUnixMs = false;
    bool sourceIsClient = true;

    // Preferred JSON: {"unixMs": <ms>, "tzOffsetMin": <minutes>, "source":"client"}
    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (!err) {
            if (doc["source"].is<const char*>()) {
                String source = doc["source"].as<String>();
                source.toLowerCase();
                sourceIsClient = (source.length() == 0 || source == "client");
            }

            if (doc["unixMs"].is<const char*>()) {
                haveUnixMs = parseUint64Strict(doc["unixMs"].as<String>(), unixMs);
            } else if (doc["unixMs"].is<uint64_t>()) {
                unixMs = doc["unixMs"].as<uint64_t>();
                haveUnixMs = true;
            } else if (doc["epochMs"].is<const char*>()) {
                // Compatibility key
                haveUnixMs = parseUint64Strict(doc["epochMs"].as<String>(), unixMs);
            } else if (doc["epochMs"].is<uint64_t>()) {
                unixMs = doc["epochMs"].as<uint64_t>();
                haveUnixMs = true;
            } else if (doc["clientEpochMs"].is<const char*>()) {
                // Compatibility key
                haveUnixMs = parseUint64Strict(doc["clientEpochMs"].as<String>(), unixMs);
            } else if (doc["clientEpochMs"].is<uint64_t>()) {
                unixMs = doc["clientEpochMs"].as<uint64_t>();
                haveUnixMs = true;
            }

            if (doc["tzOffsetMin"].is<int32_t>()) {
                tzOffsetMin = doc["tzOffsetMin"].as<int32_t>();
            } else if (doc["tzOffsetMinutes"].is<int32_t>()) {
                // Compatibility key
                tzOffsetMin = doc["tzOffsetMinutes"].as<int32_t>();
            }
        }
    }

    // Compatibility fallback: form/query args
    if (!haveUnixMs) {
        if (server.hasArg("unixMs")) {
            haveUnixMs = parseUint64Strict(server.arg("unixMs"), unixMs);
        } else if (server.hasArg("epochMs")) {
            haveUnixMs = parseUint64Strict(server.arg("epochMs"), unixMs);
        } else if (server.hasArg("clientEpochMs")) {
            haveUnixMs = parseUint64Strict(server.arg("clientEpochMs"), unixMs);
        }
    }
    if (server.hasArg("tzOffsetMin")) {
        tzOffsetMin = server.arg("tzOffsetMin").toInt();
    } else if (server.hasArg("tzOffsetMinutes")) {
        tzOffsetMin = server.arg("tzOffsetMinutes").toInt();
    }
    if (server.hasArg("source")) {
        String source = server.arg("source");
        source.toLowerCase();
        sourceIsClient = (source.length() == 0 || source == "client");
    }

    if (!sourceIsClient) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Unsupported source\"}");
        return;
    }

    if (!haveUnixMs) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing or invalid unixMs\"}");
        return;
    }

    // Sanity range: >= ~2023-11 and <= 2100
    static constexpr uint64_t MIN_VALID_UNIX_MS = 1700000000000ULL;
    static constexpr uint64_t MAX_VALID_UNIX_MS = 4102444800000ULL;
    if (unixMs < MIN_VALID_UNIX_MS || unixMs > MAX_VALID_UNIX_MS) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"unixMs out of range\"}");
        return;
    }

    if (tzOffsetMin < -840) tzOffsetMin = -840;
    if (tzOffsetMin > 840) tzOffsetMin = 840;

    const int64_t requestedEpochMs = static_cast<int64_t>(unixMs);
    const bool hasExistingTime = timeService.timeValid();
    const int64_t existingEpochMs = hasExistingTime ? timeService.nowEpochMsOr0() : 0;
    const int32_t existingTzOffsetMin = timeService.tzOffsetMinutes();
    const uint8_t existingSource = timeService.timeSource();
    static constexpr int64_t TIME_SET_NOOP_DELTA_MS = 2000LL;
    const bool nearNoopClientSync = hasExistingTime
        && existingSource == TimeService::SOURCE_CLIENT_AP
        && existingTzOffsetMin == tzOffsetMin
        && llabs(requestedEpochMs - existingEpochMs) <= TIME_SET_NOOP_DELTA_MS;

    if (!nearNoopClientSync) {
        timeService.setEpochBaseMs(
            requestedEpochMs,
            tzOffsetMin,
            TimeService::SOURCE_CLIENT_AP);
        lastStatusJsonTime = 0;  // Invalidate cached /api/status response.
    }

    JsonDocument response;
    response["ok"] = true;
    response["timeValid"] = timeService.timeValid();
    response["timeSource"] = timeService.timeSource();
    response["timeConfidence"] = timeService.timeConfidence();
    response["epochMs"] = timeService.nowEpochMsOr0();
    response["tzOffsetMin"] = timeService.tzOffsetMinutes();

    // Backward-compatible fields
    response["success"] = true;
    response["monoMs"] = timeService.nowMonoMs();
    response["epochAgeMs"] = timeService.epochAgeMsOr0();
    response["tzOffsetMinutes"] = timeService.tzOffsetMinutes();

    String json;
    serializeJson(response, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleSettingsApi() {
    const V1Settings& settings = settingsManager.get();
    
    JsonDocument doc;
    doc["ap_ssid"] = settings.apSSID;
    doc["ap_password"] = "********";  // Don't send actual password
    doc["isDefaultPassword"] = (settings.apPassword == "setupv1g2");  // Security warning flag
    doc["proxy_ble"] = settings.proxyBLE;
    doc["proxy_name"] = settings.proxyName;
    doc["obdVwDataEnabled"] = settings.obdVwDataEnabled;
    doc["gpsEnabled"] = settings.gpsEnabled;
    doc["gpsLockoutMode"] = static_cast<int>(settings.gpsLockoutMode);
    doc["gpsLockoutModeName"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    doc["gpsLockoutCoreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    doc["gpsLockoutMaxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    doc["gpsLockoutMaxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    doc["gpsLockoutMaxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    doc["displayStyle"] = static_cast<int>(settings.displayStyle);
    doc["autoPowerOffMinutes"] = settings.autoPowerOffMinutes;
    doc["apTimeoutMinutes"] = settings.apTimeoutMinutes;
    
    // Development settings
    doc["enableWifiAtBoot"] = settings.enableWifiAtBoot;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleSettingsSave() {
    if (!checkRateLimit()) return;
    
    Serial.println("=== handleSettingsSave() called ===");
    V1Settings& mutableSettings = settingsManager.mutableSettings();
    const V1Settings& currentSettings = mutableSettings;

    if (server.hasArg("ap_ssid")) {
        String apSsid = clampStringLength(server.arg("ap_ssid"), MAX_WIFI_SSID_LEN);
        String apPass = server.arg("ap_password");
        if (apPass.length() > MAX_AP_PASSWORD_LEN && apPass != "********") {
            apPass = apPass.substring(0, MAX_AP_PASSWORD_LEN);
        }
        
        // If password is placeholder, keep existing password
        if (apPass == "********") {
            apPass = currentSettings.apPassword;
        }
        
        if (apSsid.length() == 0 || apPass.length() < 8) {
            server.send(400, "application/json", "{\"error\":\"AP SSID required and password must be at least 8 characters\"}");
            return;
        }
        settingsManager.updateAPCredentials(apSsid, apPass);
    }
    
    if (server.hasArg("brightness")) {
        int brightness = server.arg("brightness").toInt();
        brightness = std::max(0, std::min(brightness, 255));
        settingsManager.updateBrightness((uint8_t)brightness);
    }

    // BLE proxy settings
    if (server.hasArg("proxy_ble")) {
        bool proxyEnabled = server.arg("proxy_ble") == "true" || server.arg("proxy_ble") == "1";
        mutableSettings.proxyBLE = proxyEnabled;
    }
    if (server.hasArg("proxy_name")) {
        mutableSettings.proxyName = sanitizeProxyNameValue(server.arg("proxy_name"));
    }
    if (server.hasArg("obdVwDataEnabled")) {
        mutableSettings.obdVwDataEnabled =
            (server.arg("obdVwDataEnabled") == "true" || server.arg("obdVwDataEnabled") == "1");
        obdHandler.setVwDataEnabled(mutableSettings.obdVwDataEnabled);
    }
    if (server.hasArg("gpsEnabled")) {
        mutableSettings.gpsEnabled =
            (server.arg("gpsEnabled") == "true" || server.arg("gpsEnabled") == "1");
        gpsRuntimeModule.setEnabled(mutableSettings.gpsEnabled);
        speedSourceSelector.setGpsEnabled(mutableSettings.gpsEnabled);
    }
    if (server.hasArg("gpsLockoutMode")) {
        mutableSettings.gpsLockoutMode = gpsLockoutParseRuntimeModeArg(server.arg("gpsLockoutMode"),
                                                                       mutableSettings.gpsLockoutMode);
    }
    if (server.hasArg("gpsLockoutCoreGuardEnabled")) {
        mutableSettings.gpsLockoutCoreGuardEnabled =
            (server.arg("gpsLockoutCoreGuardEnabled") == "true" ||
             server.arg("gpsLockoutCoreGuardEnabled") == "1");
    }
    if (server.hasArg("gpsLockoutMaxQueueDrops")) {
        mutableSettings.gpsLockoutMaxQueueDrops =
            clampU16Value(server.arg("gpsLockoutMaxQueueDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutMaxPerfDrops")) {
        mutableSettings.gpsLockoutMaxPerfDrops =
            clampU16Value(server.arg("gpsLockoutMaxPerfDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutMaxEventBusDrops")) {
        mutableSettings.gpsLockoutMaxEventBusDrops =
            clampU16Value(server.arg("gpsLockoutMaxEventBusDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("autoPowerOffMinutes")) {
        int minutes = server.arg("autoPowerOffMinutes").toInt();
        minutes = std::max(0, std::min(minutes, 60));  // Clamp 0-60 minutes
        mutableSettings.autoPowerOffMinutes = static_cast<uint8_t>(minutes);
    }
    if (server.hasArg("apTimeoutMinutes")) {
        int minutes = server.arg("apTimeoutMinutes").toInt();
        // Clamp: 0=always on, or 5-60 minutes
        if (minutes != 0) {
            minutes = std::max(5, std::min(minutes, 60));
        }
        mutableSettings.apTimeoutMinutes = static_cast<uint8_t>(minutes);
    }

    // Display style setting
    if (server.hasArg("displayStyle")) {
        DisplayStyle style = normalizeDisplayStyle(server.arg("displayStyle").toInt());
        settingsManager.updateDisplayStyle(style);
        display.forceNextRedraw();  // Force display update to show new font style
    }

    // All changes are queued in the settingsManager instance. Now, save them all at once.
    Serial.println("--- Calling settingsManager.save() ---");
    settingsManager.save();
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleDarkMode() {
    if (!checkRateLimit()) return;
    
    if (!server.hasArg("state")) {
        server.send(400, "application/json", "{\"error\":\"Missing state parameter\"}");
        return;
    }
    
    bool darkMode = server.arg("state") == "1" || server.arg("state") == "true";
    bool success = false;
    
    if (sendV1Command) {
        // Dark mode = display OFF, so invert the parameter
        success = sendV1Command("display", !darkMode);
    }
    
    Serial.printf("Dark mode request: %s, success: %s\n", darkMode ? "ON" : "OFF", success ? "yes" : "no");
    
    JsonDocument doc;
    doc["success"] = success;
    doc["darkMode"] = darkMode;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleMute() {
    if (!checkRateLimit()) return;
    
    if (!server.hasArg("state")) {
        server.send(400, "application/json", "{\"error\":\"Missing state parameter\"}");
        return;
    }
    
    bool muted = server.arg("state") == "1" || server.arg("state") == "true";
    bool success = false;
    
    if (sendV1Command) {
        success = sendV1Command("mute", muted);
    }
    
    Serial.printf("Mute request: %s, success: %s\n", muted ? "ON" : "OFF", success ? "yes" : "no");
    
    JsonDocument doc;
    doc["success"] = success;
    doc["muted"] = muted;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleV1ProfilesList() {
    std::vector<String> profileNames = v1ProfileManager.listProfiles();
    Serial.printf("[V1Profiles] Listing %d profiles\n", profileNames.size());
    
    JsonDocument doc;
    JsonArray array = doc["profiles"].to<JsonArray>();
    
    for (const String& name : profileNames) {
        V1Profile profile;
        if (v1ProfileManager.loadProfile(name, profile)) {
            JsonObject obj = array.add<JsonObject>();
            obj["name"] = profile.name;
            obj["description"] = profile.description;
            obj["displayOn"] = profile.displayOn;
            Serial.printf("[V1Profiles]   - %s: %s\n", profile.name.c_str(), profile.description.c_str());
        }
    }
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleV1ProfileGet() {
    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }
    
    String name = server.arg("name");
    V1Profile profile;
    
    if (!v1ProfileManager.loadProfile(name, profile)) {
        server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
        return;
    }
    
    server.send(200, "application/json", v1ProfileManager.profileToJson(profile));
}

void WiFiManager::handleV1ProfileSave() {
    if (!checkRateLimit()) return;
    
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }
    
    String body = server.arg("plain");
    if (body.length() > 4096) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    Serial.printf("[V1Settings] Save request body: %s\n", body.c_str());
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    
    String name = doc["name"] | "";
    if (name.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }
    
    V1Profile profile;
    profile.name = name;
    profile.description = doc["description"] | "";
    profile.displayOn = doc["displayOn"] | true;  // Default to on
    
    // Parse settings from JSON
    JsonObject settingsObj = doc["settings"];
    if (!settingsObj.isNull()) {
        if (!v1ProfileManager.jsonToSettings(settingsObj, profile.settings)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
    } else {
        // Direct settings in root
        JsonObject rootObj = doc.as<JsonObject>();
        if (!v1ProfileManager.jsonToSettings(rootObj, profile.settings)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
    }
    
    ProfileSaveResult result = v1ProfileManager.saveProfile(profile);
    if (result.success) {
        settingsManager.backupToSD();
        Serial.printf("[V1Profiles] Profile '%s' saved successfully\n", profile.name.c_str());
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        Serial.printf("[V1Profiles] Failed to save profile '%s': %s\n", profile.name.c_str(), result.error.c_str());
        String errorJson = "{\"error\":\"" + result.error + "\"}";
        server.send(500, "application/json", errorJson);
    }
}

void WiFiManager::handleV1ProfileDelete() {
    if (!checkRateLimit()) return;
    
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }
    
    String body = server.arg("plain");
    if (body.length() > 2048) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    
    String name = doc["name"] | "";
    if (name.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }
    
    if (v1ProfileManager.deleteProfile(name)) {
        settingsManager.backupToSD();
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
    }
}

void WiFiManager::handleV1CurrentSettings() {
    JsonDocument doc;
    doc["connected"] = bleClient.isConnected();
    
    if (!v1ProfileManager.hasCurrentSettings()) {
        doc["available"] = false;
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json);
        return;
    }
    
    doc["available"] = true;
    // Parse existing settings JSON and embed it
    JsonDocument settingsDoc;
    deserializeJson(settingsDoc, v1ProfileManager.settingsToJson(v1ProfileManager.getCurrentSettings()));
    doc["settings"] = settingsDoc;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleV1SettingsPull() {
    if (!checkRateLimit()) return;
    
    if (!bleClient.isConnected()) {
        server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
        return;
    }
    
    // Request user bytes from V1
    if (bleClient.requestUserBytes()) {
        // Response will come async via BLE callback
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Request sent. Check current settings.\"}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to send request\"}");
    }
}

void WiFiManager::handleV1SettingsPush() {
    if (!checkRateLimit()) return;
    
    if (!bleClient.isConnected()) {
        server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
        return;
    }
    
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }
    
    String body = server.arg("plain");
    Serial.printf("[V1Settings] Push request: %s\n", body.c_str());
    if (body.length() > 4096) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    
    uint8_t bytes[6];
    bool displayOn = true;
    
    // Check if pushing a profile by name
    String profileName = doc["name"] | "";
    if (!profileName.isEmpty()) {
        // Load profile from database
        V1Profile profile;
        if (!v1ProfileManager.loadProfile(profileName, profile)) {
            server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
            return;
        }
        memcpy(bytes, profile.settings.bytes, 6);
        displayOn = profile.displayOn;
        Serial.printf("[V1Settings] Pushing profile '%s': %02X %02X %02X %02X %02X %02X\n",
            profileName.c_str(), bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    } 
    // Check for bytes array
    else if (doc["bytes"].is<JsonArray>()) {
        JsonArray bytesArray = doc["bytes"];
        if (bytesArray.size() != 6) {
            server.send(400, "application/json", "{\"error\":\"Invalid bytes array\"}");
            return;
        }
        for (int i = 0; i < 6; i++) {
            bytes[i] = bytesArray[i].as<uint8_t>();
        }
        displayOn = doc["displayOn"] | true;
        Serial.println("[V1Settings] Using raw bytes from request");
    } 
    // Parse from individual settings
    else {
        V1UserSettings settings;
        JsonObject settingsObj = doc["settings"].as<JsonObject>();
        if (settingsObj.isNull()) {
            settingsObj = doc.as<JsonObject>();
        }
        if (!v1ProfileManager.jsonToSettings(settingsObj, settings)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
        memcpy(bytes, settings.bytes, 6);
        displayOn = doc["displayOn"] | true;
        Serial.printf("[V1Settings] Built bytes from settings: %02X %02X %02X %02X %02X %02X\n",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    }
    
    // Perform write with retry
    V1BLEClient::WriteVerifyResult result = bleClient.writeUserBytesVerified(bytes, 3);
    
    if (result == V1BLEClient::VERIFY_OK) {
        Serial.println("[V1Settings] Push sent successfully");
        bleClient.setDisplayOn(displayOn);
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Settings sent to V1\"}");
    } else {
        Serial.println("[V1Settings] Push FAILED - write command rejected");
        server.send(500, "application/json", "{\"error\":\"Write command failed - check V1 connection\"}");
    }
}

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

void WiFiManager::handleAutoPushSlotsApi() {
    const V1Settings& s = settingsManager.get();
    
    JsonDocument doc;
    doc["enabled"] = s.autoPushEnabled;
    doc["activeSlot"] = s.activeSlot;
    
    JsonArray slots = doc["slots"].to<JsonArray>();
    
    // Slot 0
    JsonObject slot0 = slots.add<JsonObject>();
    slot0["name"] = s.slot0Name;
    slot0["profile"] = s.slot0_default.profileName;
    slot0["mode"] = s.slot0_default.mode;
    slot0["color"] = s.slot0Color;
    slot0["volume"] = s.slot0Volume;
    slot0["muteVolume"] = s.slot0MuteVolume;
    slot0["darkMode"] = s.slot0DarkMode;
    slot0["muteToZero"] = s.slot0MuteToZero;
    slot0["alertPersist"] = s.slot0AlertPersist;
    slot0["priorityArrowOnly"] = s.slot0PriorityArrow;
    
    // Slot 1
    JsonObject slot1 = slots.add<JsonObject>();
    slot1["name"] = s.slot1Name;
    slot1["profile"] = s.slot1_highway.profileName;
    slot1["mode"] = s.slot1_highway.mode;
    slot1["color"] = s.slot1Color;
    slot1["volume"] = s.slot1Volume;
    slot1["muteVolume"] = s.slot1MuteVolume;
    slot1["darkMode"] = s.slot1DarkMode;
    slot1["muteToZero"] = s.slot1MuteToZero;
    slot1["alertPersist"] = s.slot1AlertPersist;
    slot1["priorityArrowOnly"] = s.slot1PriorityArrow;
    
    // Slot 2
    JsonObject slot2 = slots.add<JsonObject>();
    slot2["name"] = s.slot2Name;
    slot2["profile"] = s.slot2_comfort.profileName;
    slot2["mode"] = s.slot2_comfort.mode;
    slot2["color"] = s.slot2Color;
    slot2["volume"] = s.slot2Volume;
    slot2["muteVolume"] = s.slot2MuteVolume;
    slot2["darkMode"] = s.slot2DarkMode;
    slot2["muteToZero"] = s.slot2MuteToZero;
    slot2["alertPersist"] = s.slot2AlertPersist;
    slot2["priorityArrowOnly"] = s.slot2PriorityArrow;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleAutoPushSlotSave() {
    if (!checkRateLimit()) return;
    
    if (!server.hasArg("slot") || !server.hasArg("profile") || !server.hasArg("mode")) {
        server.send(400, "application/json", "{\"error\":\"Missing parameters\"}");
        return;
    }
    
    int slot = server.arg("slot").toInt();
    String profile = server.arg("profile");
    int mode = server.arg("mode").toInt();
    String name = server.hasArg("name") ? server.arg("name") : "";
    int color = server.hasArg("color") ? server.arg("color").toInt() : -1;
    int volume = server.hasArg("volume") ? server.arg("volume").toInt() : -1;
    int muteVol = server.hasArg("muteVol") ? server.arg("muteVol").toInt() : -1;
    bool hasDarkMode = server.hasArg("darkMode");
    bool darkMode = hasDarkMode ? (server.arg("darkMode") == "true") : false;
    bool hasMuteToZero = server.hasArg("muteToZero");
    bool muteToZero = hasMuteToZero ? (server.arg("muteToZero") == "true") : false;
    bool hasAlertPersist = server.hasArg("alertPersist");
    int alertPersist = hasAlertPersist ? server.arg("alertPersist").toInt() : -1;
    
    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }
    
    // Save slot name if provided (limited to 20 chars by setSlotName)
    if (name.length() > 0) {
        settingsManager.setSlotName(slot, name);
    }
    
    // Save slot color if provided
    if (color >= 0) {
        settingsManager.setSlotColor(slot, static_cast<uint16_t>(color));
    }
    
    // Save slot volumes - preserve existing values if not provided
    uint8_t existingVol = settingsManager.getSlotVolume(slot);
    uint8_t existingMute = settingsManager.getSlotMuteVolume(slot);
    uint8_t vol = (volume >= 0) ? static_cast<uint8_t>(volume) : existingVol;
    uint8_t mute = (muteVol >= 0) ? static_cast<uint8_t>(muteVol) : existingMute;
    
    Serial.printf("[SaveSlot] Slot %d - volume: %d (was %d), muteVol: %d (was %d)\n", 
                  slot, vol, existingVol, mute, existingMute);
    
    settingsManager.setSlotVolumes(slot, vol, mute);
    
    // Save dark mode and MZ if provided
    Serial.printf("[SaveSlot] Slot %d - hasDarkMode: %s, darkMode: %s, hasMZ: %s, muteToZero: %s\n",
                  slot, hasDarkMode ? "yes" : "no", darkMode ? "true" : "false",
                  hasMuteToZero ? "yes" : "no", muteToZero ? "true" : "false");
    if (hasDarkMode) {
        settingsManager.setSlotDarkMode(slot, darkMode);
        Serial.printf("[SaveSlot] Saved darkMode=%s for slot %d\n", darkMode ? "true" : "false", slot);
    }
    if (hasMuteToZero) {
        settingsManager.setSlotMuteToZero(slot, muteToZero);
        Serial.printf("[SaveSlot] Saved muteToZero=%s for slot %d\n", muteToZero ? "true" : "false", slot);
    }
    
    // Save alert persistence (seconds, clamped 0-5)
    if (hasAlertPersist && alertPersist >= 0) {
        int clamped = std::max(0, std::min(5, alertPersist));
        settingsManager.setSlotAlertPersistSec(slot, static_cast<uint8_t>(clamped));
        Serial.printf("[SaveSlot] Saved alertPersist=%ds for slot %d\n", clamped, slot);
    }
    
    // Save priorityArrowOnly per slot
    if (server.hasArg("priorityArrowOnly")) {
        bool prioArrow = server.arg("priorityArrowOnly") == "true";
        settingsManager.setSlotPriorityArrowOnly(slot, prioArrow);
        Serial.printf("[SaveSlot] Saved priorityArrowOnly=%s for slot %d\n", prioArrow ? "true" : "false", slot);
    }
    
    settingsManager.setSlot(slot, profile, normalizeV1ModeValue(mode));
    
    // If this is the currently active slot, update the display immediately
    if (slot == settingsManager.get().activeSlot) {
        display.drawProfileIndicator(slot);
    }
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleAutoPushActivate() {
    if (!checkRateLimit()) return;
    
    if (!server.hasArg("slot")) {
        server.send(400, "application/json", "{\"error\":\"Missing slot parameter\"}");
        return;
    }
    
    int slot = server.arg("slot").toInt();
    bool enable = server.hasArg("enable") ? (server.arg("enable") == "true") : true;
    
    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }
    
    settingsManager.setActiveSlot(slot);
    settingsManager.setAutoPushEnabled(enable);
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleAutoPushPushNow() {
    if (!checkRateLimit()) return;
    
    if (!server.hasArg("slot")) {
        server.send(400, "application/json", "{\"error\":\"Missing slot parameter\"}");
        return;
    }
    
    int slot = server.arg("slot").toInt();
    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }

    if (!bleClient.isConnected()) {
        server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
        return;
    }

    if (pushNowState.step != PushNowStep::IDLE) {
        server.send(409, "application/json", "{\"error\":\"Push already in progress\"}");
        return;
    }
    
    // Check if profile/mode are passed directly (from Push Now button)
    String profileName;
    V1Mode mode = V1_MODE_UNKNOWN;
    
    if (server.hasArg("profile") && server.arg("profile").length() > 0) {
        // Use the form values directly
        profileName = sanitizeProfileNameValue(server.arg("profile"));
        if (server.hasArg("mode")) {
            mode = normalizeV1ModeValue(server.arg("mode").toInt());
        }
    } else {
        // Fall back to saved slot settings
        const V1Settings& s = settingsManager.get();
        AutoPushSlot pushSlot;
        
        switch (slot) {
            case 0: pushSlot = s.slot0_default; break;
            case 1: pushSlot = s.slot1_highway; break;
            case 2: pushSlot = s.slot2_comfort; break;
        }
        
        profileName = sanitizeProfileNameValue(pushSlot.profileName);
        mode = normalizeV1ModeValue(static_cast<int>(pushSlot.mode));
    }
    
    if (profileName.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"No profile configured for this slot\"}");
        return;
    }
    
    // Load profile once, then execute BLE writes via non-blocking loop state machine.
    V1Profile profile;
    if (!v1ProfileManager.loadProfile(profileName, profile)) {
        server.send(500, "application/json", "{\"error\":\"Failed to load profile\"}");
        return;
    }

    // Use slot's dark mode setting, not the profile's stored displayOn value
    // (slot dark mode is the user-facing toggle in auto-push config)
    bool slotDarkMode = settingsManager.getSlotDarkMode(slot);

    // Set volumes if configured (not 0xFF = no change)
    uint8_t mainVol = settingsManager.getSlotVolume(slot);
    uint8_t muteVol = settingsManager.getSlotMuteVolume(slot);
    
    Serial.printf("[PushNow] Slot %d volumes - main: %d, mute: %d\n", slot, mainVol, muteVol);

    // Update active slot and refresh display profile indicator
    settingsManager.setActiveSlot(slot);
    display.drawProfileIndicator(slot);

    pushNowState.slot = slot;
    memcpy(pushNowState.profileBytes, profile.settings.bytes, sizeof(pushNowState.profileBytes));
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
                  slot,
                  profileName.c_str(),
                  static_cast<int>(mode),
                  pushNowState.displayOn ? 1 : 0,
                  pushNowState.applyVolume ? "set" : "skip");

    server.send(200, "application/json", "{\"success\":true,\"queued\":true}");
}

void WiFiManager::handleAutoPushStatus() {
    // Return push executor status via callback
    if (getPushStatusJson) {
        String json = getPushStatusJson();
        server.send(200, "application/json", json);
    } else {
        server.send(500, "application/json", "{\"error\":\"Push status not available\"}");
    }
}

// ============= Display Colors Handlers =============

void WiFiManager::handleDisplayColorsSave() {
    if (!checkRateLimit()) return;
    
    Serial.println("[HTTP] POST /api/displaycolors");
    Serial.printf("[HTTP] Args count: %d\n", server.args());
    for (int i = 0; i < server.args(); i++) {
        Serial.printf("[HTTP] Arg %s = %s\n", server.argName(i).c_str(), server.arg(i).c_str());
    }

    const V1Settings& currentSettings = settingsManager.get();
    V1Settings& s = const_cast<V1Settings&>(currentSettings);

    auto argBool = [this](const char* key, bool fallback) -> bool {
        if (!server.hasArg(key)) return fallback;
        return server.arg(key) == "true" || server.arg(key) == "1";
    };

    // Main display colors
    if (server.hasArg("bogey") || server.hasArg("freq") || server.hasArg("arrowFront") ||
        server.hasArg("arrowSide") || server.hasArg("arrowRear") || server.hasArg("bandL") ||
        server.hasArg("bandKa") || server.hasArg("bandK") || server.hasArg("bandX")) {
        s.colorBogey = server.hasArg("bogey") ? server.arg("bogey").toInt() : s.colorBogey;
        s.colorFrequency = server.hasArg("freq") ? server.arg("freq").toInt() : s.colorFrequency;
        s.colorArrowFront = server.hasArg("arrowFront") ? server.arg("arrowFront").toInt() : s.colorArrowFront;
        s.colorArrowSide = server.hasArg("arrowSide") ? server.arg("arrowSide").toInt() : s.colorArrowSide;
        s.colorArrowRear = server.hasArg("arrowRear") ? server.arg("arrowRear").toInt() : s.colorArrowRear;
        s.colorBandL = server.hasArg("bandL") ? server.arg("bandL").toInt() : s.colorBandL;
        s.colorBandKa = server.hasArg("bandKa") ? server.arg("bandKa").toInt() : s.colorBandKa;
        s.colorBandK = server.hasArg("bandK") ? server.arg("bandK").toInt() : s.colorBandK;
        s.colorBandX = server.hasArg("bandX") ? server.arg("bandX").toInt() : s.colorBandX;

        Serial.printf("[HTTP] Saving colors: bogey=%d freq=%d arrowF=%d arrowS=%d arrowR=%d\n",
                      s.colorBogey, s.colorFrequency, s.colorArrowFront, s.colorArrowSide, s.colorArrowRear);
    }

    // Color groups
    if (server.hasArg("wifiIcon")) s.colorWiFiIcon = server.arg("wifiIcon").toInt();
    if (server.hasArg("wifiConnected")) s.colorWiFiConnected = server.arg("wifiConnected").toInt();
    if (server.hasArg("bleConnected")) s.colorBleConnected = server.arg("bleConnected").toInt();
    if (server.hasArg("bleDisconnected")) s.colorBleDisconnected = server.arg("bleDisconnected").toInt();
    if (server.hasArg("bar1")) s.colorBar1 = server.arg("bar1").toInt();
    if (server.hasArg("bar2")) s.colorBar2 = server.arg("bar2").toInt();
    if (server.hasArg("bar3")) s.colorBar3 = server.arg("bar3").toInt();
    if (server.hasArg("bar4")) s.colorBar4 = server.arg("bar4").toInt();
    if (server.hasArg("bar5")) s.colorBar5 = server.arg("bar5").toInt();
    if (server.hasArg("bar6")) s.colorBar6 = server.arg("bar6").toInt();
    if (server.hasArg("muted")) s.colorMuted = server.arg("muted").toInt();
    if (server.hasArg("bandPhoto")) s.colorBandPhoto = server.arg("bandPhoto").toInt();
    if (server.hasArg("persisted")) s.colorPersisted = server.arg("persisted").toInt();
    if (server.hasArg("volumeMain")) s.colorVolumeMain = server.arg("volumeMain").toInt();
    if (server.hasArg("volumeMute")) s.colorVolumeMute = server.arg("volumeMute").toInt();
    if (server.hasArg("rssiV1")) s.colorRssiV1 = server.arg("rssiV1").toInt();
    if (server.hasArg("rssiProxy")) s.colorRssiProxy = server.arg("rssiProxy").toInt();

    // Display toggles
    if (server.hasArg("freqUseBandColor")) s.freqUseBandColor = argBool("freqUseBandColor", s.freqUseBandColor);
    if (server.hasArg("hideWifiIcon")) s.hideWifiIcon = argBool("hideWifiIcon", s.hideWifiIcon);
    if (server.hasArg("hideProfileIndicator")) s.hideProfileIndicator = argBool("hideProfileIndicator", s.hideProfileIndicator);
    if (server.hasArg("hideBatteryIcon")) s.hideBatteryIcon = argBool("hideBatteryIcon", s.hideBatteryIcon);
    if (server.hasArg("showBatteryPercent")) s.showBatteryPercent = argBool("showBatteryPercent", s.showBatteryPercent);
    if (server.hasArg("hideBleIcon")) s.hideBleIcon = argBool("hideBleIcon", s.hideBleIcon);
    if (server.hasArg("hideVolumeIndicator")) s.hideVolumeIndicator = argBool("hideVolumeIndicator", s.hideVolumeIndicator);
    if (server.hasArg("hideRssiIndicator")) s.hideRssiIndicator = argBool("hideRssiIndicator", s.hideRssiIndicator);
    if (server.hasArg("showRestTelemetryCards")) s.showRestTelemetryCards = argBool("showRestTelemetryCards", s.showRestTelemetryCards);

    // Development/runtime toggles
    if (server.hasArg("enableWifiAtBoot")) s.enableWifiAtBoot = argBool("enableWifiAtBoot", s.enableWifiAtBoot);
    if (server.hasArg("gpsEnabled")) {
        s.gpsEnabled = argBool("gpsEnabled", s.gpsEnabled);
        gpsRuntimeModule.setEnabled(s.gpsEnabled);
        speedSourceSelector.setGpsEnabled(s.gpsEnabled);
    }
    if (server.hasArg("gpsLockoutMode")) {
        s.gpsLockoutMode = gpsLockoutParseRuntimeModeArg(server.arg("gpsLockoutMode"), s.gpsLockoutMode);
    }
    if (server.hasArg("gpsLockoutCoreGuardEnabled")) {
        s.gpsLockoutCoreGuardEnabled =
            argBool("gpsLockoutCoreGuardEnabled", s.gpsLockoutCoreGuardEnabled);
    }
    if (server.hasArg("gpsLockoutMaxQueueDrops")) {
        s.gpsLockoutMaxQueueDrops =
            clampU16Value(server.arg("gpsLockoutMaxQueueDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutMaxPerfDrops")) {
        s.gpsLockoutMaxPerfDrops =
            clampU16Value(server.arg("gpsLockoutMaxPerfDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutMaxEventBusDrops")) {
        s.gpsLockoutMaxEventBusDrops =
            clampU16Value(server.arg("gpsLockoutMaxEventBusDrops").toInt(), 0, 65535);
    }

    // Voice settings
    if (server.hasArg("voiceAlertMode")) {
        int mode = server.arg("voiceAlertMode").toInt();
        mode = std::max(0, std::min(mode, 3));
        s.voiceAlertMode = static_cast<VoiceAlertMode>(mode);
    }
    if (server.hasArg("voiceDirectionEnabled")) s.voiceDirectionEnabled = argBool("voiceDirectionEnabled", s.voiceDirectionEnabled);
    if (server.hasArg("announceBogeyCount")) s.announceBogeyCount = argBool("announceBogeyCount", s.announceBogeyCount);
    if (server.hasArg("muteVoiceIfVolZero")) s.muteVoiceIfVolZero = argBool("muteVoiceIfVolZero", s.muteVoiceIfVolZero);

    // Secondary alerts
    if (server.hasArg("announceSecondaryAlerts")) s.announceSecondaryAlerts = argBool("announceSecondaryAlerts", s.announceSecondaryAlerts);
    if (server.hasArg("secondaryLaser")) s.secondaryLaser = argBool("secondaryLaser", s.secondaryLaser);
    if (server.hasArg("secondaryKa")) s.secondaryKa = argBool("secondaryKa", s.secondaryKa);
    if (server.hasArg("secondaryK")) s.secondaryK = argBool("secondaryK", s.secondaryK);
    if (server.hasArg("secondaryX")) s.secondaryX = argBool("secondaryX", s.secondaryX);

    // Volume fade
    if (server.hasArg("alertVolumeFadeEnabled")) s.alertVolumeFadeEnabled = argBool("alertVolumeFadeEnabled", s.alertVolumeFadeEnabled);
    if (server.hasArg("alertVolumeFadeDelaySec")) {
        int val = server.arg("alertVolumeFadeDelaySec").toInt();
        s.alertVolumeFadeDelaySec = static_cast<uint8_t>(std::max(1, std::min(val, 10)));
    }
    if (server.hasArg("alertVolumeFadeVolume")) {
        int val = server.arg("alertVolumeFadeVolume").toInt();
        s.alertVolumeFadeVolume = static_cast<uint8_t>(std::max(0, std::min(val, 9)));
    }

    // Speed volume
    if (server.hasArg("speedVolumeEnabled")) s.speedVolumeEnabled = argBool("speedVolumeEnabled", s.speedVolumeEnabled);
    if (server.hasArg("speedVolumeThresholdMph")) {
        int val = server.arg("speedVolumeThresholdMph").toInt();
        s.speedVolumeThresholdMph = static_cast<uint8_t>(std::max(10, std::min(val, 100)));
    }
    if (server.hasArg("speedVolumeBoost")) {
        int val = server.arg("speedVolumeBoost").toInt();
        s.speedVolumeBoost = static_cast<uint8_t>(std::max(1, std::min(val, 5)));
    }

    // Low-speed mute
    if (server.hasArg("lowSpeedMuteEnabled")) s.lowSpeedMuteEnabled = argBool("lowSpeedMuteEnabled", s.lowSpeedMuteEnabled);
    if (server.hasArg("lowSpeedMuteThresholdMph")) {
        int val = server.arg("lowSpeedMuteThresholdMph").toInt();
        s.lowSpeedMuteThresholdMph = static_cast<uint8_t>(std::max(1, std::min(val, 30)));
    }

    // Misc sliders
    if (server.hasArg("brightness")) {
        int brightness = server.arg("brightness").toInt();
        brightness = std::max(0, std::min(brightness, 255));
        s.brightness = static_cast<uint8_t>(brightness);
        display.setBrightness(static_cast<uint8_t>(brightness));
    }
    if (server.hasArg("voiceVolume")) {
        int volume = server.arg("voiceVolume").toInt();
        volume = std::max(0, std::min(volume, 100));
        s.voiceVolume = static_cast<uint8_t>(volume);
        audio_set_volume(static_cast<uint8_t>(volume));
    }

    // Persist all color/visibility changes
    settingsManager.save();

    // Trigger immediate display preview to show new colors (skip if requested)
    if (!server.hasArg("skipPreview") || (server.arg("skipPreview") != "true" && server.arg("skipPreview") != "1")) {
        display.showDemo();
        requestColorPreviewHold(5500);  // Hold ~5.5s and cycle bands during preview
    }
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleDisplayColorsReset() {
    if (!checkRateLimit()) return;

    const V1Settings& currentSettings = settingsManager.get();
    V1Settings& s = const_cast<V1Settings&>(currentSettings);

    // Reset to default colors: Bogey/Freq=Red, Front/Side/Rear=Red, L/K=Blue, Ka=Red, X=Green, WiFi=Cyan
    s.colorBogey = 0xF800;
    s.colorFrequency = 0xF800;
    s.colorArrowFront = 0xF800;
    s.colorArrowSide = 0xF800;
    s.colorArrowRear = 0xF800;
    s.colorBandL = 0x001F;
    s.colorBandKa = 0xF800;
    s.colorBandK = 0x001F;
    s.colorBandX = 0x07E0;
    s.colorBandPhoto = 0x780F;
    s.colorWiFiIcon = 0x07FF;
    s.colorWiFiConnected = 0x07E0;
    s.colorBleConnected = 0x07E0;
    s.colorBleDisconnected = 0x001F;
    s.colorBar1 = 0x07E0;
    s.colorBar2 = 0x07E0;
    s.colorBar3 = 0xFFE0;
    s.colorBar4 = 0xFFE0;
    s.colorBar5 = 0xF800;
    s.colorBar6 = 0xF800;
    s.colorMuted = 0x3186;
    s.colorPersisted = 0x18C3;
    s.colorVolumeMain = 0x001F;
    s.colorVolumeMute = 0xFFE0;
    s.colorRssiV1 = 0x07E0;
    s.colorRssiProxy = 0x001F;
    s.freqUseBandColor = false;

    settingsManager.save();
    
    // Trigger immediate display preview to show reset colors
    display.showDemo();
    requestColorPreviewHold(5500);
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleDisplayColorsApi() {
    const V1Settings& s = settingsManager.get();
    
    JsonDocument doc;
    doc["bogey"] = s.colorBogey;
    doc["freq"] = s.colorFrequency;
    doc["arrowFront"] = s.colorArrowFront;
    doc["arrowSide"] = s.colorArrowSide;
    doc["arrowRear"] = s.colorArrowRear;
    doc["bandL"] = s.colorBandL;
    doc["bandKa"] = s.colorBandKa;
    doc["bandK"] = s.colorBandK;
    doc["bandX"] = s.colorBandX;
    doc["bandPhoto"] = s.colorBandPhoto;
    doc["wifiIcon"] = s.colorWiFiIcon;
    doc["wifiConnected"] = s.colorWiFiConnected;
    doc["bleConnected"] = s.colorBleConnected;
    doc["bleDisconnected"] = s.colorBleDisconnected;
    doc["bar1"] = s.colorBar1;
    doc["bar2"] = s.colorBar2;
    doc["bar3"] = s.colorBar3;
    doc["bar4"] = s.colorBar4;
    doc["bar5"] = s.colorBar5;
    doc["bar6"] = s.colorBar6;
    doc["muted"] = s.colorMuted;
    doc["persisted"] = s.colorPersisted;
    doc["volumeMain"] = s.colorVolumeMain;
    doc["volumeMute"] = s.colorVolumeMute;
    doc["rssiV1"] = s.colorRssiV1;
    doc["rssiProxy"] = s.colorRssiProxy;
    doc["freqUseBandColor"] = s.freqUseBandColor;
    doc["hideWifiIcon"] = s.hideWifiIcon;
    doc["hideProfileIndicator"] = s.hideProfileIndicator;
    doc["hideBatteryIcon"] = s.hideBatteryIcon;
    doc["showBatteryPercent"] = s.showBatteryPercent;
    doc["hideBleIcon"] = s.hideBleIcon;
    doc["hideVolumeIndicator"] = s.hideVolumeIndicator;
    doc["hideRssiIndicator"] = s.hideRssiIndicator;
    doc["showRestTelemetryCards"] = s.showRestTelemetryCards;
    doc["enableWifiAtBoot"] = s.enableWifiAtBoot;
    doc["voiceAlertMode"] = (int)s.voiceAlertMode;
    doc["voiceDirectionEnabled"] = s.voiceDirectionEnabled;
    doc["announceBogeyCount"] = s.announceBogeyCount;
    doc["muteVoiceIfVolZero"] = s.muteVoiceIfVolZero;
    doc["brightness"] = s.brightness;
    doc["voiceVolume"] = s.voiceVolume;
    doc["announceSecondaryAlerts"] = s.announceSecondaryAlerts;
    doc["secondaryLaser"] = s.secondaryLaser;
    doc["secondaryKa"] = s.secondaryKa;
    doc["secondaryK"] = s.secondaryK;
    doc["secondaryX"] = s.secondaryX;
    doc["alertVolumeFadeEnabled"] = s.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = s.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = s.alertVolumeFadeVolume;
    doc["speedVolumeEnabled"] = s.speedVolumeEnabled;
    doc["speedVolumeThresholdMph"] = s.speedVolumeThresholdMph;
    doc["speedVolumeBoost"] = s.speedVolumeBoost;
    doc["lowSpeedMuteEnabled"] = s.lowSpeedMuteEnabled;
    doc["lowSpeedMuteThresholdMph"] = s.lowSpeedMuteThresholdMph;
    doc["gpsEnabled"] = s.gpsEnabled;
    doc["gpsLockoutMode"] = static_cast<int>(s.gpsLockoutMode);
    doc["gpsLockoutModeName"] = lockoutRuntimeModeName(s.gpsLockoutMode);
    doc["gpsLockoutCoreGuardEnabled"] = s.gpsLockoutCoreGuardEnabled;
    doc["gpsLockoutMaxQueueDrops"] = s.gpsLockoutMaxQueueDrops;
    doc["gpsLockoutMaxPerfDrops"] = s.gpsLockoutMaxPerfDrops;
    doc["gpsLockoutMaxEventBusDrops"] = s.gpsLockoutMaxEventBusDrops;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// ============= Debug API Handlers =============

void WiFiManager::handleDebugMetrics() {
    // Get base perf metrics
    JsonDocument doc;
    
    // Core counters (always available)
    doc["rxPackets"] = perfCounters.rxPackets.load();
    doc["rxBytes"] = perfCounters.rxBytes.load();
    doc["parseSuccesses"] = perfCounters.parseSuccesses.load();
    doc["parseFailures"] = perfCounters.parseFailures.load();
    doc["queueDrops"] = perfCounters.queueDrops.load();
    doc["perfDrop"] = perfCounters.perfDrop.load();
    doc["perfSdLockFail"] = perfCounters.perfSdLockFail.load();
    doc["perfSdDirFail"] = perfCounters.perfSdDirFail.load();
    doc["perfSdOpenFail"] = perfCounters.perfSdOpenFail.load();
    doc["perfSdHeaderFail"] = perfCounters.perfSdHeaderFail.load();
    doc["perfSdMarkerFail"] = perfCounters.perfSdMarkerFail.load();
    doc["perfSdWriteFail"] = perfCounters.perfSdWriteFail.load();
    doc["oversizeDrops"] = perfCounters.oversizeDrops.load();
    doc["queueHighWater"] = perfCounters.queueHighWater.load();
    doc["cmdBleBusy"] = perfCounters.cmdBleBusy.load();
    doc["displayUpdates"] = perfCounters.displayUpdates.load();
    doc["displaySkips"] = perfCounters.displaySkips.load();
    doc["reconnects"] = perfCounters.reconnects.load();
    doc["disconnects"] = perfCounters.disconnects.load();
    doc["uuid128FallbackHits"] = perfCounters.uuid128FallbackHits.load();
    doc["bleDiscTaskCreateFail"] = perfCounters.bleDiscTaskCreateFail.load();
    doc["wifiConnectDeferred"] = perfCounters.wifiConnectDeferred.load();
    doc["pushNowRetries"] = perfCounters.pushNowRetries.load();
    doc["pushNowFailures"] = perfCounters.pushNowFailures.load();
    doc["alertPersistStarts"] = perfCounters.alertPersistStarts.load();
    doc["alertPersistExpires"] = perfCounters.alertPersistExpires.load();
    doc["alertPersistClears"] = perfCounters.alertPersistClears.load();
    doc["autoPushStarts"] = perfCounters.autoPushStarts.load();
    doc["autoPushCompletes"] = perfCounters.autoPushCompletes.load();
    doc["autoPushNoProfile"] = perfCounters.autoPushNoProfile.load();
    doc["autoPushProfileLoadFail"] = perfCounters.autoPushProfileLoadFail.load();
    doc["autoPushProfileWriteFail"] = perfCounters.autoPushProfileWriteFail.load();
    doc["autoPushBusyRetries"] = perfCounters.autoPushBusyRetries.load();
    doc["autoPushModeFail"] = perfCounters.autoPushModeFail.load();
    doc["autoPushVolumeFail"] = perfCounters.autoPushVolumeFail.load();
    doc["autoPushDisconnectAbort"] = perfCounters.autoPushDisconnectAbort.load();
    doc["speedVolBoosts"] = perfCounters.speedVolBoosts.load();
    doc["speedVolRestores"] = perfCounters.speedVolRestores.load();
    doc["speedVolFadeTakeovers"] = perfCounters.speedVolFadeTakeovers.load();
    doc["speedVolNoHeadroom"] = perfCounters.speedVolNoHeadroom.load();
    doc["voiceAnnouncePriority"] = perfCounters.voiceAnnouncePriority.load();
    doc["voiceAnnounceDirection"] = perfCounters.voiceAnnounceDirection.load();
    doc["voiceAnnounceSecondary"] = perfCounters.voiceAnnounceSecondary.load();
    doc["voiceAnnounceEscalation"] = perfCounters.voiceAnnounceEscalation.load();
    doc["voiceDirectionThrottled"] = perfCounters.voiceDirectionThrottled.load();
    doc["powerAutoPowerArmed"] = perfCounters.powerAutoPowerArmed.load();
    doc["powerAutoPowerTimerStart"] = perfCounters.powerAutoPowerTimerStart.load();
    doc["powerAutoPowerTimerCancel"] = perfCounters.powerAutoPowerTimerCancel.load();
    doc["powerAutoPowerTimerExpire"] = perfCounters.powerAutoPowerTimerExpire.load();
    doc["powerCriticalWarn"] = perfCounters.powerCriticalWarn.load();
    doc["powerCriticalShutdown"] = perfCounters.powerCriticalShutdown.load();
    doc["loopMaxUs"] = perfGetLoopMaxUs();
    doc["wifiMaxUs"] = perfGetWifiMaxUs();
    doc["fsMaxUs"] = perfGetFsMaxUs();
    doc["sdMaxUs"] = perfGetSdMaxUs();
    doc["flushMaxUs"] = perfGetFlushMaxUs();
    doc["bleDrainMaxUs"] = perfGetBleDrainMaxUs();

    // OBD health snapshot (non-blocking lock attempt in OBD handler).
    const OBDPerfSnapshot obdPerf = obdHandler.getPerfSnapshot();
    JsonObject obdObj = doc["obd"].to<JsonObject>();
    obdObj["state"] = obdPerf.state;
    obdObj["connected"] = (obdPerf.connected != 0);
    obdObj["scanActive"] = (obdPerf.scanActive != 0);
    obdObj["hasValidData"] = (obdPerf.hasValidData != 0);
    if (obdPerf.sampleAgeMs == UINT32_MAX) {
        obdObj["sampleAgeMs"] = nullptr;
    } else {
        obdObj["sampleAgeMs"] = obdPerf.sampleAgeMs;
    }
    if (obdPerf.speedMphX10 < 0) {
        obdObj["speedMphX10"] = nullptr;
    } else {
        obdObj["speedMphX10"] = obdPerf.speedMphX10;
    }
    obdObj["connFailures"] = obdPerf.connectionFailures;
    obdObj["pollFailStreak"] = obdPerf.consecutivePollFailures;
    obdObj["notifyDrops"] = obdPerf.notifyDrops;

    const uint32_t nowMs = millis();
    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    JsonObject gpsObj = doc["gps"].to<JsonObject>();
    gpsObj["enabled"] = gpsStatus.enabled;
    gpsObj["mode"] = (gpsStatus.parserActive || gpsStatus.moduleDetected || gpsStatus.hardwareSamples > 0)
                         ? "runtime"
                         : "scaffold";
    gpsObj["sampleValid"] = gpsStatus.sampleValid;
    gpsObj["hasFix"] = gpsStatus.hasFix;
    gpsObj["satellites"] = gpsStatus.satellites;
    gpsObj["injectedSamples"] = gpsStatus.injectedSamples;
    gpsObj["moduleDetected"] = gpsStatus.moduleDetected;
    gpsObj["detectionTimedOut"] = gpsStatus.detectionTimedOut;
    gpsObj["parserActive"] = gpsStatus.parserActive;
    gpsObj["hardwareSamples"] = gpsStatus.hardwareSamples;
    gpsObj["bytesRead"] = gpsStatus.bytesRead;
    gpsObj["sentencesSeen"] = gpsStatus.sentencesSeen;
    gpsObj["sentencesParsed"] = gpsStatus.sentencesParsed;
    gpsObj["parseFailures"] = gpsStatus.parseFailures;
    gpsObj["checksumFailures"] = gpsStatus.checksumFailures;
    gpsObj["bufferOverruns"] = gpsStatus.bufferOverruns;
    if (std::isnan(gpsStatus.hdop)) {
        gpsObj["hdop"] = nullptr;
    } else {
        gpsObj["hdop"] = gpsStatus.hdop;
    }
    gpsObj["locationValid"] = gpsStatus.locationValid;
    if (gpsStatus.locationValid) {
        gpsObj["latitude"] = gpsStatus.latitudeDeg;
        gpsObj["longitude"] = gpsStatus.longitudeDeg;
    } else {
        gpsObj["latitude"] = nullptr;
        gpsObj["longitude"] = nullptr;
    }
    if (gpsStatus.sampleValid) {
        gpsObj["speedMph"] = gpsStatus.speedMph;
        gpsObj["sampleTsMs"] = gpsStatus.sampleTsMs;
    } else {
        gpsObj["speedMph"] = nullptr;
        gpsObj["sampleTsMs"] = nullptr;
    }
    if (gpsStatus.sampleAgeMs == UINT32_MAX) {
        gpsObj["sampleAgeMs"] = nullptr;
    } else {
        gpsObj["sampleAgeMs"] = gpsStatus.sampleAgeMs;
    }
    if (gpsStatus.lastSentenceTsMs == 0) {
        gpsObj["lastSentenceTsMs"] = nullptr;
    } else {
        gpsObj["lastSentenceTsMs"] = gpsStatus.lastSentenceTsMs;
    }
    const GpsObservationLogStats gpsLogStats = gpsObservationLog.stats();
    JsonObject gpsLogObj = doc["gpsLog"].to<JsonObject>();
    gpsLogObj["published"] = gpsLogStats.published;
    gpsLogObj["drops"] = gpsLogStats.drops;
    gpsLogObj["size"] = static_cast<uint32_t>(gpsLogStats.size);
    gpsLogObj["capacity"] = static_cast<uint32_t>(GpsObservationLog::kCapacity);

    const SpeedSelectorStatus speedStatus = speedSourceSelector.snapshot(nowMs);
    JsonObject speedObj = doc["speedSource"].to<JsonObject>();
    speedObj["gpsEnabled"] = speedStatus.gpsEnabled;
    speedObj["obdConnected"] = speedStatus.obdConnected;
    speedObj["selected"] = SpeedSourceSelector::sourceName(speedStatus.selectedSource);
    if (speedStatus.selectedSource == SpeedSource::NONE) {
        speedObj["selectedMph"] = nullptr;
        speedObj["selectedAgeMs"] = nullptr;
    } else {
        speedObj["selectedMph"] = speedStatus.selectedSpeedMph;
        speedObj["selectedAgeMs"] = speedStatus.selectedAgeMs;
    }
    speedObj["obdFresh"] = speedStatus.obdFresh;
    speedObj["obdMph"] = speedStatus.obdSpeedMph;
    if (speedStatus.obdAgeMs == UINT32_MAX) {
        speedObj["obdAgeMs"] = nullptr;
    } else {
        speedObj["obdAgeMs"] = speedStatus.obdAgeMs;
    }
    speedObj["gpsFresh"] = speedStatus.gpsFresh;
    speedObj["gpsMph"] = speedStatus.gpsSpeedMph;
    if (speedStatus.gpsAgeMs == UINT32_MAX) {
        speedObj["gpsAgeMs"] = nullptr;
    } else {
        speedObj["gpsAgeMs"] = speedStatus.gpsAgeMs;
    }
    speedObj["sourceSwitches"] = speedStatus.sourceSwitches;
    speedObj["obdSelections"] = speedStatus.obdSelections;
    speedObj["gpsSelections"] = speedStatus.gpsSelections;
    speedObj["noSourceSelections"] = speedStatus.noSourceSelections;
    
    // Heap stats - both total and DMA-capable (for WiFi/SD contention diagnosis)
    doc["heapFree"] = ESP.getFreeHeap();
    doc["heapMinFree"] = perfGetMinFreeHeap();
    doc["heapLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    // DMA stats use cached values from StorageManager (no extra API calls)
    doc["heapDma"] = StorageManager::getCachedFreeDma();
    doc["heapDmaMin"] = perfGetMinFreeDma();
    doc["heapDmaLargest"] = StorageManager::getCachedLargestDma();
    doc["heapDmaLargestMin"] = perfGetMinLargestDma();
    
    // SD access contention stats
    doc["sdTryLockFails"] = StorageManager::sdTryLockFailCount.load();
    doc["sdDmaStarvation"] = StorageManager::sdDmaStarvationCount.load();
    
#if PERF_METRICS
    doc["monitoringEnabled"] = (bool)PERF_MONITORING;
#if PERF_MONITORING
    extern PerfLatency perfLatency;
    extern bool perfDebugEnabled;
    uint32_t minUsVal = perfLatency.minUs.load();
    uint32_t minUs = (minUsVal == UINT32_MAX) ? 0 : minUsVal;
    doc["latencyMinUs"] = minUs;
    doc["latencyAvgUs"] = perfLatency.avgUs();
    doc["latencyMaxUs"] = perfLatency.maxUs.load();
    doc["latencySamples"] = perfLatency.sampleCount.load();
    doc["debugEnabled"] = perfDebugEnabled;
#else
    doc["latencyMinUs"] = 0;
    doc["latencyAvgUs"] = 0;
    doc["latencyMaxUs"] = 0;
    doc["latencySamples"] = 0;
    doc["debugEnabled"] = false;
#endif
#else
    doc["metricsEnabled"] = false;
#endif
    
    // Add proxy metrics from BLE client
    const ProxyMetrics& proxy = bleClient.getProxyMetrics();
    JsonObject proxyObj = doc["proxy"].to<JsonObject>();
    proxyObj["sendCount"] = proxy.sendCount;
    proxyObj["dropCount"] = proxy.dropCount;
    proxyObj["errorCount"] = proxy.errorCount;
    proxyObj["queueHighWater"] = proxy.queueHighWater;
    proxyObj["connected"] = bleClient.isProxyClientConnected();

    // Event-bus health metrics (used to verify no backlog/drop under load).
    JsonObject eventBusObj = doc["eventBus"].to<JsonObject>();
    eventBusObj["publishCount"] = systemEventBus.getPublishCount();
    eventBusObj["dropCount"] = systemEventBus.getDropCount();
    eventBusObj["size"] = static_cast<uint32_t>(systemEventBus.size());

    const V1Settings& settings = settingsManager.get();
    const GpsLockoutCoreGuardStatus lockoutGuard = gpsLockoutEvaluateCoreGuard(
        settings.gpsLockoutCoreGuardEnabled,
        settings.gpsLockoutMaxQueueDrops,
        settings.gpsLockoutMaxPerfDrops,
        settings.gpsLockoutMaxEventBusDrops,
        perfCounters.queueDrops.load(),
        perfCounters.perfDrop.load(),
        systemEventBus.getDropCount());
    JsonObject lockoutObj = doc["lockout"].to<JsonObject>();
    lockoutObj["mode"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    lockoutObj["modeRaw"] = static_cast<int>(settings.gpsLockoutMode);
    lockoutObj["coreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    lockoutObj["coreGuardTripped"] = lockoutGuard.tripped;
    lockoutObj["coreGuardReason"] = lockoutGuard.reason;
    lockoutObj["maxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    lockoutObj["maxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    lockoutObj["maxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    lockoutObj["enforceRequested"] = (settings.gpsLockoutMode == LOCKOUT_RUNTIME_ENFORCE);
    lockoutObj["enforceAllowed"] = (settings.gpsLockoutMode == LOCKOUT_RUNTIME_ENFORCE) &&
                                   !lockoutGuard.tripped;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleDebugEnable() {
    if (!checkRateLimit()) return;
    bool enable = true;
    if (server.hasArg("enable")) {
        enable = (server.arg("enable") == "true" || server.arg("enable") == "1");
    }
    perfMetricsSetDebug(enable);
    server.send(200, "application/json", "{\"success\":true,\"debugEnabled\":" + String(enable ? "true" : "false") + "}");
}

void WiFiManager::handleDebugPanic() {
    // Return last panic info from LittleFS (written by logPanicBreadcrumbs on crash recovery)
    JsonDocument doc;
    
    // Get last reset reason
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "UNKNOWN";
    switch (reason) {
        case ESP_RST_POWERON: reasonStr = "POWERON"; break;
        case ESP_RST_SW: reasonStr = "SW"; break;
        case ESP_RST_PANIC: reasonStr = "PANIC"; break;
        case ESP_RST_INT_WDT: reasonStr = "WDT_INT"; break;
        case ESP_RST_TASK_WDT: reasonStr = "WDT_TASK"; break;
        case ESP_RST_WDT: reasonStr = "WDT"; break;
        case ESP_RST_DEEPSLEEP: reasonStr = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT: reasonStr = "BROWNOUT"; break;
        default: break;
    }
    doc["lastResetReason"] = reasonStr;
    doc["wasCrash"] = (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || 
                       reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT);
    
    // Try to read panic.txt from LittleFS
    String panicContent = "";
    if (LittleFS.exists("/panic.txt")) {
        File f = LittleFS.open("/panic.txt", "r");
        if (f) {
            panicContent = f.readString();
            f.close();
        }
        doc["hasPanicFile"] = true;
    } else {
        doc["hasPanicFile"] = false;
    }
    doc["panicInfo"] = panicContent;
    
    // Current heap stats for comparison
    doc["heapFree"] = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    doc["heapLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    doc["heapMinEver"] = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    doc["heapDma"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    doc["heapDmaMin"] = perfGetMinFreeDma();
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleDebugPerfFilesList() {
    if (!checkRateLimit()) return;
    markUiActivity();

    JsonDocument doc;
    doc["success"] = true;
    doc["storageReady"] = storageManager.isReady();
    doc["onSdCard"] = storageManager.isSDCard();
    doc["path"] = "/perf";

    JsonArray filesArr = doc["files"].to<JsonArray>();

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json);
        return;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        doc["success"] = false;
        doc["error"] = lock.isDmaStarved() ? "Low DMA heap; perf file listing deferred" : "SD busy";
        String json;
        serializeJson(doc, json);
        server.send(503, "application/json", json);
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists("/perf")) {
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json);
        return;
    }

    File dir = fs->open("/perf");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        doc["success"] = false;
        doc["error"] = "Failed to open /perf directory";
        String json;
        serializeJson(doc, json);
        server.send(500, "application/json", json);
        return;
    }

    struct PerfFileInfo {
        String name;
        uint32_t sizeBytes;
        uint32_t bootId;
    };
    std::vector<PerfFileInfo> rows;
    rows.reserve(16);

    File entry;
    while ((entry = dir.openNextFile())) {
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }

        String name = fileNameFromPath(entry.name());
        if (!isValidPerfFileName(name)) {
            entry.close();
            continue;
        }

        uint32_t bootId = 0;
        if (name.startsWith("perf_boot_") && name.endsWith(".csv")) {
            String digits = name.substring(strlen("perf_boot_"), name.length() - 4);
            bootId = static_cast<uint32_t>(strtoul(digits.c_str(), nullptr, 10));
        }

        rows.push_back({name, static_cast<uint32_t>(entry.size()), bootId});
        entry.close();
    }
    dir.close();

    std::sort(rows.begin(), rows.end(), [](const PerfFileInfo& a, const PerfFileInfo& b) {
        if (a.bootId != b.bootId) {
            return a.bootId > b.bootId;
        }
        return a.name > b.name;
    });

    for (const PerfFileInfo& row : rows) {
        JsonObject f = filesArr.add<JsonObject>();
        f["name"] = row.name;
        f["sizeBytes"] = row.sizeBytes;
        f["bootId"] = row.bootId;
        f["active"] = (String("/perf/") + row.name) == String(perfSdLogger.csvPath());
    }
    doc["count"] = static_cast<uint32_t>(rows.size());

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleDebugPerfFileDownload() {
    if (!checkRateLimit()) return;
    markUiActivity();

    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing file name\"}");
        return;
    }

    String requestedName = server.arg("name");
    String path;
    if (!perfFilePathFromName(requestedName, path)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid file name\"}");
        return;
    }

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"SD storage unavailable\"}");
        return;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        if (lock.isDmaStarved()) {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"Low DMA heap; try again\"}");
        } else {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"SD busy\"}");
        }
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists(path)) {
        server.send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
        return;
    }

    File f = fs->open(path, FILE_READ);
    if (!f) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Failed to open file\"}");
        return;
    }

    size_t fileSize = f.size();
    server.sendHeader("Content-Type", "text/csv");
    String contentDisposition = String("attachment; filename=\"") + requestedName + "\"";
    server.sendHeader("Content-Disposition", contentDisposition);
    server.sendHeader("Cache-Control", "no-cache");
    server.setContentLength(fileSize);
    server.send(200, "text/csv", "");

    const size_t CHUNK_SIZE = 4096;
    uint8_t* buffer = static_cast<uint8_t*>(malloc(CHUNK_SIZE));
    if (!buffer) {
        f.close();
        return;
    }

    size_t totalSent = 0;
    while (f.available() && server.client().connected()) {
        size_t toRead = min(CHUNK_SIZE, static_cast<size_t>(f.available()));
        size_t bytesRead = f.read(buffer, toRead);
        if (bytesRead > 0) {
            server.client().write(buffer, bytesRead);
            totalSent += bytesRead;
        }
        yield();
        if ((totalSent % 32768) == 0) {
            delay(1);
        }
    }

    free(buffer);
    f.close();
}

void WiFiManager::handleDebugPerfFileDelete() {
    if (!checkRateLimit()) return;
    markUiActivity();

    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing file name\"}");
        return;
    }

    String requestedName = server.arg("name");
    String path;
    if (!perfFilePathFromName(requestedName, path)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid file name\"}");
        return;
    }

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"SD storage unavailable\"}");
        return;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        if (lock.isDmaStarved()) {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"Low DMA heap; try again\"}");
        } else {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"SD busy\"}");
        }
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists(path)) {
        server.send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
        return;
    }

    bool ok = fs->remove(path);

    JsonDocument doc;
    doc["success"] = ok;
    doc["name"] = requestedName;
    if (!ok) {
        doc["error"] = "Delete failed";
    }

    String json;
    serializeJson(doc, json);
    server.send(ok ? 200 : 500, "application/json", json);
}

// ============= Settings Backup/Restore API Handlers =============

void WiFiManager::handleSettingsBackup() {
    markUiActivity();
    Serial.println("[HTTP] GET /api/settings/backup");
    
    const V1Settings& s = settingsManager.get();
    JsonDocument doc;
    
    // Metadata
    doc["_version"] = 4;  // Backup format version
    doc["_type"] = "v1simple_backup";
    doc["_timestamp"] = millis();
    
    // WiFi settings (exclude password for security)
    doc["apSSID"] = s.apSSID;
    // Note: password not included in backup for security
    
    // BLE settings
    doc["proxyBLE"] = s.proxyBLE;
    doc["proxyName"] = s.proxyName;
    doc["obdVwDataEnabled"] = s.obdVwDataEnabled;
    doc["gpsEnabled"] = s.gpsEnabled;
    doc["gpsLockoutMode"] = static_cast<int>(s.gpsLockoutMode);
    doc["gpsLockoutCoreGuardEnabled"] = s.gpsLockoutCoreGuardEnabled;
    doc["gpsLockoutMaxQueueDrops"] = s.gpsLockoutMaxQueueDrops;
    doc["gpsLockoutMaxPerfDrops"] = s.gpsLockoutMaxPerfDrops;
    doc["gpsLockoutMaxEventBusDrops"] = s.gpsLockoutMaxEventBusDrops;
    
    // Display settings
    doc["brightness"] = s.brightness;
    doc["displayStyle"] = (int)s.displayStyle;
    doc["turnOffDisplay"] = s.turnOffDisplay;
    
    // All colors (RGB565)
    doc["colorBogey"] = s.colorBogey;
    doc["colorFrequency"] = s.colorFrequency;
    doc["colorArrowFront"] = s.colorArrowFront;
    doc["colorArrowSide"] = s.colorArrowSide;
    doc["colorArrowRear"] = s.colorArrowRear;
    doc["colorBandL"] = s.colorBandL;
    doc["colorBandKa"] = s.colorBandKa;
    doc["colorBandK"] = s.colorBandK;
    doc["colorBandX"] = s.colorBandX;
    doc["colorBandPhoto"] = s.colorBandPhoto;
    doc["colorWiFiIcon"] = s.colorWiFiIcon;
    doc["colorBleConnected"] = s.colorBleConnected;
    doc["colorBleDisconnected"] = s.colorBleDisconnected;
    doc["colorBar1"] = s.colorBar1;
    doc["colorBar2"] = s.colorBar2;
    doc["colorBar3"] = s.colorBar3;
    doc["colorBar4"] = s.colorBar4;
    doc["colorBar5"] = s.colorBar5;
    doc["colorBar6"] = s.colorBar6;
    doc["colorMuted"] = s.colorMuted;
    doc["colorPersisted"] = s.colorPersisted;
    doc["colorVolumeMain"] = s.colorVolumeMain;
    doc["colorVolumeMute"] = s.colorVolumeMute;
    doc["colorWiFiConnected"] = s.colorWiFiConnected;
    doc["colorRssiV1"] = s.colorRssiV1;
    doc["colorRssiProxy"] = s.colorRssiProxy;
    doc["freqUseBandColor"] = s.freqUseBandColor;
    
    // Display visibility
    doc["hideWifiIcon"] = s.hideWifiIcon;
    doc["hideProfileIndicator"] = s.hideProfileIndicator;
    doc["hideBatteryIcon"] = s.hideBatteryIcon;
    doc["showBatteryPercent"] = s.showBatteryPercent;
    doc["hideBleIcon"] = s.hideBleIcon;
    doc["hideVolumeIndicator"] = s.hideVolumeIndicator;
    doc["hideRssiIndicator"] = s.hideRssiIndicator;
    doc["showRestTelemetryCards"] = s.showRestTelemetryCards;
    
    // Development
    doc["enableWifiAtBoot"] = s.enableWifiAtBoot;
    
    // WiFi client settings
    doc["wifiMode"] = (int)s.wifiMode;
    doc["wifiClientEnabled"] = s.wifiClientEnabled;
    doc["wifiClientSSID"] = s.wifiClientSSID;
    
    // Auto power-off
    doc["autoPowerOffMinutes"] = s.autoPowerOffMinutes;
    doc["apTimeoutMinutes"] = s.apTimeoutMinutes;
    
    // Voice settings
    doc["voiceAlertMode"] = (int)s.voiceAlertMode;
    doc["voiceDirectionEnabled"] = s.voiceDirectionEnabled;
    doc["announceBogeyCount"] = s.announceBogeyCount;
    doc["muteVoiceIfVolZero"] = s.muteVoiceIfVolZero;
    doc["voiceVolume"] = s.voiceVolume;
    doc["announceSecondaryAlerts"] = s.announceSecondaryAlerts;
    doc["secondaryLaser"] = s.secondaryLaser;
    doc["secondaryKa"] = s.secondaryKa;
    doc["secondaryK"] = s.secondaryK;
    doc["secondaryX"] = s.secondaryX;
    doc["alertVolumeFadeEnabled"] = s.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = s.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = s.alertVolumeFadeVolume;
    doc["speedVolumeEnabled"] = s.speedVolumeEnabled;
    doc["speedVolumeThresholdMph"] = s.speedVolumeThresholdMph;
    doc["speedVolumeBoost"] = s.speedVolumeBoost;
    doc["lowSpeedMuteEnabled"] = s.lowSpeedMuteEnabled;
    doc["lowSpeedMuteThresholdMph"] = s.lowSpeedMuteThresholdMph;
    
    // Auto-push slot settings
    doc["autoPushEnabled"] = s.autoPushEnabled;
    doc["activeSlot"] = s.activeSlot;
    doc["slot0Name"] = s.slot0Name;
    doc["slot1Name"] = s.slot1Name;
    doc["slot2Name"] = s.slot2Name;
    doc["slot0Color"] = s.slot0Color;
    doc["slot1Color"] = s.slot1Color;
    doc["slot2Color"] = s.slot2Color;
    doc["slot0Volume"] = s.slot0Volume;
    doc["slot1Volume"] = s.slot1Volume;
    doc["slot2Volume"] = s.slot2Volume;
    doc["slot0MuteVolume"] = s.slot0MuteVolume;
    doc["slot1MuteVolume"] = s.slot1MuteVolume;
    doc["slot2MuteVolume"] = s.slot2MuteVolume;
    doc["slot0DarkMode"] = s.slot0DarkMode;
    doc["slot1DarkMode"] = s.slot1DarkMode;
    doc["slot2DarkMode"] = s.slot2DarkMode;
    doc["slot0MuteToZero"] = s.slot0MuteToZero;
    doc["slot1MuteToZero"] = s.slot1MuteToZero;
    doc["slot2MuteToZero"] = s.slot2MuteToZero;
    doc["slot0AlertPersist"] = s.slot0AlertPersist;
    doc["slot1AlertPersist"] = s.slot1AlertPersist;
    doc["slot2AlertPersist"] = s.slot2AlertPersist;
    doc["slot0PriorityArrow"] = s.slot0PriorityArrow;
    doc["slot1PriorityArrow"] = s.slot1PriorityArrow;
    doc["slot2PriorityArrow"] = s.slot2PriorityArrow;
    doc["slot0ProfileName"] = s.slot0_default.profileName;
    doc["slot0Mode"] = s.slot0_default.mode;
    doc["slot1ProfileName"] = s.slot1_highway.profileName;
    doc["slot1Mode"] = s.slot1_highway.mode;
    doc["slot2ProfileName"] = s.slot2_comfort.profileName;
    doc["slot2Mode"] = s.slot2_comfort.mode;
    
    // V1 Profiles backup
    JsonArray profilesArr = doc["profiles"].to<JsonArray>();
    std::vector<String> profileNames = v1ProfileManager.listProfiles();
    for (const String& name : profileNames) {
        V1Profile profile;
        if (v1ProfileManager.loadProfile(name, profile)) {
            JsonObject p = profilesArr.add<JsonObject>();
            p["name"] = profile.name;
            p["description"] = profile.description;
            p["displayOn"] = profile.displayOn;
            p["mainVolume"] = profile.mainVolume;
            p["mutedVolume"] = profile.mutedVolume;
            // Store raw bytes array
            JsonArray bytes = p["bytes"].to<JsonArray>();
            for (int i = 0; i < 6; i++) {
                bytes.add(profile.settings.bytes[i]);
            }
        }
    }
    
    String json;
    serializeJsonPretty(doc, json);
    
    // Send with Content-Disposition header for download
    server.sendHeader("Content-Disposition", "attachment; filename=\"v1simple_backup.json\"");
    server.send(200, "application/json", json);
}

void WiFiManager::handleSettingsRestore() {
    if (!checkRateLimit()) return;
    markUiActivity();
    Serial.println("[HTTP] POST /api/settings/restore");
    
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"No JSON body provided\"}");
        return;
    }
    
    String body = server.arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    
    if (err) {
        Serial.printf("[Settings] Restore parse error: %s\n", err.c_str());
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    
    // Verify backup format
    if (!doc["_type"].is<const char*>() || String(doc["_type"].as<const char*>()) != "v1simple_backup") {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid backup format\"}");
        return;
    }
    
    // Restore settings (same logic as restoreFromSD but from JSON body)
    V1Settings& s = const_cast<V1Settings&>(settingsManager.get());
    
    // BLE settings
    if (doc["proxyBLE"].is<bool>()) s.proxyBLE = doc["proxyBLE"];
    if (doc["proxyName"].is<const char*>()) s.proxyName = sanitizeProxyNameValue(doc["proxyName"].as<String>());
    if (doc["obdVwDataEnabled"].is<bool>()) s.obdVwDataEnabled = doc["obdVwDataEnabled"];
    if (doc["gpsEnabled"].is<bool>()) s.gpsEnabled = doc["gpsEnabled"];
    if (doc["gpsLockoutMode"].is<int>()) {
        s.gpsLockoutMode = clampLockoutRuntimeModeValue(doc["gpsLockoutMode"].as<int>());
    } else if (doc["gpsLockoutMode"].is<const char*>()) {
        s.gpsLockoutMode = gpsLockoutParseRuntimeModeArg(doc["gpsLockoutMode"].as<String>(),
                                                         s.gpsLockoutMode);
    }
    if (doc["gpsLockoutCoreGuardEnabled"].is<bool>()) {
        s.gpsLockoutCoreGuardEnabled = doc["gpsLockoutCoreGuardEnabled"];
    }
    if (doc["gpsLockoutMaxQueueDrops"].is<int>()) {
        s.gpsLockoutMaxQueueDrops = clampU16Value(doc["gpsLockoutMaxQueueDrops"].as<int>(), 0, 65535);
    }
    if (doc["gpsLockoutMaxPerfDrops"].is<int>()) {
        s.gpsLockoutMaxPerfDrops = clampU16Value(doc["gpsLockoutMaxPerfDrops"].as<int>(), 0, 65535);
    }
    if (doc["gpsLockoutMaxEventBusDrops"].is<int>()) {
        s.gpsLockoutMaxEventBusDrops = clampU16Value(doc["gpsLockoutMaxEventBusDrops"].as<int>(), 0, 65535);
    }
    
    // WiFi settings (password intentionally excluded from backups)
    if (doc["apSSID"].is<const char*>()) {
        // Preserve existing password while restoring SSID
        settingsManager.updateAPCredentials(sanitizeApSsidValue(doc["apSSID"].as<String>()), settingsManager.get().apPassword);
    }
    
    // Display settings
    if (doc["brightness"].is<int>()) s.brightness = clampU8Value(doc["brightness"].as<int>(), 1, 255);
    if (doc["displayStyle"].is<int>()) s.displayStyle = normalizeDisplayStyle(doc["displayStyle"].as<int>());
    if (doc["turnOffDisplay"].is<bool>()) s.turnOffDisplay = doc["turnOffDisplay"];
    
    // All colors
    if (doc["colorBogey"].is<int>()) s.colorBogey = doc["colorBogey"];
    if (doc["colorFrequency"].is<int>()) s.colorFrequency = doc["colorFrequency"];
    if (doc["colorArrowFront"].is<int>()) s.colorArrowFront = doc["colorArrowFront"];
    if (doc["colorArrowSide"].is<int>()) s.colorArrowSide = doc["colorArrowSide"];
    if (doc["colorArrowRear"].is<int>()) s.colorArrowRear = doc["colorArrowRear"];
    if (doc["colorBandL"].is<int>()) s.colorBandL = doc["colorBandL"];
    if (doc["colorBandKa"].is<int>()) s.colorBandKa = doc["colorBandKa"];
    if (doc["colorBandK"].is<int>()) s.colorBandK = doc["colorBandK"];
    if (doc["colorBandX"].is<int>()) s.colorBandX = doc["colorBandX"];
    if (doc["colorBandPhoto"].is<int>()) s.colorBandPhoto = doc["colorBandPhoto"];
    if (doc["colorWiFiIcon"].is<int>()) s.colorWiFiIcon = doc["colorWiFiIcon"];
    if (doc["colorBleConnected"].is<int>()) s.colorBleConnected = doc["colorBleConnected"];
    if (doc["colorBleDisconnected"].is<int>()) s.colorBleDisconnected = doc["colorBleDisconnected"];
    if (doc["colorBar1"].is<int>()) s.colorBar1 = doc["colorBar1"];
    if (doc["colorBar2"].is<int>()) s.colorBar2 = doc["colorBar2"];
    if (doc["colorBar3"].is<int>()) s.colorBar3 = doc["colorBar3"];
    if (doc["colorBar4"].is<int>()) s.colorBar4 = doc["colorBar4"];
    if (doc["colorBar5"].is<int>()) s.colorBar5 = doc["colorBar5"];
    if (doc["colorBar6"].is<int>()) s.colorBar6 = doc["colorBar6"];
    if (doc["colorMuted"].is<int>()) s.colorMuted = doc["colorMuted"];
    if (doc["colorPersisted"].is<int>()) s.colorPersisted = doc["colorPersisted"];
    if (doc["colorVolumeMain"].is<int>()) s.colorVolumeMain = doc["colorVolumeMain"];
    if (doc["colorVolumeMute"].is<int>()) s.colorVolumeMute = doc["colorVolumeMute"];
    if (doc["colorWiFiConnected"].is<int>()) s.colorWiFiConnected = doc["colorWiFiConnected"];
    if (doc["colorRssiV1"].is<int>()) s.colorRssiV1 = doc["colorRssiV1"];
    if (doc["colorRssiProxy"].is<int>()) s.colorRssiProxy = doc["colorRssiProxy"];
    if (doc["freqUseBandColor"].is<bool>()) s.freqUseBandColor = doc["freqUseBandColor"];
    
    // Display visibility
    if (doc["hideWifiIcon"].is<bool>()) s.hideWifiIcon = doc["hideWifiIcon"];
    if (doc["hideProfileIndicator"].is<bool>()) s.hideProfileIndicator = doc["hideProfileIndicator"];
    if (doc["hideBatteryIcon"].is<bool>()) s.hideBatteryIcon = doc["hideBatteryIcon"];
    if (doc["showBatteryPercent"].is<bool>()) s.showBatteryPercent = doc["showBatteryPercent"];
    if (doc["hideBleIcon"].is<bool>()) s.hideBleIcon = doc["hideBleIcon"];
    if (doc["hideVolumeIndicator"].is<bool>()) s.hideVolumeIndicator = doc["hideVolumeIndicator"];
    if (doc["hideRssiIndicator"].is<bool>()) s.hideRssiIndicator = doc["hideRssiIndicator"];
    if (doc["showRestTelemetryCards"].is<bool>()) s.showRestTelemetryCards = doc["showRestTelemetryCards"];
    
    // Development
    if (doc["enableWifiAtBoot"].is<bool>()) s.enableWifiAtBoot = doc["enableWifiAtBoot"];
    
    // WiFi client settings
    if (doc["wifiMode"].is<int>()) {
        int mode = doc["wifiMode"].as<int>();
        mode = std::max(static_cast<int>(V1_WIFI_OFF), std::min(mode, static_cast<int>(V1_WIFI_APSTA)));
        s.wifiMode = static_cast<WiFiModeSetting>(mode);
    }
    if (doc["wifiClientEnabled"].is<bool>()) s.wifiClientEnabled = doc["wifiClientEnabled"];
    if (doc["wifiClientSSID"].is<const char*>()) s.wifiClientSSID = sanitizeWifiClientSsidValue(doc["wifiClientSSID"].as<String>());
    
    // Auto power-off
    if (doc["autoPowerOffMinutes"].is<int>()) {
        s.autoPowerOffMinutes = clampU8Value(doc["autoPowerOffMinutes"].as<int>(), 0, 60);
    }
    if (doc["apTimeoutMinutes"].is<int>()) {
        s.apTimeoutMinutes = clampApTimeoutValue(doc["apTimeoutMinutes"].as<int>());
    }
    
    // Voice settings
    if (doc["voiceAlertMode"].is<int>()) {
        int mode = doc["voiceAlertMode"].as<int>();
        mode = std::max(static_cast<int>(VOICE_MODE_DISABLED), std::min(mode, static_cast<int>(VOICE_MODE_BAND_FREQ)));
        s.voiceAlertMode = static_cast<VoiceAlertMode>(mode);
    }
    if (doc["voiceDirectionEnabled"].is<bool>()) s.voiceDirectionEnabled = doc["voiceDirectionEnabled"];
    if (doc["announceBogeyCount"].is<bool>()) s.announceBogeyCount = doc["announceBogeyCount"];
    if (doc["muteVoiceIfVolZero"].is<bool>()) s.muteVoiceIfVolZero = doc["muteVoiceIfVolZero"];
    if (doc["voiceVolume"].is<int>()) s.voiceVolume = clampU8Value(doc["voiceVolume"].as<int>(), 0, 100);
    if (doc["alertVolumeFadeEnabled"].is<bool>()) s.alertVolumeFadeEnabled = doc["alertVolumeFadeEnabled"];
    if (doc["alertVolumeFadeDelaySec"].is<int>()) {
        s.alertVolumeFadeDelaySec = clampU8Value(doc["alertVolumeFadeDelaySec"].as<int>(), 1, 10);
    }
    if (doc["alertVolumeFadeVolume"].is<int>()) {
        s.alertVolumeFadeVolume = clampU8Value(doc["alertVolumeFadeVolume"].as<int>(), 0, 9);
    }
    if (doc["speedVolumeEnabled"].is<bool>()) s.speedVolumeEnabled = doc["speedVolumeEnabled"];
    if (doc["speedVolumeThresholdMph"].is<int>()) {
        s.speedVolumeThresholdMph = clampU8Value(doc["speedVolumeThresholdMph"].as<int>(), 10, 100);
    }
    if (doc["speedVolumeBoost"].is<int>()) {
        s.speedVolumeBoost = clampU8Value(doc["speedVolumeBoost"].as<int>(), 1, 5);
    }
    if (doc["lowSpeedMuteEnabled"].is<bool>()) s.lowSpeedMuteEnabled = doc["lowSpeedMuteEnabled"];
    if (doc["lowSpeedMuteThresholdMph"].is<int>()) {
        s.lowSpeedMuteThresholdMph = clampU8Value(doc["lowSpeedMuteThresholdMph"].as<int>(), 1, 30);
    }
    if (doc["announceSecondaryAlerts"].is<bool>()) s.announceSecondaryAlerts = doc["announceSecondaryAlerts"];
    if (doc["secondaryLaser"].is<bool>()) s.secondaryLaser = doc["secondaryLaser"];
    if (doc["secondaryKa"].is<bool>()) s.secondaryKa = doc["secondaryKa"];
    if (doc["secondaryK"].is<bool>()) s.secondaryK = doc["secondaryK"];
    if (doc["secondaryX"].is<bool>()) s.secondaryX = doc["secondaryX"];
    
    // Auto-push slot settings
    // Only allow backup to enable auto-push (avoid stale backups disabling it)
    if (doc["autoPushEnabled"].is<bool>() && doc["autoPushEnabled"].as<bool>()) {
        s.autoPushEnabled = true;
    }
    if (doc["activeSlot"].is<int>()) s.activeSlot = std::max(0, std::min(doc["activeSlot"].as<int>(), 2));
    if (doc["slot0Name"].is<const char*>()) s.slot0Name = sanitizeSlotNameValue(doc["slot0Name"].as<String>());
    if (doc["slot1Name"].is<const char*>()) s.slot1Name = sanitizeSlotNameValue(doc["slot1Name"].as<String>());
    if (doc["slot2Name"].is<const char*>()) s.slot2Name = sanitizeSlotNameValue(doc["slot2Name"].as<String>());
    if (doc["slot0Color"].is<int>()) s.slot0Color = doc["slot0Color"];
    if (doc["slot1Color"].is<int>()) s.slot1Color = doc["slot1Color"];
    if (doc["slot2Color"].is<int>()) s.slot2Color = doc["slot2Color"];
    if (doc["slot0Volume"].is<int>()) s.slot0Volume = clampSlotVolumeValue(doc["slot0Volume"].as<int>());
    if (doc["slot1Volume"].is<int>()) s.slot1Volume = clampSlotVolumeValue(doc["slot1Volume"].as<int>());
    if (doc["slot2Volume"].is<int>()) s.slot2Volume = clampSlotVolumeValue(doc["slot2Volume"].as<int>());
    if (doc["slot0MuteVolume"].is<int>()) s.slot0MuteVolume = clampSlotVolumeValue(doc["slot0MuteVolume"].as<int>());
    if (doc["slot1MuteVolume"].is<int>()) s.slot1MuteVolume = clampSlotVolumeValue(doc["slot1MuteVolume"].as<int>());
    if (doc["slot2MuteVolume"].is<int>()) s.slot2MuteVolume = clampSlotVolumeValue(doc["slot2MuteVolume"].as<int>());
    if (doc["slot0DarkMode"].is<bool>()) s.slot0DarkMode = doc["slot0DarkMode"];
    if (doc["slot1DarkMode"].is<bool>()) s.slot1DarkMode = doc["slot1DarkMode"];
    if (doc["slot2DarkMode"].is<bool>()) s.slot2DarkMode = doc["slot2DarkMode"];
    if (doc["slot0MuteToZero"].is<bool>()) s.slot0MuteToZero = doc["slot0MuteToZero"];
    if (doc["slot1MuteToZero"].is<bool>()) s.slot1MuteToZero = doc["slot1MuteToZero"];
    if (doc["slot2MuteToZero"].is<bool>()) s.slot2MuteToZero = doc["slot2MuteToZero"];
    if (doc["slot0AlertPersist"].is<int>()) s.slot0AlertPersist = clampU8Value(doc["slot0AlertPersist"].as<int>(), 0, 5);
    if (doc["slot1AlertPersist"].is<int>()) s.slot1AlertPersist = clampU8Value(doc["slot1AlertPersist"].as<int>(), 0, 5);
    if (doc["slot2AlertPersist"].is<int>()) s.slot2AlertPersist = clampU8Value(doc["slot2AlertPersist"].as<int>(), 0, 5);
    if (doc["slot0PriorityArrow"].is<bool>()) s.slot0PriorityArrow = doc["slot0PriorityArrow"];
    if (doc["slot1PriorityArrow"].is<bool>()) s.slot1PriorityArrow = doc["slot1PriorityArrow"];
    if (doc["slot2PriorityArrow"].is<bool>()) s.slot2PriorityArrow = doc["slot2PriorityArrow"];
    if (doc["slot0ProfileName"].is<const char*>()) s.slot0_default.profileName = sanitizeProfileNameValue(doc["slot0ProfileName"].as<String>());
    if (doc["slot0Mode"].is<int>()) s.slot0_default.mode = normalizeV1ModeValue(doc["slot0Mode"].as<int>());
    if (doc["slot1ProfileName"].is<const char*>()) s.slot1_highway.profileName = sanitizeProfileNameValue(doc["slot1ProfileName"].as<String>());
    if (doc["slot1Mode"].is<int>()) s.slot1_highway.mode = normalizeV1ModeValue(doc["slot1Mode"].as<int>());
    if (doc["slot2ProfileName"].is<const char*>()) s.slot2_comfort.profileName = sanitizeProfileNameValue(doc["slot2ProfileName"].as<String>());
    if (doc["slot2Mode"].is<int>()) s.slot2_comfort.mode = normalizeV1ModeValue(doc["slot2Mode"].as<int>());
    
    // Restore V1 profiles if present
    int profilesRestored = 0;
    if (doc["profiles"].is<JsonArray>()) {
        JsonArray profilesArr = doc["profiles"].as<JsonArray>();
        for (JsonObject p : profilesArr) {
            if (!p["name"].is<const char*>() || !p["bytes"].is<JsonArray>()) {
                continue;  // Skip invalid profile entries
            }
            
            V1Profile profile;
            profile.name = sanitizeProfileNameValue(p["name"].as<String>());
            if (profile.name.length() == 0) {
                continue;
            }
            if (p["description"].is<const char*>()) {
                profile.description = sanitizeProfileDescriptionValue(p["description"].as<String>());
            }
            if (p["displayOn"].is<bool>()) profile.displayOn = p["displayOn"];
            if (p["mainVolume"].is<int>()) profile.mainVolume = clampSlotVolumeValue(p["mainVolume"].as<int>());
            if (p["mutedVolume"].is<int>()) profile.mutedVolume = clampSlotVolumeValue(p["mutedVolume"].as<int>());
            
            JsonArray bytes = p["bytes"].as<JsonArray>();
            if (bytes.size() == 6) {
                for (int i = 0; i < 6; i++) {
                    profile.settings.bytes[i] = bytes[i].as<uint8_t>();
                }
                
                ProfileSaveResult result = v1ProfileManager.saveProfile(profile);
                if (result.success) {
                    profilesRestored++;
                    Serial.printf("[Settings] Restored profile: %s\n", profile.name.c_str());
                } else {
                    Serial.printf("[Settings] Failed to restore profile: %s - %s\n", 
                                  profile.name.c_str(), result.error.c_str());
                }
            }
        }
    }
    
    // Save to flash
    settingsManager.save();

    obdHandler.setVwDataEnabled(settingsManager.get().obdVwDataEnabled);
    gpsRuntimeModule.setEnabled(settingsManager.get().gpsEnabled);
    speedSourceSelector.setGpsEnabled(settingsManager.get().gpsEnabled);
    
    Serial.printf("[Settings] Restored from uploaded backup (%d profiles)\n", profilesRestored);
    
    // Build response with profile count
    String response = "{\"success\":true,\"message\":\"Settings restored successfully";
    if (profilesRestored > 0) {
        response += " (" + String(profilesRestored) + " profiles)";
    }
    response += "\"}";
    server.send(200, "application/json", response);
}

// ==================== WiFi Client (STA) API Handlers ====================

void WiFiManager::handleWifiClientStatus() {
    markUiActivity();
    
    const V1Settings& settings = settingsManager.get();
    
    JsonDocument doc;
    doc["enabled"] = settings.wifiClientEnabled;
    doc["savedSSID"] = settings.wifiClientSSID;
    
    // Map state to string
    const char* stateStr = "unknown";
    switch (wifiClientState) {
        case WIFI_CLIENT_DISABLED: stateStr = "disabled"; break;
        case WIFI_CLIENT_DISCONNECTED: stateStr = "disconnected"; break;
        case WIFI_CLIENT_CONNECTING: stateStr = "connecting"; break;
        case WIFI_CLIENT_CONNECTED: stateStr = "connected"; break;
        case WIFI_CLIENT_FAILED: stateStr = "failed"; break;
    }
    doc["state"] = stateStr;
    
    if (wifiClientState == WIFI_CLIENT_CONNECTED) {
        doc["connectedSSID"] = WiFi.SSID();
        doc["ip"] = WiFi.localIP().toString();
        doc["rssi"] = WiFi.RSSI();
    }
    
    doc["scanRunning"] = wifiScanRunning;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void WiFiManager::handleWifiClientScan() {
    if (!checkRateLimit()) return;
    markUiActivity();
    
    Serial.println("[HTTP] POST /api/wifi/scan");
    
    // Check if scan is already running - return current results
    if (wifiScanRunning) {
        int16_t scanResult = WiFi.scanComplete();
        if (scanResult == WIFI_SCAN_RUNNING) {
            server.send(200, "application/json", "{\"scanning\":true,\"networks\":[]}");
            return;
        }
    }
    
    // Check if we have results from a completed scan
    int16_t scanResult = WiFi.scanComplete();
    if (scanResult > 0) {
        // Return results
        std::vector<ScannedNetwork> networks = getScannedNetworks();
        
        JsonDocument doc;
        doc["scanning"] = false;
        JsonArray arr = doc["networks"].to<JsonArray>();
        
        for (const auto& net : networks) {
            JsonObject obj = arr.add<JsonObject>();
            obj["ssid"] = net.ssid;
            obj["rssi"] = net.rssi;
            obj["secure"] = !net.isOpen();
        }
        
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
        return;
    }
    
    // Start a new scan
    if (startWifiScan()) {
        server.send(200, "application/json", "{\"scanning\":true,\"networks\":[]}");
    } else {
        server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to start scan\"}");
    }
}

void WiFiManager::handleWifiClientConnect() {
    if (!checkRateLimit()) return;
    markUiActivity();
    
    Serial.println("[HTTP] POST /api/wifi/connect");
    
    // Parse JSON body
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing request body\"}");
        return;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }
    
    String ssid = doc["ssid"] | "";
    String password = doc["password"] | "";
    
    if (ssid.length() == 0) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"SSID required\"}");
        return;
    }
    
    // Note: Password can be empty for open networks
    if (connectToNetwork(ssid, password)) {
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Connecting...\"}");
    } else {
        server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to start connection\"}");
    }
}

void WiFiManager::handleWifiClientDisconnect() {
    if (!checkRateLimit()) return;
    markUiActivity();
    
    Serial.println("[HTTP] POST /api/wifi/disconnect");
    
    disconnectFromNetwork();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Disconnected\"}");
}

void WiFiManager::handleWifiClientForget() {
    if (!checkRateLimit()) return;
    markUiActivity();
    
    Serial.println("[HTTP] POST /api/wifi/forget");
    
    // Disconnect if connected
    disconnectFromNetwork();
    
    // Clear saved credentials
    settingsManager.clearWifiClientCredentials();
    
    // Switch back to AP-only mode
    wifiClientState = WIFI_CLIENT_DISABLED;
    WiFi.mode(WIFI_AP);
    
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi credentials forgotten\"}");
}

void WiFiManager::handleWifiClientEnable() {
    if (!checkRateLimit()) return;
    markUiActivity();
    
    // Parse JSON body for enable state
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    
    if (err || !doc["enabled"].is<bool>()) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing enabled field\"}");
        return;
    }
    
    bool enable = doc["enabled"];
    Serial.printf("[HTTP] POST /api/wifi/enable: %s\n", enable ? "true" : "false");
    
    const V1Settings& settings = settingsManager.get();
    
    if (enable) {
        // Enable WiFi client mode
        settingsManager.setWifiClientEnabled(true);
        
        // If we have saved credentials, try to connect
        if (settings.wifiClientSSID.length() > 0) {
            String savedPassword = settingsManager.getWifiClientPassword();
            connectToNetwork(settings.wifiClientSSID, savedPassword);
        } else {
            wifiClientState = WIFI_CLIENT_DISCONNECTED;
        }
        server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi client enabled\"}");
    } else {
        // Disable WiFi client mode
        disconnectFromNetwork();
        settingsManager.setWifiClientEnabled(false);
        wifiClientState = WIFI_CLIENT_DISABLED;
        WiFi.mode(WIFI_AP);
        server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi client disabled\"}");
    }
}

// ==================== OBD API Handlers ====================

void WiFiManager::handleObdStatus() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::sendStatus(server, obdHandler, bleClient, settingsManager.get());
}

void WiFiManager::handleObdScan() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::handleScan(server, obdHandler, bleClient);
}

void WiFiManager::handleObdScanStop() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::handleScanStop(server, obdHandler);
}

void WiFiManager::handleObdDevices() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::sendDevices(server, obdHandler);
}

void WiFiManager::handleObdDevicesClear() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::handleDevicesClear(server, obdHandler);
}

void WiFiManager::handleObdConnect() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::handleConnect(server, obdHandler, bleClient);
}

void WiFiManager::handleObdDisconnect() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::handleDisconnect(server, obdHandler);
}

void WiFiManager::handleObdConfig() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::handleConfig(server, obdHandler, settingsManager);
}

void WiFiManager::handleObdRemembered() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::sendRemembered(server, obdHandler);
}

void WiFiManager::handleObdRememberedAutoConnect() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::handleRememberedAutoConnect(server, obdHandler);
}

void WiFiManager::handleObdForget() {
    if (!checkRateLimit()) return;
    markUiActivity();
    ObdApiService::handleForget(server, obdHandler);
}

void WiFiManager::handleGpsStatus() {
    markUiActivity();

    const uint32_t nowMs = millis();
    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    const SpeedSelectorStatus speedStatus = speedSourceSelector.snapshot(nowMs);

    JsonDocument doc;
    doc["enabled"] = settingsManager.get().gpsEnabled;
    doc["runtimeEnabled"] = gpsStatus.enabled;
    doc["mode"] = (gpsStatus.parserActive || gpsStatus.moduleDetected || gpsStatus.hardwareSamples > 0)
                      ? "runtime"
                      : "scaffold";
    doc["sampleValid"] = gpsStatus.sampleValid;
    doc["hasFix"] = gpsStatus.hasFix;
    doc["satellites"] = gpsStatus.satellites;
    doc["injectedSamples"] = gpsStatus.injectedSamples;
    doc["moduleDetected"] = gpsStatus.moduleDetected;
    doc["detectionTimedOut"] = gpsStatus.detectionTimedOut;
    doc["parserActive"] = gpsStatus.parserActive;
    doc["hardwareSamples"] = gpsStatus.hardwareSamples;
    doc["bytesRead"] = gpsStatus.bytesRead;
    doc["sentencesSeen"] = gpsStatus.sentencesSeen;
    doc["sentencesParsed"] = gpsStatus.sentencesParsed;
    doc["parseFailures"] = gpsStatus.parseFailures;
    doc["checksumFailures"] = gpsStatus.checksumFailures;
    doc["bufferOverruns"] = gpsStatus.bufferOverruns;

    if (std::isnan(gpsStatus.hdop)) {
        doc["hdop"] = nullptr;
    } else {
        doc["hdop"] = gpsStatus.hdop;
    }
    doc["locationValid"] = gpsStatus.locationValid;
    if (gpsStatus.locationValid) {
        doc["latitude"] = gpsStatus.latitudeDeg;
        doc["longitude"] = gpsStatus.longitudeDeg;
    } else {
        doc["latitude"] = nullptr;
        doc["longitude"] = nullptr;
    }

    if (gpsStatus.sampleValid) {
        doc["speedMph"] = gpsStatus.speedMph;
        doc["sampleTsMs"] = gpsStatus.sampleTsMs;
    } else {
        doc["speedMph"] = nullptr;
        doc["sampleTsMs"] = nullptr;
    }

    if (gpsStatus.sampleAgeMs == UINT32_MAX) {
        doc["sampleAgeMs"] = nullptr;
    } else {
        doc["sampleAgeMs"] = gpsStatus.sampleAgeMs;
    }
    if (gpsStatus.lastSentenceTsMs == 0) {
        doc["lastSentenceTsMs"] = nullptr;
    } else {
        doc["lastSentenceTsMs"] = gpsStatus.lastSentenceTsMs;
    }
    const GpsObservationLogStats gpsLogStats = gpsObservationLog.stats();
    JsonObject observationsObj = doc["observations"].to<JsonObject>();
    observationsObj["published"] = gpsLogStats.published;
    observationsObj["drops"] = gpsLogStats.drops;
    observationsObj["size"] = static_cast<uint32_t>(gpsLogStats.size);
    observationsObj["capacity"] = static_cast<uint32_t>(GpsObservationLog::kCapacity);

    JsonObject speedObj = doc["speedSource"].to<JsonObject>();
    speedObj["selected"] = SpeedSourceSelector::sourceName(speedStatus.selectedSource);
    speedObj["gpsEnabled"] = speedStatus.gpsEnabled;
    speedObj["obdConnected"] = speedStatus.obdConnected;
    speedObj["obdFresh"] = speedStatus.obdFresh;
    speedObj["gpsFresh"] = speedStatus.gpsFresh;
    speedObj["sourceSwitches"] = speedStatus.sourceSwitches;
    if (speedStatus.selectedSource == SpeedSource::NONE) {
        speedObj["selectedMph"] = nullptr;
        speedObj["selectedAgeMs"] = nullptr;
    } else {
        speedObj["selectedMph"] = speedStatus.selectedSpeedMph;
        speedObj["selectedAgeMs"] = speedStatus.selectedAgeMs;
    }
    if (speedStatus.obdAgeMs == UINT32_MAX) {
        speedObj["obdAgeMs"] = nullptr;
    } else {
        speedObj["obdAgeMs"] = speedStatus.obdAgeMs;
    }
    if (speedStatus.gpsAgeMs == UINT32_MAX) {
        speedObj["gpsAgeMs"] = nullptr;
    } else {
        speedObj["gpsAgeMs"] = speedStatus.gpsAgeMs;
    }

    const V1Settings& settings = settingsManager.get();
    const GpsLockoutCoreGuardStatus lockoutGuard = gpsLockoutEvaluateCoreGuard(
        settings.gpsLockoutCoreGuardEnabled,
        settings.gpsLockoutMaxQueueDrops,
        settings.gpsLockoutMaxPerfDrops,
        settings.gpsLockoutMaxEventBusDrops,
        perfCounters.queueDrops.load(),
        perfCounters.perfDrop.load(),
        systemEventBus.getDropCount());
    JsonObject lockoutObj = doc["lockout"].to<JsonObject>();
    lockoutObj["mode"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    lockoutObj["modeRaw"] = static_cast<int>(settings.gpsLockoutMode);
    lockoutObj["coreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    lockoutObj["coreGuardTripped"] = lockoutGuard.tripped;
    lockoutObj["coreGuardReason"] = lockoutGuard.reason;
    lockoutObj["enforceAllowed"] = (settings.gpsLockoutMode == LOCKOUT_RUNTIME_ENFORCE) &&
                                   !lockoutGuard.tripped;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void WiFiManager::handleGpsObservations() {
    if (!checkRateLimit()) return;
    markUiActivity();

    uint16_t limit = 16;
    if (server.hasArg("limit")) {
        limit = clampU16Value(server.arg("limit").toInt(), 1, 32);
    }

    GpsObservation samples[32] = {};
    const size_t count = gpsObservationLog.copyRecent(samples, limit);
    const GpsObservationLogStats stats = gpsObservationLog.stats();

    JsonDocument doc;
    doc["success"] = true;
    doc["count"] = static_cast<uint32_t>(count);
    doc["published"] = stats.published;
    doc["drops"] = stats.drops;
    doc["size"] = static_cast<uint32_t>(stats.size);
    doc["capacity"] = static_cast<uint32_t>(GpsObservationLog::kCapacity);

    JsonArray samplesArray = doc["samples"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i) {
        const GpsObservation& sample = samples[i];
        JsonObject entry = samplesArray.add<JsonObject>();
        entry["tsMs"] = sample.tsMs;
        entry["hasFix"] = sample.hasFix;
        entry["satellites"] = sample.satellites;
        if (std::isnan(sample.hdop)) {
            entry["hdop"] = nullptr;
        } else {
            entry["hdop"] = sample.hdop;
        }
        if (sample.speedValid) {
            entry["speedMph"] = sample.speedMph;
        } else {
            entry["speedMph"] = nullptr;
        }
        if (sample.locationValid) {
            entry["latitude"] = sample.latitudeDeg;
            entry["longitude"] = sample.longitudeDeg;
        } else {
            entry["latitude"] = nullptr;
            entry["longitude"] = nullptr;
        }
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void WiFiManager::handleGpsConfig() {
    if (!checkRateLimit()) return;
    markUiActivity();

    const V1Settings& currentSettings = settingsManager.get();
    V1Settings& mutableSettings = const_cast<V1Settings&>(currentSettings);

    bool hasEnabled = false;
    bool enabled = currentSettings.gpsEnabled;
    bool hasScaffoldSample = false;
    float scaffoldSpeedMph = 0.0f;
    bool scaffoldHasFix = true;
    uint8_t scaffoldSatellites = 0;
    float scaffoldHdop = NAN;
    float scaffoldLatitudeDeg = NAN;
    float scaffoldLongitudeDeg = NAN;
    bool hasLockoutMode = false;
    LockoutRuntimeMode lockoutMode = currentSettings.gpsLockoutMode;
    bool hasCoreGuardEnabled = false;
    bool coreGuardEnabled = currentSettings.gpsLockoutCoreGuardEnabled;
    bool hasMaxQueueDrops = false;
    uint16_t maxQueueDrops = currentSettings.gpsLockoutMaxQueueDrops;
    bool hasMaxPerfDrops = false;
    uint16_t maxPerfDrops = currentSettings.gpsLockoutMaxPerfDrops;
    bool hasMaxEventBusDrops = false;
    uint16_t maxEventBusDrops = currentSettings.gpsLockoutMaxEventBusDrops;

    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument body;
        DeserializationError error = deserializeJson(body, server.arg("plain"));
        if (error) {
            server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        if (body["enabled"].is<bool>()) {
            enabled = body["enabled"].as<bool>();
            hasEnabled = true;
        }
        if (body["lockoutMode"].is<int>()) {
            lockoutMode = clampLockoutRuntimeModeValue(body["lockoutMode"].as<int>());
            hasLockoutMode = true;
        } else if (body["lockoutMode"].is<const char*>()) {
            lockoutMode = gpsLockoutParseRuntimeModeArg(body["lockoutMode"].as<String>(), lockoutMode);
            hasLockoutMode = true;
        } else if (body["gpsLockoutMode"].is<int>()) {
            lockoutMode = clampLockoutRuntimeModeValue(body["gpsLockoutMode"].as<int>());
            hasLockoutMode = true;
        } else if (body["gpsLockoutMode"].is<const char*>()) {
            lockoutMode = gpsLockoutParseRuntimeModeArg(body["gpsLockoutMode"].as<String>(), lockoutMode);
            hasLockoutMode = true;
        }
        if (body["lockoutCoreGuardEnabled"].is<bool>()) {
            coreGuardEnabled = body["lockoutCoreGuardEnabled"].as<bool>();
            hasCoreGuardEnabled = true;
        } else if (body["gpsLockoutCoreGuardEnabled"].is<bool>()) {
            coreGuardEnabled = body["gpsLockoutCoreGuardEnabled"].as<bool>();
            hasCoreGuardEnabled = true;
        }
        if (body["lockoutMaxQueueDrops"].is<int>()) {
            maxQueueDrops = clampU16Value(body["lockoutMaxQueueDrops"].as<int>(), 0, 65535);
            hasMaxQueueDrops = true;
        } else if (body["gpsLockoutMaxQueueDrops"].is<int>()) {
            maxQueueDrops = clampU16Value(body["gpsLockoutMaxQueueDrops"].as<int>(), 0, 65535);
            hasMaxQueueDrops = true;
        }
        if (body["lockoutMaxPerfDrops"].is<int>()) {
            maxPerfDrops = clampU16Value(body["lockoutMaxPerfDrops"].as<int>(), 0, 65535);
            hasMaxPerfDrops = true;
        } else if (body["gpsLockoutMaxPerfDrops"].is<int>()) {
            maxPerfDrops = clampU16Value(body["gpsLockoutMaxPerfDrops"].as<int>(), 0, 65535);
            hasMaxPerfDrops = true;
        }
        if (body["lockoutMaxEventBusDrops"].is<int>()) {
            maxEventBusDrops = clampU16Value(body["lockoutMaxEventBusDrops"].as<int>(), 0, 65535);
            hasMaxEventBusDrops = true;
        } else if (body["gpsLockoutMaxEventBusDrops"].is<int>()) {
            maxEventBusDrops = clampU16Value(body["gpsLockoutMaxEventBusDrops"].as<int>(), 0, 65535);
            hasMaxEventBusDrops = true;
        }
        if (body["speedMph"].is<float>() || body["speedMph"].is<double>() || body["speedMph"].is<int>()) {
            scaffoldSpeedMph = body["speedMph"].as<float>();
            hasScaffoldSample = true;
        }
        if (body["hasFix"].is<bool>()) {
            scaffoldHasFix = body["hasFix"].as<bool>();
        }
        if (body["satellites"].is<int>()) {
            int sats = body["satellites"].as<int>();
            scaffoldSatellites = static_cast<uint8_t>(std::max(0, std::min(sats, 99)));
        }
        if (body["hdop"].is<float>() || body["hdop"].is<double>() || body["hdop"].is<int>()) {
            scaffoldHdop = body["hdop"].as<float>();
        }
        if (body["latitude"].is<float>() || body["latitude"].is<double>() || body["latitude"].is<int>()) {
            scaffoldLatitudeDeg = body["latitude"].as<float>();
        }
        if (body["longitude"].is<float>() || body["longitude"].is<double>() || body["longitude"].is<int>()) {
            scaffoldLongitudeDeg = body["longitude"].as<float>();
        }
    }

    if (!hasEnabled && server.hasArg("enabled")) {
        String value = server.arg("enabled");
        value.toLowerCase();
        if (value == "1" || value == "true" || value == "on") {
            enabled = true;
            hasEnabled = true;
        } else if (value == "0" || value == "false" || value == "off") {
            enabled = false;
            hasEnabled = true;
        }
    }
    if (!hasLockoutMode && server.hasArg("lockoutMode")) {
        lockoutMode = gpsLockoutParseRuntimeModeArg(server.arg("lockoutMode"), lockoutMode);
        hasLockoutMode = true;
    }
    if (!hasLockoutMode && server.hasArg("gpsLockoutMode")) {
        lockoutMode = gpsLockoutParseRuntimeModeArg(server.arg("gpsLockoutMode"), lockoutMode);
        hasLockoutMode = true;
    }
    if (!hasCoreGuardEnabled && server.hasArg("lockoutCoreGuardEnabled")) {
        String value = server.arg("lockoutCoreGuardEnabled");
        value.toLowerCase();
        coreGuardEnabled = (value == "1" || value == "true" || value == "on");
        hasCoreGuardEnabled = true;
    }
    if (!hasCoreGuardEnabled && server.hasArg("gpsLockoutCoreGuardEnabled")) {
        String value = server.arg("gpsLockoutCoreGuardEnabled");
        value.toLowerCase();
        coreGuardEnabled = (value == "1" || value == "true" || value == "on");
        hasCoreGuardEnabled = true;
    }
    if (!hasMaxQueueDrops && server.hasArg("lockoutMaxQueueDrops")) {
        maxQueueDrops = clampU16Value(server.arg("lockoutMaxQueueDrops").toInt(), 0, 65535);
        hasMaxQueueDrops = true;
    }
    if (!hasMaxQueueDrops && server.hasArg("gpsLockoutMaxQueueDrops")) {
        maxQueueDrops = clampU16Value(server.arg("gpsLockoutMaxQueueDrops").toInt(), 0, 65535);
        hasMaxQueueDrops = true;
    }
    if (!hasMaxPerfDrops && server.hasArg("lockoutMaxPerfDrops")) {
        maxPerfDrops = clampU16Value(server.arg("lockoutMaxPerfDrops").toInt(), 0, 65535);
        hasMaxPerfDrops = true;
    }
    if (!hasMaxPerfDrops && server.hasArg("gpsLockoutMaxPerfDrops")) {
        maxPerfDrops = clampU16Value(server.arg("gpsLockoutMaxPerfDrops").toInt(), 0, 65535);
        hasMaxPerfDrops = true;
    }
    if (!hasMaxEventBusDrops && server.hasArg("lockoutMaxEventBusDrops")) {
        maxEventBusDrops = clampU16Value(server.arg("lockoutMaxEventBusDrops").toInt(), 0, 65535);
        hasMaxEventBusDrops = true;
    }
    if (!hasMaxEventBusDrops && server.hasArg("gpsLockoutMaxEventBusDrops")) {
        maxEventBusDrops = clampU16Value(server.arg("gpsLockoutMaxEventBusDrops").toInt(), 0, 65535);
        hasMaxEventBusDrops = true;
    }

    if (!hasEnabled) {
        bool hasLockoutUpdate = hasLockoutMode || hasCoreGuardEnabled ||
                                hasMaxQueueDrops || hasMaxPerfDrops || hasMaxEventBusDrops;
        if (!hasLockoutUpdate) {
            server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing enabled or lockout settings\"}");
            return;
        }
    }

    if (hasScaffoldSample &&
        (!std::isfinite(scaffoldSpeedMph) ||
         scaffoldSpeedMph < 0.0f ||
         scaffoldSpeedMph > SpeedSourceSelector::MAX_VALID_SPEED_MPH)) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"speedMph out of range\"}");
        return;
    }
    if (std::isfinite(scaffoldHdop) && scaffoldHdop < 0.0f) {
        scaffoldHdop = 0.0f;
    }
    const bool hasScaffoldLatitude = std::isfinite(scaffoldLatitudeDeg);
    const bool hasScaffoldLongitude = std::isfinite(scaffoldLongitudeDeg);
    if (hasScaffoldLatitude != hasScaffoldLongitude) {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"latitude and longitude must be provided together\"}");
        return;
    }
    if (hasScaffoldLatitude &&
        (scaffoldLatitudeDeg < -90.0f || scaffoldLatitudeDeg > 90.0f ||
         scaffoldLongitudeDeg < -180.0f || scaffoldLongitudeDeg > 180.0f)) {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"latitude/longitude out of range\"}");
        return;
    }

    if (hasEnabled) {
        settingsManager.setGpsEnabled(enabled);
        gpsRuntimeModule.setEnabled(enabled);
        speedSourceSelector.setGpsEnabled(enabled);
    }

    bool lockoutSettingsChanged = false;
    if (hasLockoutMode && mutableSettings.gpsLockoutMode != lockoutMode) {
        mutableSettings.gpsLockoutMode = lockoutMode;
        lockoutSettingsChanged = true;
    }
    if (hasCoreGuardEnabled && mutableSettings.gpsLockoutCoreGuardEnabled != coreGuardEnabled) {
        mutableSettings.gpsLockoutCoreGuardEnabled = coreGuardEnabled;
        lockoutSettingsChanged = true;
    }
    if (hasMaxQueueDrops && mutableSettings.gpsLockoutMaxQueueDrops != maxQueueDrops) {
        mutableSettings.gpsLockoutMaxQueueDrops = maxQueueDrops;
        lockoutSettingsChanged = true;
    }
    if (hasMaxPerfDrops && mutableSettings.gpsLockoutMaxPerfDrops != maxPerfDrops) {
        mutableSettings.gpsLockoutMaxPerfDrops = maxPerfDrops;
        lockoutSettingsChanged = true;
    }
    if (hasMaxEventBusDrops && mutableSettings.gpsLockoutMaxEventBusDrops != maxEventBusDrops) {
        mutableSettings.gpsLockoutMaxEventBusDrops = maxEventBusDrops;
        lockoutSettingsChanged = true;
    }
    if (lockoutSettingsChanged) {
        settingsManager.save();
    }

    if (enabled && hasScaffoldSample) {
        gpsRuntimeModule.setScaffoldSample(scaffoldSpeedMph,
                                           scaffoldHasFix,
                                           scaffoldSatellites,
                                           scaffoldHdop,
                                           millis(),
                                           scaffoldLatitudeDeg,
                                           scaffoldLongitudeDeg);
    }

    const uint32_t nowMs = millis();
    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    const SpeedSelectorStatus speedStatus = speedSourceSelector.snapshot(nowMs);
    const V1Settings& settings = settingsManager.get();
    const GpsLockoutCoreGuardStatus lockoutGuard = gpsLockoutEvaluateCoreGuard(
        settings.gpsLockoutCoreGuardEnabled,
        settings.gpsLockoutMaxQueueDrops,
        settings.gpsLockoutMaxPerfDrops,
        settings.gpsLockoutMaxEventBusDrops,
        perfCounters.queueDrops.load(),
        perfCounters.perfDrop.load(),
        systemEventBus.getDropCount());

    JsonDocument response;
    response["success"] = true;
    response["enabled"] = settingsManager.get().gpsEnabled;
    response["runtimeEnabled"] = gpsStatus.enabled;
    response["sampleValid"] = gpsStatus.sampleValid;
    response["hasFix"] = gpsStatus.hasFix;
    response["injectedSamples"] = gpsStatus.injectedSamples;
    response["speedSource"] = SpeedSourceSelector::sourceName(speedStatus.selectedSource);
    response["lockoutMode"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    response["lockoutModeRaw"] = static_cast<int>(settings.gpsLockoutMode);
    response["lockoutCoreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    response["lockoutMaxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    response["lockoutMaxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    response["lockoutMaxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    response["lockoutCoreGuardTripped"] = lockoutGuard.tripped;
    response["lockoutCoreGuardReason"] = lockoutGuard.reason;
    response["locationValid"] = gpsStatus.locationValid;
    if (gpsStatus.locationValid) {
        response["latitude"] = gpsStatus.latitudeDeg;
        response["longitude"] = gpsStatus.longitudeDeg;
    } else {
        response["latitude"] = nullptr;
        response["longitude"] = nullptr;
    }
    const GpsObservationLogStats gpsLogStats = gpsObservationLog.stats();
    response["observationSize"] = static_cast<uint32_t>(gpsLogStats.size);
    response["observationDrops"] = gpsLogStats.drops;

    String json;
    serializeJson(response, json);
    server.send(200, "application/json", json);
}

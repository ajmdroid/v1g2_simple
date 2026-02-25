/**
 * WiFi Manager for V1 Gen2 Display
 * AP+STA: always-on access point serving the local UI/API
 *         plus optional station mode to connect to external network
 */

#include "wifi_manager_internals.h"
#include "perf_metrics.h"
#include "settings.h"
#include "perf_sd_logger.h"
#include "time_service.h"
#include <LittleFS.h>
#include "esp_wifi.h"

// Optional AP auto-timeout (milliseconds). Set to 0 to keep always-on behavior.
static constexpr unsigned long WIFI_AP_AUTO_TIMEOUT_MS = 0;            // e.g., 10 * 60 * 1000 for 10 minutes
static constexpr unsigned long WIFI_AP_INACTIVITY_GRACE_MS = 60 * 1000; // Require no UI activity/clients for this long before stopping

// ---- Promoted helpers (declared in wifi_manager_internals.h) ----

void dumpLittleFSRoot() {
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

const char* wifiClientStateApiName(WifiClientState state) {
    switch (state) {
        case WIFI_CLIENT_DISABLED: return "disabled";
        case WIFI_CLIENT_DISCONNECTED: return "disconnected";
        case WIFI_CLIENT_CONNECTING: return "connecting";
        case WIFI_CLIENT_CONNECTED: return "connected";
        case WIFI_CLIENT_FAILED: return "failed";
        default: return "unknown";
    }
}

// ---- Static helpers (used only in this TU) ----

static bool shouldUseApSta(const V1Settings& settings) {
    return settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0;
}

static void getWifiStartThresholds(bool apStaMode, uint32_t& minFree, uint32_t& minBlock) {
    minFree = apStaMode ? WiFiManager::WIFI_START_MIN_FREE_AP_STA
                        : WiFiManager::WIFI_START_MIN_FREE_AP_ONLY;
    minBlock = apStaMode ? WiFiManager::WIFI_START_MIN_BLOCK_AP_STA
                         : WiFiManager::WIFI_START_MIN_BLOCK_AP_ONLY;
}

static void getWifiRuntimeThresholds(bool apStaMode, bool staOnlyMode, uint32_t& minFree, uint32_t& minBlock) {
    if (apStaMode) {
        minFree = WiFiManager::WIFI_RUNTIME_MIN_FREE_AP_STA;
        minBlock = WiFiManager::WIFI_RUNTIME_MIN_BLOCK_AP_STA;
        return;
    }
    if (staOnlyMode) {
        minFree = WiFiManager::WIFI_RUNTIME_MIN_FREE_STA_ONLY;
        minBlock = WiFiManager::WIFI_RUNTIME_MIN_BLOCK_STA_ONLY;
        return;
    }
    minFree = WiFiManager::WIFI_RUNTIME_MIN_FREE_AP_ONLY;
    minBlock = WiFiManager::WIFI_RUNTIME_MIN_BLOCK_AP_ONLY;
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

WiFiManager::WiFiManager() : server(80), setupModeState(SETUP_MODE_OFF), apInterfaceEnabled(false), setupModeStartTime(0), rateLimitWindowStart(0), rateLimitRequestCount(0) {
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
    if (!apStaMode) {
        Serial.printf("[SetupMode] STA unavailable for this session (wifiClientEnabled=%s ssidLen=%u)\n",
                      settings.wifiClientEnabled ? "true" : "false",
                      static_cast<unsigned>(settings.wifiClientSSID.length()));
    }

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
    lastAnyClientSeenMs = setupModeStartTime;
    lastApStaCountPollMs = 0;
    cachedApStaCount = 0;
    lastMaintenanceFastMs = 0;
    lastStatusCheckMs = 0;
    lastTimeoutCheckMs = 0;
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
    apInterfaceEnabled = true;

    // When AP+STA mode is active, connect to the saved STA network directly.
    // WiFi.mode(WIFI_AP_STA) is already set and setupAP() has configured the AP,
    // so we can call WiFi.begin() immediately — no need for the deferred phase
    // machine that connectToNetwork() uses (which adds multi-loop-iteration
    // latency and mode-switch guards that are redundant here).
    if (apStaMode) {
        String savedPassword = settingsManager.getWifiClientPassword();
        Serial.printf("[SetupMode] STA connecting to '%s'\n", settings.wifiClientSSID.c_str());
        debugLogger.notifyWifiTransition(false);
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        WiFi.begin(settings.wifiClientSSID.c_str(), savedPassword.c_str());
        wifiClientState = WIFI_CLIENT_CONNECTING;
        wifiConnectStartMs = millis();
        wifiConnectPhase = WifiConnectPhase::IDLE;
    }

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

bool WiFiManager::stopSetupModeImmediate(bool emergencyLowDma) {
    if (emergencyLowDma) {
        // Emergency path: prioritize low-latency return to BLE/display loop.
        WIFI_LOG("[SetupMode] Emergency low_dma shutdown (fast path)\n");
        lowDmaCooldownUntilMs = millis() + WIFI_LOW_DMA_RETRY_COOLDOWN_MS;
        server.stop();
        WiFi.mode(WIFI_OFF);
        finalizeStopSetupMode();
        return true;
    }

    WIFI_LOG("[SetupMode] Stopping WiFi (immediate OFF contract)...\n");
    const wifi_mode_t currentMode = WiFi.getMode();
    const bool modeHasSta = (currentMode == WIFI_AP_STA || currentMode == WIFI_STA);
    const bool modeHasAp = (currentMode == WIFI_AP_STA || currentMode == WIFI_AP);

    // Stop server first, then release STA/AP without erasing configured credentials.
    server.stop();
    WIFI_LOG("[SetupMode] HTTP server stopped\n");
    if (modeHasSta &&
        (wifiClientState == WIFI_CLIENT_CONNECTED ||
         wifiClientState == WIFI_CLIENT_CONNECTING ||
         WiFi.status() == WL_CONNECTED)) {
        WiFi.disconnect(false, false);
        WIFI_LOG("[SetupMode] STA disconnected\n");
    }
    if (modeHasAp || apInterfaceEnabled) {
        if (!WiFi.enableAP(false)) {
            WiFi.softAPdisconnect(true);
        }
        WIFI_LOG("[SetupMode] AP disconnected\n");
    }

    WiFi.mode(WIFI_OFF);
    WIFI_LOG("[SetupMode] Radio stopped via WiFi.mode(WIFI_OFF)\n");
    finalizeStopSetupMode();
    return true;
}

void WiFiManager::finalizeStopSetupMode() {
    const String stopReason = wifiStopReason;
    const bool stopManual = wifiStopManual;
    const uint32_t stopDurMs = millis() - wifiStopStartMs;

    // ========== RESET ALL STATE ==========
    setupModeState = SETUP_MODE_OFF;
    apInterfaceEnabled = false;
    wifiClientState = WIFI_CLIENT_DISABLED;
    wifiScanRunning = false;
    wifiConnectStartMs = 0;
    wifiConnectPhase = WifiConnectPhase::IDLE;
    wifiConnectPhaseStartMs = 0;
    pendingConnectSSID = "";
    pendingConnectPassword = "";
    pendingConnectPersistCredentials = true;
    lastUiActivityMs = 0;
    lastClientSeenMs = 0;
    lastAnyClientSeenMs = 0;
    lastApStaCountPollMs = 0;
    cachedApStaCount = 0;
    lastMaintenanceFastMs = 0;
    lastStatusCheckMs = 0;
    lastTimeoutCheckMs = 0;
    lastReconnectAttemptMs = 0;
    wifiReconnectDeferredLogged = false;
    wasAutoStarted = false;
    lowDmaSinceMs = 0;
    wifiStopPhase = WifiStopPhase::IDLE;
    wifiStopPhaseStartMs = 0;
    wifiStopStartMs = 0;
    wifiStopReason = "";
    wifiStopManual = false;
    wifiStopHadSta = false;
    wifiStopHadAp = false;

    debugLogger.notifyWifiTransition(true);

    // ========== OBSERVABILITY ==========
    uint32_t freeInternalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t largestInternalAfter = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[SetupMode] WiFi OFF: reason=%s manual=%d radio=%d http=%d freeDma=%lu largestDma=%lu durMs=%lu\n",
                  stopReason.length() ? stopReason.c_str() : "unknown",
                  stopManual ? 1 : 0,
                  0,
                  0,
                  (unsigned long)freeInternalAfter,
                  (unsigned long)largestInternalAfter,
                  (unsigned long)stopDurMs);
}

void WiFiManager::processStopSetupModePhase() {
    if (wifiStopPhase == WifiStopPhase::IDLE) {
        return;
    }

    const unsigned long now = millis();
    if (wifiStopPhase != WifiStopPhase::STOP_HTTP_SERVER &&
        (now - wifiStopPhaseStartMs) < WIFI_STOP_PHASE_SETTLE_MS) {
        return;
    }

    switch (wifiStopPhase) {
        case WifiStopPhase::STOP_HTTP_SERVER:
            server.stop();
            WIFI_LOG("[SetupMode] Graceful stop phase: HTTP server stopped\n");
            wifiStopPhase = WifiStopPhase::DISCONNECT_STA;
            wifiStopPhaseStartMs = now;
            break;

        case WifiStopPhase::DISCONNECT_STA:
            if (wifiStopHadSta &&
                (wifiClientState == WIFI_CLIENT_CONNECTED ||
                 wifiClientState == WIFI_CLIENT_CONNECTING ||
                 WiFi.status() == WL_CONNECTED)) {
                WiFi.disconnect(false, false);
                WIFI_LOG("[SetupMode] Graceful stop phase: STA disconnected\n");
            }
            wifiStopPhase = WifiStopPhase::DISABLE_AP;
            wifiStopPhaseStartMs = now;
            break;

        case WifiStopPhase::DISABLE_AP:
            if (wifiStopHadAp || apInterfaceEnabled) {
                if (!WiFi.enableAP(false)) {
                    WiFi.softAPdisconnect(true);
                }
                apInterfaceEnabled = false;
                cachedApStaCount = 0;
                lastApStaCountPollMs = 0;
                WIFI_LOG("[SetupMode] Graceful stop phase: AP disabled\n");
            }
            wifiStopPhase = WifiStopPhase::MODE_OFF;
            wifiStopPhaseStartMs = now;
            break;

        case WifiStopPhase::MODE_OFF:
            WiFi.mode(WIFI_OFF);
            WIFI_LOG("[SetupMode] Graceful stop phase: radio OFF\n");
            wifiStopPhase = WifiStopPhase::FINALIZE;
            wifiStopPhaseStartMs = now;
            break;

        case WifiStopPhase::FINALIZE:
            finalizeStopSetupMode();
            break;

        case WifiStopPhase::IDLE:
        default:
            break;
    }
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
    const bool forceImmediate = emergencyLowDma || (strcmp(stopReason, "poweroff") == 0);
    const uint32_t stopStartMs = millis();
    uint32_t freeInternalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t largestInternalBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[SetupMode] Stopping WiFi: reason=%s manual=%d freeDma=%lu largestDma=%lu\n",
                  stopReason,
                  manual ? 1 : 0,
                  (unsigned long)freeInternalBefore,
                  (unsigned long)largestInternalBefore);

    if (wifiStopPhase != WifiStopPhase::IDLE) {
        if (forceImmediate) {
            wifiStopReason = stopReason;
            wifiStopManual = manual;
            wifiStopStartMs = stopStartMs;
            return stopSetupModeImmediate(emergencyLowDma);
        }
        WIFI_LOG("[SetupMode] stopSetupMode ignored (already stopping)\n");
        return true;
    }

    lowDmaSinceMs = 0;
    wifiStopReason = stopReason;
    wifiStopManual = manual;
    wifiStopStartMs = stopStartMs;

    if (forceImmediate) {
        return stopSetupModeImmediate(emergencyLowDma);
    }

    const wifi_mode_t currentMode = WiFi.getMode();
    wifiStopHadSta = (currentMode == WIFI_AP_STA || currentMode == WIFI_STA);
    wifiStopHadAp = (currentMode == WIFI_AP_STA || currentMode == WIFI_AP || apInterfaceEnabled);
    wifiStopPhase = WifiStopPhase::STOP_HTTP_SERVER;
    wifiStopPhaseStartMs = stopStartMs;
    WIFI_LOG("[SetupMode] Graceful stop scheduled: reason=%s hasSta=%d hasAp=%d\n",
             stopReason,
             wifiStopHadSta ? 1 : 0,
             wifiStopHadAp ? 1 : 0);
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


// --- make*Runtime() factory methods moved to wifi_runtimes.cpp ---


// --- setupWebServer() moved to wifi_routes.cpp ---

void WiFiManager::checkAutoTimeout() {
    uint8_t timeoutMins = settingsManager.getApTimeoutMinutes();
    if (timeoutMins == 0) return;  // Disabled (always on)
    if (!isSetupModeActive()) return;

    unsigned long timeoutMs = (unsigned long)timeoutMins * 60UL * 1000UL;
    unsigned long now = millis();
    int staCount = cachedApStaCount;
    if (lastApStaCountPollMs == 0 ||
        (now - lastApStaCountPollMs) >= AP_STA_COUNT_POLL_MS) {
        // softAPgetStationNum() can be slow on some loops; cache short-term to
        // reduce loop jitter without changing timeout semantics materially.
        staCount = WiFi.softAPgetStationNum();
        cachedApStaCount = staCount;
        lastApStaCountPollMs = now;
    }
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

    // Graceful shutdown runs as a staged sequence to avoid long stop-time stalls.
    if (wifiStopPhase != WifiStopPhase::IDLE) {
        processStopSetupModePhase();
        return;
    }

    // Runtime SRAM guard with persistence + mode-aware thresholds:
    // AP+STA needs more memory than AP-only, and short dips should not force shutdown.
    const wifi_mode_t mode = WiFi.getMode();
    const bool staRadioOn = (mode == WIFI_AP_STA || mode == WIFI_STA);
    const bool dualRadioMode = isSetupModeActive() && staRadioOn;
    const bool staOnlyMode = staRadioOn && !dualRadioMode;
    const char* runtimeModeLabel = dualRadioMode ? "AP+STA" : (staRadioOn ? "STA" : "AP");
    uint32_t criticalFree = 0;
    uint32_t criticalBlock = 0;
    getWifiRuntimeThresholds(dualRadioMode, staOnlyMode, criticalFree, criticalBlock);

    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool freeLow = (freeInternal < criticalFree);
    if (freeLow && dualRadioMode) {
        const uint32_t freeDeficit = criticalFree - freeInternal;
        if (freeDeficit <= WIFI_RUNTIME_AP_STA_FREE_JITTER_TOLERANCE) {
            freeLow = false;
        }
    }
    bool blockLow = (largestInternal < criticalBlock);
    if (blockLow && staOnlyMode) {
        const uint32_t blockDeficit = criticalBlock - largestInternal;
        if (blockDeficit <= WIFI_RUNTIME_STA_BLOCK_JITTER_TOLERANCE) {
            blockLow = false;
        }
    }
    const bool lowHeap = freeLow || blockLow;

    if (lowHeap) {
        const unsigned long now = millis();
        if (lowDmaSinceMs == 0) {
            lowDmaSinceMs = now;
            Serial.printf("[WiFi] WARN: Internal SRAM low (mode=%s free=%lu block=%lu need>=%lu/%lu) - grace %lu ms\n",
                          runtimeModeLabel,
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

            // In AP+STA mode, drop AP first to preserve STA utility under pressure.
            if (dualRadioMode) {
                Serial.println("[WiFi] ACTION: dropping AP due to sustained low SRAM (keeping STA online)");
                if (!WiFi.enableAP(false)) {
                    Serial.println("[WiFi] WARN: enableAP(false) failed during low-SRAM AP drop; falling back to softAPdisconnect");
                    WiFi.softAPdisconnect(true);
                }
                apInterfaceEnabled = false;

                const wl_status_t staStatus = WiFi.status();
                if (staStatus == WL_CONNECTED) {
                    wifiClientState = WIFI_CLIENT_CONNECTED;
                    wifiReconnectFailures = 0;
                } else if (wifiClientState == WIFI_CLIENT_CONNECTING) {
                    // Keep connect workflow active and let status polling settle.
                    wifiClientState = WIFI_CLIENT_CONNECTING;
                } else {
                    wifiClientState = WIFI_CLIENT_DISCONNECTED;
                }

                // Cancel staged mode-switch workflow; reconnect logic can resume normally.
                wifiConnectPhase = WifiConnectPhase::IDLE;
                wifiConnectPhaseStartMs = 0;
                lowDmaCooldownUntilMs = now + WIFI_LOW_DMA_RETRY_COOLDOWN_MS;
                lowDmaSinceMs = 0;
                debugLogger.notifyWifiTransition(true);
                Serial.printf("[WiFi] AP dropped; STA status=%s\n", wifiClientStateApiName(wifiClientState));
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
    
    const unsigned long now = millis();
    int apClientCount = 0;
    const bool apInterfaceActive = isSetupModeActive();
    if (apInterfaceActive) {
        if (lastApStaCountPollMs == 0 ||
            (now - lastApStaCountPollMs) >= AP_STA_COUNT_POLL_MS) {
            cachedApStaCount = WiFi.softAPgetStationNum();
            lastApStaCountPollMs = now;
        }
        apClientCount = cachedApStaCount;
    } else {
        cachedApStaCount = 0;
    }

    const bool staConnectedNow =
        (wifiClientState == WIFI_CLIENT_CONNECTED) || (WiFi.status() == WL_CONNECTED);
    if (apInterfaceActive && apClientCount > 0) {
        lastClientSeenMs = now;
    }

    if (apInterfaceActive &&
        staConnectedNow &&
        apClientCount == 0 &&
        lastClientSeenMs != 0 &&
        (now - lastClientSeenMs) >= WIFI_AP_IDLE_DROP_AFTER_STA_MS) {
        Serial.printf("[WiFi] STA connected and AP idle for %lu ms - dropping AP\n",
                      static_cast<unsigned long>(WIFI_AP_IDLE_DROP_AFTER_STA_MS));
        const bool staWasConnected = (WiFi.status() == WL_CONNECTED);
        if (!WiFi.enableAP(false)) {
            Serial.println("[WiFi] WARN: enableAP(false) failed during idle AP retire; falling back to softAPdisconnect");
            WiFi.softAPdisconnect(true);
        }
        apInterfaceEnabled = false;
        cachedApStaCount = 0;
        lastApStaCountPollMs = 0;
        if (staWasConnected && WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] WARN: STA dropped during AP retire; reconnecting");
            wifiClientState = WIFI_CLIENT_DISCONNECTED;
            const V1Settings& settings = settingsManager.get();
            if (settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0) {
                String savedPassword = settingsManager.getWifiClientPassword();
                connectToNetwork(settings.wifiClientSSID, savedPassword, false);
            }
        }
        return;
    }

    if (staConnectedNow || apClientCount > 0) {
        lastAnyClientSeenMs = now;
    } else if (lastAnyClientSeenMs != 0) {
        const unsigned long noClientLimit = wasAutoStarted
            ? WIFI_NO_CLIENT_SHUTDOWN_AUTO_MS
            : WIFI_NO_CLIENT_SHUTDOWN_MS;
        if ((now - lastAnyClientSeenMs) >= noClientLimit) {
            Serial.printf("[WiFi] No AP/STA clients for %lu ms (%s) - stopping WiFi\n",
                          static_cast<unsigned long>(noClientLimit),
                          wasAutoStarted ? "auto-start" : "manual");
            stopSetupMode(false, wasAutoStarted ? "no_clients_auto" : "no_clients");
            return;
        }
    }

    // Continue serving HTTP while STA remains online even after AP is retired.
    if (isWifiServiceActive()) {
        server.handleClient();
    }

    if (lastMaintenanceFastMs == 0 ||
        (now - lastMaintenanceFastMs) >= WIFI_MAINTENANCE_FAST_MS) {
        processWifiClientConnectPhase();
        processPendingPushNow();
        lastMaintenanceFastMs = now;
    }
    if (lastTimeoutCheckMs == 0 ||
        (now - lastTimeoutCheckMs) >= WIFI_TIMEOUT_CHECK_MS) {
        checkAutoTimeout();
        lastTimeoutCheckMs = now;
    }

    // Check WiFi client (STA) status at a moderate cadence to avoid tight-loop
    // status polling jitter while preserving reconnect responsiveness.
    if (lastStatusCheckMs == 0 ||
        (now - lastStatusCheckMs) >= WIFI_STATUS_CHECK_MS) {
        checkWifiClientStatus();
        lastStatusCheckMs = now;
    }
}

// --- getAPIPAddress, getIPAddress, getConnectedSSID, startWifiScan,
//     getScannedNetworks, connectToNetwork, disconnectFromNetwork,
//     processWifiClientConnectPhase, processPendingPushNow,
//     checkWifiClientStatus moved to wifi_client.cpp ---

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

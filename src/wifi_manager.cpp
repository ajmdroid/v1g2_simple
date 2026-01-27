/**
 * WiFi Manager for V1 Gen2 Display
 * AP+STA: always-on access point serving the local UI/API
 *         plus optional station mode to connect to external network
 */

#include "wifi_manager.h"
#include "settings.h"
#include "display.h"
#include "storage_manager.h"
#include "debug_logger.h"
#include "v1_profiles.h"
#include "ble_client.h"
#include "obd_handler.h"
#include "gps_handler.h"
#include "camera_manager.h"
#include "perf_metrics.h"
#include "event_ring.h"
#include "audio_beep.h"
#include "battery_manager.h"
#include "../include/config.h"
#include "../include/color_themes.h"
#include <HTTPClient.h>
#include <algorithm>
#include <map>
#include <vector>
#include <ArduinoJson.h>
#include <LittleFS.h>

// External BLE client for V1 commands
extern V1BLEClient bleClient;
// External GPS handler for runtime enable/disable
extern GPSHandler gpsHandler;
// Camera load trigger flags (set when GPS enabled at runtime)
extern bool cameraLoadPending;
extern bool cameraLoadComplete;
// Preview hold helper to keep color demo visible briefly
extern void requestColorPreviewHold(uint32_t durationMs);
extern bool isColorPreviewRunning();
extern void cancelColorPreview();

// Enable to dump LittleFS root on WiFi start (debug only); keep false for release
static constexpr bool WIFI_DEBUG_FS_DUMP = false;

// Optional AP auto-timeout (milliseconds). Set to 0 to keep always-on behavior.
static constexpr unsigned long WIFI_AP_AUTO_TIMEOUT_MS = 0;            // e.g., 10 * 60 * 1000 for 10 minutes
static constexpr unsigned long WIFI_AP_INACTIVITY_GRACE_MS = 60 * 1000; // Require no UI activity/clients for this long before stopping

static void applyDebugLogFilterFromSettings() {
    DebugLogConfig cfg = settingsManager.getDebugLogConfig();
    DebugLogFilter filter{cfg.alerts, cfg.wifi, cfg.ble, cfg.gps, cfg.obd, cfg.system, cfg.display, cfg.perfMetrics};
    debugLogger.setFilter(filter);
}

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

// Helper to serve files from LittleFS (with gzip support)
bool serveLittleFSFileHelper(WebServer& server, const char* path, const char* contentType) {
    // Try compressed version first (only if client accepts gzip)
    String acceptEncoding = server.header("Accept-Encoding");
    bool clientAcceptsGzip = acceptEncoding.indexOf("gzip") >= 0;
    
    if (clientAcceptsGzip) {
        String gzPath = String(path) + ".gz";
        if (LittleFS.exists(gzPath.c_str())) {
            File file = LittleFS.open(gzPath.c_str(), "r");
            if (file) {
                size_t fileSize = file.size();
                server.setContentLength(fileSize);
                server.sendHeader("Content-Encoding", "gzip");
                server.sendHeader("Cache-Control", "max-age=86400");
                server.send(200, contentType, "");
                Serial.printf("[HTTP] 200 %s -> %s.gz (%u bytes)\n", path, path, fileSize);
                
                // Stream file content
                uint8_t buf[1024];
                while (file.available()) {
                    size_t len = file.read(buf, sizeof(buf));
                    server.client().write(buf, len);
                }
                file.close();
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
    server.sendHeader("Cache-Control", "max-age=86400");
    server.streamFile(file, contentType);
    Serial.printf("[HTTP] 200 %s (%u bytes)\n", path, fileSize);
    file.close();
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

// Ensure last client seen timestamp advances when UI is accessed
// (called on every HTTP request via checkRateLimit/markUiActivity)

bool WiFiManager::startSetupMode() {
    // Always-on AP; idempotent start
    if (setupModeState == SETUP_MODE_AP_ON) {
        Serial.println("[SetupMode] Already active");
        return true;
    }

    Serial.println("[SetupMode] Starting AP (always-on mode)...");
    setupModeStartTime = millis();
    lastClientSeenMs = setupModeStartTime;

    // Check if WiFi client is enabled - use AP+STA mode
    const V1Settings& settings = settingsManager.get();
    if (settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0) {
        Serial.println("[SetupMode] WiFi client enabled, using AP+STA mode");
        WiFi.mode(WIFI_AP_STA);
        wifiClientState = WIFI_CLIENT_DISCONNECTED;
    } else {
        WiFi.mode(WIFI_AP);
        wifiClientState = WIFI_CLIENT_DISABLED;
    }

    setupAP();
    setupWebServer();

    // Collect Accept-Encoding header for GZIP support
    const char* headerKeys[] = {"Accept-Encoding"};
    server.collectHeaders(headerKeys, 1);

    server.begin();
    setupModeState = SETUP_MODE_AP_ON;

    EVENT_LOG(EVT_WIFI_AP_START, 0);
    EVENT_LOG(EVT_SETUP_MODE_ENTER, 0);

    Serial.printf("[SetupMode] AP started - connect to SSID shown on display\n");
    Serial.printf("[SetupMode] Web UI at http://%s\n", WiFi.softAPIP().toString().c_str());
    if (WIFI_AP_AUTO_TIMEOUT_MS == 0) {
        Serial.println("[SetupMode] AP will remain on (no timeout)");
    } else {
        Serial.printf("[SetupMode] AP auto-timeout set to %lu ms\n", WIFI_AP_AUTO_TIMEOUT_MS);
    }

    if (debugLogger.isEnabled()) {
        debugLogger.log(DebugLogCategory::Wifi, "Setup mode AP started");
    }

    return true;
}

bool WiFiManager::stopSetupMode(bool manual) {
    if (setupModeState != SETUP_MODE_AP_ON) {
        return false;
    }

    Serial.println("[SetupMode] Stopping AP...");
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    setupModeState = SETUP_MODE_OFF;

    if (debugLogger.isEnabled()) {
        debugLogger.log(DebugLogCategory::Wifi, manual ? "Setup mode AP stopped (manual)" : "Setup mode AP stopped (timeout)");
    }

    EVENT_LOG(EVT_WIFI_AP_STOP, 0);
    EVENT_LOG(EVT_SETUP_MODE_EXIT, manual ? 1 : 0);
    return true;
}

bool WiFiManager::toggleSetupMode(bool manual) {
    if (setupModeState == SETUP_MODE_AP_ON) {
        return stopSetupMode(manual);
    }
    return startSetupMode();
}

void WiFiManager::setupAP() {
    // Use saved SSID/password when available; fall back to defaults if missing/too short
    const V1Settings& settings = settingsManager.get();
    String apSSID = settings.apSSID.length() ? settings.apSSID : "V1-Simple";
    String apPass = (settings.apPassword.length() >= 8) ? settings.apPassword : "setupv1g2";  // WPA2 requires 8+
    
    Serial.printf("[SetupMode] Starting AP: %s (pass: ****)\n", apSSID.c_str());
    
    // Configure AP IP
    IPAddress apIP(192, 168, 35, 5);
    IPAddress gateway(192, 168, 35, 5);
    IPAddress subnet(255, 255, 255, 0);
    
    if (!WiFi.softAPConfig(apIP, gateway, subnet)) {
        // NOTE: Intentional fallthrough - softAP will still work with default IP (192.168.4.1)
        // Device remains functional. Reviewed January 20, 2026.
        Serial.println("[SetupMode] softAPConfig failed! Will use default IP 192.168.4.1");
    }
    
    if (!WiFi.softAP(apSSID.c_str(), apPass.c_str())) {
        Serial.println("[SetupMode] softAP failed!");
        return;
    }
    
    Serial.printf("[SetupMode] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
}

void WiFiManager::setupWebServer() {
    // Initialize LittleFS for serving web UI files
    if (!LittleFS.begin(false)) {
        Serial.println("[SetupMode] ERROR: LittleFS mount failed (not formatting automatically)");
        return;
    }
    Serial.println("[SetupMode] LittleFS mounted");
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
    
    // Debug API routes (performance metrics and event ring)
    server.on("/api/debug/metrics", HTTP_GET, [this]() { handleDebugMetrics(); });
    server.on("/api/debug/events", HTTP_GET, [this]() { handleDebugEvents(); });
    server.on("/api/debug/events/clear", HTTP_POST, [this]() { handleDebugEventsClear(); });
    server.on("/api/debug/enable", HTTP_POST, [this]() { handleDebugEnable(); });
    server.on("/api/debug/logs", HTTP_GET, [this]() { handleDebugLogsMeta(); });
    server.on("/api/debug/logs/download", HTTP_GET, [this]() { handleDebugLogsDownload(); });
    server.on("/api/debug/logs/tail", HTTP_GET, [this]() { handleDebugLogsTail(); });
    server.on("/api/debug/logs/clear", HTTP_POST, [this]() { handleDebugLogsClear(); });
    
    // OBD-II API routes
    server.on("/api/obd/status", HTTP_GET, [this]() { handleObdStatus(); });
    server.on("/api/obd/scan", HTTP_POST, [this]() { handleObdScan(); });
    server.on("/api/obd/scan/stop", HTTP_POST, [this]() { handleObdScanStop(); });
    server.on("/api/obd/devices", HTTP_GET, [this]() { handleObdDevices(); });
    server.on("/api/obd/devices/clear", HTTP_POST, [this]() { handleObdDevicesClear(); });
    server.on("/api/obd/connect", HTTP_POST, [this]() { handleObdConnect(); });
    server.on("/api/obd/forget", HTTP_POST, [this]() { handleObdForget(); });
    
    // GPS API routes
    server.on("/api/gps/status", HTTP_GET, [this]() { handleGpsStatus(); });
    server.on("/api/gps/reset", HTTP_POST, [this]() { handleGpsReset(); });
    
    // Camera alerts API routes
    server.on("/api/cameras/status", HTTP_GET, [this]() { handleCameraStatus(); });
    server.on("/api/cameras/reload", HTTP_POST, [this]() { handleCameraReload(); });
    server.on("/api/cameras/upload", HTTP_POST, [this]() { handleCameraUpload(); });
    server.on("/api/cameras/test", HTTP_POST, [this]() { handleCameraTest(); });
    server.on("/api/cameras/sync-osm", HTTP_POST, [this]() { handleCameraSyncOsm(); });
    
    // WiFi client (STA) API routes - connect to external network
    server.on("/api/wifi/status", HTTP_GET, [this]() { handleWifiClientStatus(); });
    server.on("/api/wifi/scan", HTTP_POST, [this]() { handleWifiClientScan(); });
    server.on("/api/wifi/connect", HTTP_POST, [this]() { handleWifiClientConnect(); });
    server.on("/api/wifi/disconnect", HTTP_POST, [this]() { handleWifiClientDisconnect(); });
    server.on("/api/wifi/forget", HTTP_POST, [this]() { handleWifiClientForget(); });
    
    // Note: onNotFound is set earlier to handle LittleFS static files
}

void WiFiManager::checkAutoTimeout() {
    if (WIFI_AP_AUTO_TIMEOUT_MS == 0) return;  // Disabled by default
    if (setupModeState != SETUP_MODE_AP_ON) return;

    unsigned long now = millis();
    int staCount = WiFi.softAPgetStationNum();
    if (staCount > 0) {
        lastClientSeenMs = now;
    }

    unsigned long lastActivity = lastUiActivityMs;
    if (lastClientSeenMs > lastActivity) {
        lastActivity = lastClientSeenMs;
    }

    bool timeoutElapsed = (now - setupModeStartTime) >= WIFI_AP_AUTO_TIMEOUT_MS;
    bool inactiveEnough = (lastActivity == 0) ? ((now - setupModeStartTime) >= WIFI_AP_INACTIVITY_GRACE_MS)
                                              : ((now - lastActivity) >= WIFI_AP_INACTIVITY_GRACE_MS);

    if (timeoutElapsed && inactiveEnough && staCount == 0) {
        Serial.println("[SetupMode] Auto-timeout reached - stopping AP");
        stopSetupMode(false);
    }
}

void WiFiManager::process() {
    if (setupModeState != SETUP_MODE_AP_ON) {
        return;  // No WiFi processing when Setup Mode is off
    }
    
    // Handle web requests
    if (setupModeState != SETUP_MODE_AP_ON) {
        return;
    }

    server.handleClient();
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
    
    // Make sure we're in AP+STA mode
    if (WiFi.getMode() != WIFI_AP_STA) {
        Serial.println("[WiFiClient] Switching to AP+STA mode");
        WiFi.mode(WIFI_AP_STA);
        delay(100);  // Brief delay for mode switch
    }
    
    Serial.printf("[WiFiClient] Connecting to: %s\n", ssid.c_str());
    
    pendingConnectSSID = ssid;
    pendingConnectPassword = password;
    wifiConnectStartMs = millis();
    wifiClientState = WIFI_CLIENT_CONNECTING;
    
    WiFi.begin(ssid.c_str(), password.c_str());
    
    return true;
}

void WiFiManager::disconnectFromNetwork() {
    Serial.println("[WiFiClient] Disconnecting from network");
    WiFi.disconnect(false);  // Don't turn off station mode
    wifiClientState = WIFI_CLIENT_DISCONNECTED;
    pendingConnectSSID = "";
    pendingConnectPassword = "";
}

void WiFiManager::checkWifiClientStatus() {
    // Skip if WiFi client is disabled
    if (wifiClientState == WIFI_CLIENT_DISABLED) {
        return;
    }
    
    wl_status_t status = WiFi.status();
    
    switch (wifiClientState) {
        case WIFI_CLIENT_CONNECTING: {
            if (status == WL_CONNECTED) {
                wifiClientState = WIFI_CLIENT_CONNECTED;
                Serial.printf("[WiFiClient] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
                
                // Save credentials on successful connection
                if (pendingConnectSSID.length() > 0) {
                    settingsManager.setWifiClientCredentials(pendingConnectSSID, pendingConnectPassword);
                    pendingConnectSSID = "";
                    pendingConnectPassword = "";
                }
            } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
                wifiClientState = WIFI_CLIENT_FAILED;
                Serial.printf("[WiFiClient] Connection failed: %d\n", status);
                pendingConnectSSID = "";
                pendingConnectPassword = "";
            } else if (millis() - wifiConnectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
                wifiClientState = WIFI_CLIENT_FAILED;
                Serial.println("[WiFiClient] Connection timeout");
                WiFi.disconnect(false);
                pendingConnectSSID = "";
                pendingConnectPassword = "";
            }
            break;
        }
        
        case WIFI_CLIENT_CONNECTED: {
            if (status != WL_CONNECTED) {
                wifiClientState = WIFI_CLIENT_DISCONNECTED;
                Serial.println("[WiFiClient] Lost connection");
            }
            break;
        }
        
        case WIFI_CLIENT_DISCONNECTED:
        case WIFI_CLIENT_FAILED: {
            // Auto-reconnect if we have saved credentials
            const V1Settings& settings = settingsManager.get();
            if (settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0) {
                String savedPassword = settingsManager.getWifiClientPassword();
                if (savedPassword.length() > 0 || status == WL_NO_SSID_AVAIL) {
                    // Only try auto-reconnect every 30 seconds
                    static unsigned long lastReconnectAttempt = 0;
                    if (millis() - lastReconnectAttempt > 30000) {
                        lastReconnectAttempt = millis();
                        Serial.println("[WiFiClient] Auto-reconnect attempt...");
                        connectToNetwork(settings.wifiClientSSID, savedPassword);
                    }
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
        wifi["sta_connected"] = false;  // AP-only
        wifi["sta_ip"] = "";
        wifi["ap_ip"] = getAPIPAddress();
        wifi["ssid"] = settings.apSSID;
        wifi["rssi"] = 0;
        
        // Device info
        JsonObject device = doc["device"].to<JsonObject>();
        device["uptime"] = millis() / 1000;
        device["heap_free"] = ESP.getFreeHeap();
        device["hostname"] = "v1g2";
        device["firmware_version"] = FIRMWARE_VERSION;
        
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

void WiFiManager::handleSettingsApi() {
    const V1Settings& settings = settingsManager.get();
    
    JsonDocument doc;
    doc["ap_ssid"] = settings.apSSID;
    doc["ap_password"] = "********";  // Don't send actual password
    doc["isDefaultPassword"] = (settings.apPassword == "setupv1g2");  // Security warning flag
    doc["proxy_ble"] = settings.proxyBLE;
    doc["proxy_name"] = settings.proxyName;
    doc["displayStyle"] = static_cast<int>(settings.displayStyle);
    doc["autoPowerOffMinutes"] = settings.autoPowerOffMinutes;
    doc["gpsEnabled"] = settings.gpsEnabled;
    doc["obdEnabled"] = settings.obdEnabled;
    
    // Auto-lockout settings (JBV1-style)
    doc["lockoutEnabled"] = settings.lockoutEnabled;
    doc["lockoutKaProtection"] = settings.lockoutKaProtection;
    doc["lockoutDirectionalUnlearn"] = settings.lockoutDirectionalUnlearn;
    doc["lockoutFreqToleranceMHz"] = settings.lockoutFreqToleranceMHz;
    doc["lockoutLearnCount"] = settings.lockoutLearnCount;
    doc["lockoutUnlearnCount"] = settings.lockoutUnlearnCount;
    doc["lockoutManualDeleteCount"] = settings.lockoutManualDeleteCount;
    doc["lockoutLearnIntervalHours"] = settings.lockoutLearnIntervalHours;
    doc["lockoutUnlearnIntervalHours"] = settings.lockoutUnlearnIntervalHours;
    doc["lockoutMaxSignalStrength"] = settings.lockoutMaxSignalStrength;
    doc["lockoutMaxDistanceM"] = settings.lockoutMaxDistanceM;
    
    // Camera alert settings
    doc["cameraAlertsEnabled"] = settings.cameraAlertsEnabled;
    doc["cameraAudioEnabled"] = settings.cameraAudioEnabled;
    doc["cameraAlertDistanceM"] = settings.cameraAlertDistanceM;
    
    // Development/Debug settings
    doc["enableWifiAtBoot"] = settings.enableWifiAtBoot;
    doc["enableDebugLogging"] = settings.enableDebugLogging;
    doc["logAlerts"] = settings.logAlerts;
    doc["logWifi"] = settings.logWifi;
    doc["logBle"] = settings.logBle;
    doc["logGps"] = settings.logGps;
    doc["logObd"] = settings.logObd;
    doc["logSystem"] = settings.logSystem;
    doc["logDisplay"] = settings.logDisplay;
    doc["kittScannerEnabled"] = settings.kittScannerEnabled;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleSettingsSave() {
    if (!checkRateLimit()) return;
    
    Serial.println("=== handleSettingsSave() called ===");
    const V1Settings& currentSettings = settingsManager.get();

    if (server.hasArg("ap_ssid")) {
        String apSsid = server.arg("ap_ssid");
        String apPass = server.arg("ap_password");
        
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
        settingsManager.setProxyBLE(proxyEnabled);
    }
    if (server.hasArg("proxy_name")) {
        String proxyName = server.arg("proxy_name");
        if (proxyName.length() > 32) {
            proxyName = proxyName.substring(0, 32);  // Truncate to max 32 chars
        }
        settingsManager.setProxyName(proxyName);
    }
    if (server.hasArg("autoPowerOffMinutes")) {
        int minutes = server.arg("autoPowerOffMinutes").toInt();
        minutes = std::max(0, std::min(minutes, 60));  // Clamp 0-60 minutes
        settingsManager.setAutoPowerOffMinutes(minutes);
    }

    // Display style setting
    if (server.hasArg("displayStyle")) {
        int style = server.arg("displayStyle").toInt();
        style = std::max(0, std::min(style, 3));  // Clamp to valid range (0=Classic, 1=Modern, 2=Hemi, 3=Serpentine)
        settingsManager.updateDisplayStyle(static_cast<DisplayStyle>(style));
        display.forceNextRedraw();  // Force display update to show new font style
    }

    // GPS/OBD module settings
    if (server.hasArg("gpsEnabled")) {
        bool enabled = server.arg("gpsEnabled") == "true" || server.arg("gpsEnabled") == "1";
        bool wasEnabled = settingsManager.isGpsEnabled();
        settingsManager.setGpsEnabled(enabled);
        
        // Actually start/stop GPS hardware at runtime
        if (enabled && !wasEnabled) {
            Serial.println("[WiFi] GPS enabled - starting GPS handler");
            gpsHandler.begin();
            // Trigger camera database loading if SD card available
            if (storageManager.isSDCard() && !cameraLoadComplete) {
                cameraLoadPending = true;
                Serial.println("[WiFi] Camera database will load after V1 connects");
            }
        } else if (!enabled && wasEnabled) {
            Serial.println("[WiFi] GPS disabled - stopping GPS handler");
            gpsHandler.end();
        }
    }
    if (server.hasArg("obdEnabled")) {
        bool enabled = server.arg("obdEnabled") == "true" || server.arg("obdEnabled") == "1";
        bool wasEnabled = settingsManager.isObdEnabled();
        settingsManager.setObdEnabled(enabled);
        
        // Actually start/stop OBD hardware at runtime
        if (enabled && !wasEnabled) {
            Serial.println("[WiFi] OBD enabled - starting OBD handler");
            obdHandler.begin();
        } else if (!enabled && wasEnabled) {
            Serial.println("[WiFi] OBD disabled - disconnecting OBD");
            obdHandler.disconnect();
        }
    }
    if (server.hasArg("obdPin")) {
        settingsManager.setObdPin(server.arg("obdPin"));
    }
    
    // Auto-lockout settings (JBV1-style)
    if (server.hasArg("lockoutEnabled")) {
        bool enabled = server.arg("lockoutEnabled") == "true" || server.arg("lockoutEnabled") == "1";
        settingsManager.updateLockoutEnabled(enabled);
    }
    if (server.hasArg("lockoutKaProtection")) {
        bool enabled = server.arg("lockoutKaProtection") == "true" || server.arg("lockoutKaProtection") == "1";
        settingsManager.updateLockoutKaProtection(enabled);
    }
    if (server.hasArg("lockoutDirectionalUnlearn")) {
        bool enabled = server.arg("lockoutDirectionalUnlearn") == "true" || server.arg("lockoutDirectionalUnlearn") == "1";
        settingsManager.updateLockoutDirectionalUnlearn(enabled);
    }
    if (server.hasArg("lockoutFreqToleranceMHz")) {
        int mhz = server.arg("lockoutFreqToleranceMHz").toInt();
        mhz = std::max(1, std::min(mhz, 50));  // Clamp 1-50 MHz
        settingsManager.updateLockoutFreqToleranceMHz(mhz);
    }
    if (server.hasArg("lockoutLearnCount")) {
        int count = server.arg("lockoutLearnCount").toInt();
        count = std::max(1, std::min(count, 10));
        settingsManager.updateLockoutLearnCount(count);
    }
    if (server.hasArg("lockoutUnlearnCount")) {
        int count = server.arg("lockoutUnlearnCount").toInt();
        count = std::max(1, std::min(count, 50));
        settingsManager.updateLockoutUnlearnCount(count);
    }
    if (server.hasArg("lockoutManualDeleteCount")) {
        int count = server.arg("lockoutManualDeleteCount").toInt();
        count = std::max(1, std::min(count, 100));
        settingsManager.updateLockoutManualDeleteCount(count);
    }
    if (server.hasArg("lockoutLearnIntervalHours")) {
        int hours = server.arg("lockoutLearnIntervalHours").toInt();
        hours = std::max(0, std::min(hours, 24));
        settingsManager.updateLockoutLearnIntervalHours(hours);
    }
    if (server.hasArg("lockoutUnlearnIntervalHours")) {
        int hours = server.arg("lockoutUnlearnIntervalHours").toInt();
        hours = std::max(0, std::min(hours, 24));
        settingsManager.updateLockoutUnlearnIntervalHours(hours);
    }
    if (server.hasArg("lockoutMaxSignalStrength")) {
        int strength = server.arg("lockoutMaxSignalStrength").toInt();
        strength = std::max(0, std::min(strength, 9));  // 0=disabled, 1-9=threshold
        settingsManager.updateLockoutMaxSignalStrength(strength);
    }
    if (server.hasArg("lockoutMaxDistanceM")) {
        int meters = server.arg("lockoutMaxDistanceM").toInt();
        meters = std::max(100, std::min(meters, 2000));  // Clamp 100-2000m
        settingsManager.updateLockoutMaxDistanceM(meters);
    }
    
    // Camera alert settings
    if (server.hasArg("cameraAlertsEnabled")) {
        bool enabled = server.arg("cameraAlertsEnabled") == "true" || server.arg("cameraAlertsEnabled") == "1";
        settingsManager.updateCameraAlertsEnabled(enabled);
    }
    if (server.hasArg("cameraAudioEnabled")) {
        bool enabled = server.arg("cameraAudioEnabled") == "true" || server.arg("cameraAudioEnabled") == "1";
        settingsManager.updateCameraAudioEnabled(enabled);
    }
    if (server.hasArg("cameraAlertDistanceM")) {
        int meters = server.arg("cameraAlertDistanceM").toInt();
        meters = std::max(100, std::min(meters, 2000));  // Clamp 100-2000m
        settingsManager.updateCameraAlertDistanceM(meters);
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
    
    settingsManager.setSlot(slot, profile, static_cast<V1Mode>(mode));
    
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
    
    // Check if profile/mode are passed directly (from Push Now button)
    String profileName;
    V1Mode mode = V1_MODE_UNKNOWN;
    
    if (server.hasArg("profile") && server.arg("profile").length() > 0) {
        // Use the form values directly
        profileName = server.arg("profile");
        if (server.hasArg("mode")) {
            mode = static_cast<V1Mode>(server.arg("mode").toInt());
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
        
        profileName = pushSlot.profileName;
        mode = pushSlot.mode;
    }
    
    if (profileName.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"No profile configured for this slot\"}");
        return;
    }
    
    // Load and push the profile
    V1Profile profile;
    if (!v1ProfileManager.loadProfile(profileName, profile)) {
        server.send(500, "application/json", "{\"error\":\"Failed to load profile\"}");
        return;
    }
    
    if (!bleClient.writeUserBytes(profile.settings.bytes)) {
        server.send(500, "application/json", "{\"error\":\"Failed to push settings\"}");
        return;
    }
    
    // Use slot's dark mode setting, not the profile's stored displayOn value
    // (slot dark mode is the user-facing toggle in auto-push config)
    bool slotDarkMode = settingsManager.getSlotDarkMode(slot);
    bleClient.setDisplayOn(!slotDarkMode);  // Dark mode = display off
    
    if (mode != V1_MODE_UNKNOWN) {
        bleClient.setMode(static_cast<uint8_t>(mode));
    }
    
    // Set volumes if configured (not 0xFF = no change)
    uint8_t mainVol = settingsManager.getSlotVolume(slot);
    uint8_t muteVol = settingsManager.getSlotMuteVolume(slot);
    
    Serial.printf("[PushNow] Slot %d volumes - main: %d, mute: %d\n", slot, mainVol, muteVol);
    
    // Only set volume if BOTH are configured (both != 0xFF means both 0-9)
    if (mainVol != 0xFF && muteVol != 0xFF) {
        delay(100);
        Serial.printf("[PushNow] Setting volume - main: %d, muted: %d\n", mainVol, muteVol);
        bleClient.setVolume(mainVol, muteVol);
    } else {
        Serial.printf("[PushNow] Volume: skipping (need both 0-9, got main=%d mute=%d)\n", 
                        mainVol, muteVol);
    }
    
    // Update active slot and refresh display profile indicator
    settingsManager.setActiveSlot(slot);
    display.drawProfileIndicator(slot);
    
    server.send(200, "application/json", "{\"success\":true}");
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
    
    uint16_t bogey = server.hasArg("bogey") ? server.arg("bogey").toInt() : 0xF800;
    uint16_t freq = server.hasArg("freq") ? server.arg("freq").toInt() : 0xF800;
    uint16_t arrowFront = server.hasArg("arrowFront") ? server.arg("arrowFront").toInt() : 0xF800;
    uint16_t arrowSide = server.hasArg("arrowSide") ? server.arg("arrowSide").toInt() : 0xF800;
    uint16_t arrowRear = server.hasArg("arrowRear") ? server.arg("arrowRear").toInt() : 0xF800;
    uint16_t bandL = server.hasArg("bandL") ? server.arg("bandL").toInt() : 0x001F;
    uint16_t bandKa = server.hasArg("bandKa") ? server.arg("bandKa").toInt() : 0xF800;
    uint16_t bandK = server.hasArg("bandK") ? server.arg("bandK").toInt() : 0x001F;
    uint16_t bandX = server.hasArg("bandX") ? server.arg("bandX").toInt() : 0x07E0;
    
    Serial.printf("[HTTP] Saving colors: bogey=%d freq=%d arrowF=%d arrowS=%d arrowR=%d\n", bogey, freq, arrowFront, arrowSide, arrowRear);
    
    settingsManager.setDisplayColors(bogey, freq, arrowFront, arrowSide, arrowRear, bandL, bandKa, bandK, bandX);
    
    // Handle WiFi icon colors if provided
    if (server.hasArg("wifiIcon") || server.hasArg("wifiConnected")) {
        uint16_t wifiIcon = server.hasArg("wifiIcon") ? server.arg("wifiIcon").toInt() : 0x07FF;
        uint16_t wifiConn = server.hasArg("wifiConnected") ? server.arg("wifiConnected").toInt() : 0x07E0;
        settingsManager.setWiFiIconColors(wifiIcon, wifiConn);
    }
    
    // Handle BLE icon colors if provided
    if (server.hasArg("bleConnected") || server.hasArg("bleDisconnected")) {
        uint16_t bleConn = server.hasArg("bleConnected") ? server.arg("bleConnected").toInt() : 0x07E0;
        uint16_t bleDisc = server.hasArg("bleDisconnected") ? server.arg("bleDisconnected").toInt() : 0x001F;
        settingsManager.setBleIconColors(bleConn, bleDisc);
    }
    
    // Handle signal bar colors if provided
    if (server.hasArg("bar1") || server.hasArg("bar2") || server.hasArg("bar3") ||
        server.hasArg("bar4") || server.hasArg("bar5") || server.hasArg("bar6")) {
        uint16_t bar1 = server.hasArg("bar1") ? server.arg("bar1").toInt() : 0x07E0;
        uint16_t bar2 = server.hasArg("bar2") ? server.arg("bar2").toInt() : 0x07E0;
        uint16_t bar3 = server.hasArg("bar3") ? server.arg("bar3").toInt() : 0xFFE0;
        uint16_t bar4 = server.hasArg("bar4") ? server.arg("bar4").toInt() : 0xFFE0;
        uint16_t bar5 = server.hasArg("bar5") ? server.arg("bar5").toInt() : 0xF800;
        uint16_t bar6 = server.hasArg("bar6") ? server.arg("bar6").toInt() : 0xF800;
        settingsManager.setSignalBarColors(bar1, bar2, bar3, bar4, bar5, bar6);
    }
    
    // Handle muted color if provided
    if (server.hasArg("muted")) {
        uint16_t mutedColor = server.arg("muted").toInt();
        settingsManager.setMutedColor(mutedColor);
    }
    
    // Handle photo radar band color if provided
    if (server.hasArg("bandPhoto")) {
        uint16_t bandPhotoColor = server.arg("bandPhoto").toInt();
        settingsManager.setBandPhotoColor(bandPhotoColor);
    }
    
    // Handle persisted color if provided
    if (server.hasArg("persisted")) {
        uint16_t persistedColor = server.arg("persisted").toInt();
        settingsManager.setPersistedColor(persistedColor);
    }
    
    // Handle volume indicator colors
    if (server.hasArg("volumeMain")) {
        uint16_t volMainColor = server.arg("volumeMain").toInt();
        settingsManager.setVolumeMainColor(volMainColor);
    }
    if (server.hasArg("volumeMute")) {
        uint16_t volMuteColor = server.arg("volumeMute").toInt();
        settingsManager.setVolumeMuteColor(volMuteColor);
    }
    
    // Handle RSSI label colors
    if (server.hasArg("rssiV1")) {
        uint16_t rssiV1Color = server.arg("rssiV1").toInt();
        settingsManager.setRssiV1Color(rssiV1Color);
    }
    if (server.hasArg("rssiProxy")) {
        uint16_t rssiProxyColor = server.arg("rssiProxy").toInt();
        settingsManager.setRssiProxyColor(rssiProxyColor);
    }
    
    // Handle status bar colors
    if (server.hasArg("statusGps")) {
        uint16_t statusGpsColor = server.arg("statusGps").toInt();
        settingsManager.setStatusGpsColor(statusGpsColor);
    }
    if (server.hasArg("statusGpsWarn")) {
        uint16_t statusGpsWarnColor = server.arg("statusGpsWarn").toInt();
        settingsManager.setStatusGpsWarnColor(statusGpsWarnColor);
    }
    if (server.hasArg("statusCam")) {
        uint16_t statusCamColor = server.arg("statusCam").toInt();
        settingsManager.setStatusCamColor(statusCamColor);
    }
    if (server.hasArg("statusObd")) {
        uint16_t statusObdColor = server.arg("statusObd").toInt();
        settingsManager.setStatusObdColor(statusObdColor);
    }
    
    // Handle frequency uses band color setting
    if (server.hasArg("freqUseBandColor")) {
        settingsManager.setFreqUseBandColor(server.arg("freqUseBandColor") == "true" || server.arg("freqUseBandColor") == "1");
    }
    
    // Handle display visibility settings
    if (server.hasArg("hideWifiIcon")) {
        settingsManager.setHideWifiIcon(server.arg("hideWifiIcon") == "true" || server.arg("hideWifiIcon") == "1");
    }
    if (server.hasArg("hideProfileIndicator")) {
        settingsManager.setHideProfileIndicator(server.arg("hideProfileIndicator") == "true" || server.arg("hideProfileIndicator") == "1");
    }
    if (server.hasArg("hideBatteryIcon")) {
        settingsManager.setHideBatteryIcon(server.arg("hideBatteryIcon") == "true" || server.arg("hideBatteryIcon") == "1");
    }
    if (server.hasArg("showBatteryPercent")) {
        settingsManager.setShowBatteryPercent(server.arg("showBatteryPercent") == "true" || server.arg("showBatteryPercent") == "1");
    }
    if (server.hasArg("hideBleIcon")) {
        settingsManager.setHideBleIcon(server.arg("hideBleIcon") == "true" || server.arg("hideBleIcon") == "1");
    }
    if (server.hasArg("hideVolumeIndicator")) {
        settingsManager.setHideVolumeIndicator(server.arg("hideVolumeIndicator") == "true" || server.arg("hideVolumeIndicator") == "1");
    }
    if (server.hasArg("hideRssiIndicator")) {
        settingsManager.setHideRssiIndicator(server.arg("hideRssiIndicator") == "true" || server.arg("hideRssiIndicator") == "1");
    }
    if (server.hasArg("kittScannerEnabled")) {
        settingsManager.setKittScannerEnabled(server.arg("kittScannerEnabled") == "true" || server.arg("kittScannerEnabled") == "1");
    }
    if (server.hasArg("enableWifiAtBoot")) {
        settingsManager.setEnableWifiAtBoot(server.arg("enableWifiAtBoot") == "true" || server.arg("enableWifiAtBoot") == "1");
    }
    if (server.hasArg("enableDebugLogging")) {
        settingsManager.setEnableDebugLogging(server.arg("enableDebugLogging") == "true" || server.arg("enableDebugLogging") == "1");
    }
    if (server.hasArg("logAlerts")) {
        settingsManager.setLogAlerts(server.arg("logAlerts") == "true" || server.arg("logAlerts") == "1");
    }
    if (server.hasArg("logWifi")) {
        settingsManager.setLogWifi(server.arg("logWifi") == "true" || server.arg("logWifi") == "1");
    }
    if (server.hasArg("logBle")) {
        settingsManager.setLogBle(server.arg("logBle") == "true" || server.arg("logBle") == "1");
    }
    if (server.hasArg("logGps")) {
        settingsManager.setLogGps(server.arg("logGps") == "true" || server.arg("logGps") == "1");
    }
    if (server.hasArg("logObd")) {
        settingsManager.setLogObd(server.arg("logObd") == "true" || server.arg("logObd") == "1");
    }
    if (server.hasArg("logSystem")) {
        settingsManager.setLogSystem(server.arg("logSystem") == "true" || server.arg("logSystem") == "1");
    }
    if (server.hasArg("logDisplay")) {
        settingsManager.setLogDisplay(server.arg("logDisplay") == "true" || server.arg("logDisplay") == "1");
    }
    if (server.hasArg("logPerfMetrics")) {
        settingsManager.setLogPerfMetrics(server.arg("logPerfMetrics") == "true" || server.arg("logPerfMetrics") == "1");
    }
    if (server.hasArg("logAudio")) {
        settingsManager.setLogAudio(server.arg("logAudio") == "true" || server.arg("logAudio") == "1");
    }
    if (server.hasArg("logCamera")) {
        settingsManager.setLogCamera(server.arg("logCamera") == "true" || server.arg("logCamera") == "1");
    }
    if (server.hasArg("logLockout")) {
        settingsManager.setLogLockout(server.arg("logLockout") == "true" || server.arg("logLockout") == "1");
    }
    if (server.hasArg("logTouch")) {
        settingsManager.setLogTouch(server.arg("logTouch") == "true" || server.arg("logTouch") == "1");
    }
    // Voice alert mode (dropdown: 0=disabled, 1=band, 2=freq, 3=band+freq)
    if (server.hasArg("voiceAlertMode")) {
        int mode = server.arg("voiceAlertMode").toInt();
        mode = std::max(0, std::min(mode, 3));
        settingsManager.setVoiceAlertMode((VoiceAlertMode)mode);
    }
    // Voice direction toggle (separate from mode)
    if (server.hasArg("voiceDirectionEnabled")) {
        settingsManager.setVoiceDirectionEnabled(server.arg("voiceDirectionEnabled") == "true" || server.arg("voiceDirectionEnabled") == "1");
    }
    if (server.hasArg("announceBogeyCount")) {
        settingsManager.setAnnounceBogeyCount(server.arg("announceBogeyCount") == "true" || server.arg("announceBogeyCount") == "1");
    }
    if (server.hasArg("muteVoiceIfVolZero")) {
        settingsManager.setMuteVoiceIfVolZero(server.arg("muteVoiceIfVolZero") == "true" || server.arg("muteVoiceIfVolZero") == "1");
    }
    // Secondary alert settings
    if (server.hasArg("announceSecondaryAlerts")) {
        settingsManager.setAnnounceSecondaryAlerts(server.arg("announceSecondaryAlerts") == "true" || server.arg("announceSecondaryAlerts") == "1");
    }
    if (server.hasArg("secondaryLaser")) {
        settingsManager.setSecondaryLaser(server.arg("secondaryLaser") == "true" || server.arg("secondaryLaser") == "1");
    }
    if (server.hasArg("secondaryKa")) {
        settingsManager.setSecondaryKa(server.arg("secondaryKa") == "true" || server.arg("secondaryKa") == "1");
    }
    if (server.hasArg("secondaryK")) {
        settingsManager.setSecondaryK(server.arg("secondaryK") == "true" || server.arg("secondaryK") == "1");
    }
    if (server.hasArg("secondaryX")) {
        settingsManager.setSecondaryX(server.arg("secondaryX") == "true" || server.arg("secondaryX") == "1");
    }
    // Volume fade settings
    if (server.hasArg("alertVolumeFadeEnabled") || server.hasArg("alertVolumeFadeDelaySec") || server.hasArg("alertVolumeFadeVolume")) {
        const V1Settings& current = settingsManager.get();
        bool enabled = current.alertVolumeFadeEnabled;
        uint8_t delaySec = current.alertVolumeFadeDelaySec;
        uint8_t volume = current.alertVolumeFadeVolume;
        if (server.hasArg("alertVolumeFadeEnabled")) {
            enabled = server.arg("alertVolumeFadeEnabled") == "true" || server.arg("alertVolumeFadeEnabled") == "1";
        }
        if (server.hasArg("alertVolumeFadeDelaySec")) {
            int val = server.arg("alertVolumeFadeDelaySec").toInt();
            delaySec = (uint8_t)std::max(1, std::min(val, 10));
        }
        if (server.hasArg("alertVolumeFadeVolume")) {
            int val = server.arg("alertVolumeFadeVolume").toInt();
            volume = (uint8_t)std::max(0, std::min(val, 9));
        }
        settingsManager.setAlertVolumeFade(enabled, delaySec, volume);
    }
    // Speed-based volume settings
    if (server.hasArg("speedVolumeEnabled") || server.hasArg("speedVolumeThresholdMph") || server.hasArg("speedVolumeBoost")) {
        const V1Settings& current = settingsManager.get();
        bool enabled = current.speedVolumeEnabled;
        uint8_t threshold = current.speedVolumeThresholdMph;
        uint8_t boost = current.speedVolumeBoost;
        if (server.hasArg("speedVolumeEnabled")) {
            enabled = server.arg("speedVolumeEnabled") == "true" || server.arg("speedVolumeEnabled") == "1";
        }
        if (server.hasArg("speedVolumeThresholdMph")) {
            int val = server.arg("speedVolumeThresholdMph").toInt();
            threshold = (uint8_t)std::max(10, std::min(val, 100));
        }
        if (server.hasArg("speedVolumeBoost")) {
            int val = server.arg("speedVolumeBoost").toInt();
            boost = (uint8_t)std::max(1, std::min(val, 5));
        }
        settingsManager.setSpeedVolume(enabled, threshold, boost);
    }
    // Low-speed mute settings
    if (server.hasArg("lowSpeedMuteEnabled") || server.hasArg("lowSpeedMuteThresholdMph")) {
        const V1Settings& current = settingsManager.get();
        bool enabled = current.lowSpeedMuteEnabled;
        uint8_t threshold = current.lowSpeedMuteThresholdMph;
        if (server.hasArg("lowSpeedMuteEnabled")) {
            enabled = server.arg("lowSpeedMuteEnabled") == "true" || server.arg("lowSpeedMuteEnabled") == "1";
        }
        if (server.hasArg("lowSpeedMuteThresholdMph")) {
            int val = server.arg("lowSpeedMuteThresholdMph").toInt();
            threshold = (uint8_t)std::max(1, std::min(val, 30));  // 1-30 mph range
        }
        settingsManager.setLowSpeedMute(enabled, threshold);
    }
    if (server.hasArg("brightness")) {
        int brightness = server.arg("brightness").toInt();
        brightness = std::max(0, std::min(brightness, 255));
        settingsManager.updateBrightness((uint8_t)brightness);
        display.setBrightness((uint8_t)brightness);
    }
    if (server.hasArg("voiceVolume")) {
        int volume = server.arg("voiceVolume").toInt();
        volume = std::max(0, std::min(volume, 100));
        settingsManager.updateVoiceVolume((uint8_t)volume);
        audio_set_volume((uint8_t)volume);
    }

    // Persist all color/visibility changes
    settingsManager.save();

    // Apply debug logging runtime state immediately
    applyDebugLogFilterFromSettings();
    debugLogger.setEnabled(settingsManager.get().enableDebugLogging);
    if (debugLogger.isEnabled()) {
        debugLogger.logf(DebugLogCategory::System, "Debug logging enabled via /api/displaycolors (size=%u bytes)", (unsigned int)debugLogger.size());
    }
    
    // Trigger immediate display preview to show new colors
    display.showDemo();
    requestColorPreviewHold(5500);  // Hold ~5.5s and cycle bands during preview
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleDisplayColorsReset() {
    if (!checkRateLimit()) return;
    
    // Reset to default colors: Bogey/Freq=Red, Front/Side/Rear=Red, L/K=Blue, Ka=Red, X=Green, WiFi=Cyan
    settingsManager.setDisplayColors(0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0x001F, 0xF800, 0x001F, 0x07E0);
    settingsManager.setWiFiIconColors(0x07FF, 0x07E0);  // Cyan (no client), Green (connected)
    // Reset bar colors: Green, Green, Yellow, Yellow, Red, Red
    settingsManager.setSignalBarColors(0x07E0, 0x07E0, 0xFFE0, 0xFFE0, 0xF800, 0xF800);
    // Reset muted color to default dark grey
    settingsManager.setMutedColor(0x3186);
    // Reset persisted color to darker grey
    settingsManager.setPersistedColor(0x18C3);
    // Reset volume indicator colors: Main=Blue, Mute=Yellow
    settingsManager.setVolumeMainColor(0x001F);
    settingsManager.setVolumeMuteColor(0xFFE0);
    // Reset RSSI label colors: V1=Green, Proxy=Blue
    settingsManager.setRssiV1Color(0x07E0);
    settingsManager.setRssiProxyColor(0x001F);
    // Reset frequency use band color to off
    settingsManager.setFreqUseBandColor(false);
    
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
    doc["statusGps"] = s.colorStatusGps;
    doc["statusGpsWarn"] = s.colorStatusGpsWarn;
    doc["statusCam"] = s.colorStatusCam;
    doc["statusObd"] = s.colorStatusObd;
    doc["freqUseBandColor"] = s.freqUseBandColor;
    doc["hideWifiIcon"] = s.hideWifiIcon;
    doc["hideProfileIndicator"] = s.hideProfileIndicator;
    doc["hideBatteryIcon"] = s.hideBatteryIcon;
    doc["showBatteryPercent"] = s.showBatteryPercent;
    doc["hideBleIcon"] = s.hideBleIcon;
    doc["hideVolumeIndicator"] = s.hideVolumeIndicator;
    doc["hideRssiIndicator"] = s.hideRssiIndicator;
    doc["kittScannerEnabled"] = s.kittScannerEnabled;
    doc["enableWifiAtBoot"] = s.enableWifiAtBoot;
    doc["enableDebugLogging"] = s.enableDebugLogging;
    doc["logAlerts"] = s.logAlerts;
    doc["logWifi"] = s.logWifi;
    doc["logBle"] = s.logBle;
    doc["logGps"] = s.logGps;
    doc["logObd"] = s.logObd;
    doc["logSystem"] = s.logSystem;
    doc["logDisplay"] = s.logDisplay;
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
    doc["oversizeDrops"] = perfCounters.oversizeDrops.load();
    doc["queueHighWater"] = perfCounters.queueHighWater.load();
    doc["displayUpdates"] = perfCounters.displayUpdates.load();
    doc["displaySkips"] = perfCounters.displaySkips.load();
    doc["reconnects"] = perfCounters.reconnects.load();
    doc["disconnects"] = perfCounters.disconnects.load();
    
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
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleDebugEvents() {
    String json = eventRingToJson();
    server.send(200, "application/json", json);
}

void WiFiManager::handleDebugEventsClear() {
    if (!checkRateLimit()) return;
    eventRingClear();
    server.send(200, "application/json", "{\"success\":true}");
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

void WiFiManager::handleDebugLogsMeta() {
    if (!checkRateLimit()) return;

    JsonDocument doc;
    doc["enabled"] = settingsManager.get().enableDebugLogging;
    doc["canEnable"] = debugLogger.canEnable();  // SD card required
    doc["storageReady"] = storageManager.isReady();
    doc["onSdCard"] = storageManager.isSDCard();
    doc["exists"] = debugLogger.exists();
    doc["sizeBytes"] = static_cast<uint32_t>(debugLogger.size());
    doc["maxSizeBytes"] = static_cast<uint32_t>(DEBUG_LOG_MAX_BYTES);
    doc["path"] = DEBUG_LOG_PATH;
    DebugLogConfig cfg = settingsManager.getDebugLogConfig();
    doc["logAlerts"] = cfg.alerts;
    doc["logWifi"] = cfg.wifi;
    doc["logBle"] = cfg.ble;
    doc["logGps"] = cfg.gps;
    doc["logObd"] = cfg.obd;
    doc["logSystem"] = cfg.system;
    doc["logDisplay"] = cfg.display;

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleDebugLogsDownload() {
    if (!checkRateLimit()) return;

    if (!storageManager.isReady()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"Storage not available\"}");
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists(DEBUG_LOG_PATH)) {
        server.send(404, "application/json", "{\"success\":false,\"error\":\"Log file not found\"}");
        return;
    }

    File f = fs->open(DEBUG_LOG_PATH, FILE_READ);
    if (!f) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Failed to open log\"}");
        return;
    }

    server.sendHeader("Content-Type", "text/plain");
    server.sendHeader("Content-Disposition", "attachment; filename=\"debug.log\"");
    server.sendHeader("Cache-Control", "no-cache");
    server.streamFile(f, "text/plain");
    f.close();
}

void WiFiManager::handleDebugLogsTail() {
    if (!checkRateLimit()) return;

    // Optional ?bytes= parameter (default 32KB, max 64KB)
    size_t maxBytes = 32768;
    if (server.hasArg("bytes")) {
        maxBytes = server.arg("bytes").toInt();
        if (maxBytes > 65536) maxBytes = 65536;  // Cap at 64KB
        if (maxBytes < 1024) maxBytes = 1024;    // Min 1KB
    }

    String content = debugLogger.tail(maxBytes);

    JsonDocument doc;
    doc["content"] = content;
    doc["bytes"] = content.length();
    doc["totalSize"] = static_cast<uint32_t>(debugLogger.size());
    doc["exists"] = debugLogger.exists();
    doc["enabled"] = debugLogger.isEnabled();

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleDebugLogsClear() {
    if (!checkRateLimit()) return;

    bool ok = debugLogger.clear();

    JsonDocument doc;
    doc["success"] = ok;
    doc["enabled"] = debugLogger.isEnabled();
    doc["exists"] = debugLogger.exists();
    doc["sizeBytes"] = static_cast<uint32_t>(debugLogger.size());

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
    doc["_version"] = 2;  // Backup format version
    doc["_type"] = "v1simple_backup";
    doc["_timestamp"] = millis();
    
    // WiFi settings (exclude password for security)
    doc["apSSID"] = s.apSSID;
    // Note: password not included in backup for security
    
    // BLE settings
    doc["proxyBLE"] = s.proxyBLE;
    doc["proxyName"] = s.proxyName;
    
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
    doc["colorStatusGps"] = s.colorStatusGps;
    doc["colorStatusGpsWarn"] = s.colorStatusGpsWarn;
    doc["colorStatusCam"] = s.colorStatusCam;
    doc["colorStatusObd"] = s.colorStatusObd;
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
    doc["kittScannerEnabled"] = s.kittScannerEnabled;
    
    // Development/Debug
    doc["enableWifiAtBoot"] = s.enableWifiAtBoot;
    doc["enableDebugLogging"] = s.enableDebugLogging;
    doc["logAlerts"] = s.logAlerts;
    doc["logWifi"] = s.logWifi;
    doc["logBle"] = s.logBle;
    doc["logGps"] = s.logGps;
    doc["logObd"] = s.logObd;
    doc["logSystem"] = s.logSystem;
    doc["logDisplay"] = s.logDisplay;
    doc["logPerfMetrics"] = s.logPerfMetrics;
    doc["logAudio"] = s.logAudio;
    doc["logCamera"] = s.logCamera;
    doc["logLockout"] = s.logLockout;
    doc["logTouch"] = s.logTouch;
    
    // WiFi client settings
    doc["wifiMode"] = (int)s.wifiMode;
    doc["wifiClientEnabled"] = s.wifiClientEnabled;
    doc["wifiClientSSID"] = s.wifiClientSSID;
    
    // GPS settings
    doc["gpsEnabled"] = s.gpsEnabled;
    
    // OBD settings
    doc["obdEnabled"] = s.obdEnabled;
    doc["obdDeviceAddress"] = s.obdDeviceAddress;
    doc["obdDeviceName"] = s.obdDeviceName;
    doc["obdPin"] = s.obdPin;
    
    // Auto-lockout settings
    doc["lockoutEnabled"] = s.lockoutEnabled;
    doc["lockoutKaProtection"] = s.lockoutKaProtection;
    doc["lockoutDirectionalUnlearn"] = s.lockoutDirectionalUnlearn;
    doc["lockoutFreqToleranceMHz"] = s.lockoutFreqToleranceMHz;
    doc["lockoutLearnCount"] = s.lockoutLearnCount;
    doc["lockoutUnlearnCount"] = s.lockoutUnlearnCount;
    doc["lockoutManualDeleteCount"] = s.lockoutManualDeleteCount;
    doc["lockoutLearnIntervalHours"] = s.lockoutLearnIntervalHours;
    doc["lockoutUnlearnIntervalHours"] = s.lockoutUnlearnIntervalHours;
    doc["lockoutMaxSignalStrength"] = s.lockoutMaxSignalStrength;
    doc["lockoutMaxDistanceM"] = s.lockoutMaxDistanceM;
    
    // Camera alert settings
    doc["cameraAlertsEnabled"] = s.cameraAlertsEnabled;
    doc["cameraAlertDistanceM"] = s.cameraAlertDistanceM;
    doc["cameraAlertRedLight"] = s.cameraAlertRedLight;
    doc["cameraAlertSpeed"] = s.cameraAlertSpeed;
    doc["cameraAlertALPR"] = s.cameraAlertALPR;
    doc["cameraAudioEnabled"] = s.cameraAudioEnabled;
    doc["colorCameraAlert"] = s.colorCameraAlert;
    
    // Auto power-off
    doc["autoPowerOffMinutes"] = s.autoPowerOffMinutes;
    
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
    if (doc["proxyBLE"].is<bool>()) settingsManager.setProxyBLE(doc["proxyBLE"]);
    if (doc["proxyName"].is<const char*>()) settingsManager.setProxyName(doc["proxyName"].as<String>());
    
    // WiFi settings (password intentionally excluded from backups)
    if (doc["apSSID"].is<const char*>()) {
        // Preserve existing password while restoring SSID
        settingsManager.updateAPCredentials(doc["apSSID"].as<String>(), settingsManager.get().apPassword);
    }
    
    // Display settings
    if (doc["brightness"].is<int>()) s.brightness = doc["brightness"];
    if (doc["displayStyle"].is<int>()) s.displayStyle = (DisplayStyle)doc["displayStyle"].as<int>();
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
    if (doc["colorStatusGps"].is<int>()) s.colorStatusGps = doc["colorStatusGps"];
    if (doc["colorStatusGpsWarn"].is<int>()) s.colorStatusGpsWarn = doc["colorStatusGpsWarn"];
    if (doc["colorStatusCam"].is<int>()) s.colorStatusCam = doc["colorStatusCam"];
    if (doc["colorStatusObd"].is<int>()) s.colorStatusObd = doc["colorStatusObd"];
    if (doc["colorCameraAlert"].is<int>()) s.colorCameraAlert = doc["colorCameraAlert"];
    if (doc["freqUseBandColor"].is<bool>()) s.freqUseBandColor = doc["freqUseBandColor"];
    
    // Display visibility
    if (doc["hideWifiIcon"].is<bool>()) s.hideWifiIcon = doc["hideWifiIcon"];
    if (doc["hideProfileIndicator"].is<bool>()) s.hideProfileIndicator = doc["hideProfileIndicator"];
    if (doc["hideBatteryIcon"].is<bool>()) s.hideBatteryIcon = doc["hideBatteryIcon"];
    if (doc["showBatteryPercent"].is<bool>()) s.showBatteryPercent = doc["showBatteryPercent"];
    if (doc["hideBleIcon"].is<bool>()) s.hideBleIcon = doc["hideBleIcon"];
    if (doc["hideVolumeIndicator"].is<bool>()) s.hideVolumeIndicator = doc["hideVolumeIndicator"];
    if (doc["hideRssiIndicator"].is<bool>()) s.hideRssiIndicator = doc["hideRssiIndicator"];
    if (doc["kittScannerEnabled"].is<bool>()) s.kittScannerEnabled = doc["kittScannerEnabled"];
    
    // Development/Debug
    if (doc["enableWifiAtBoot"].is<bool>()) s.enableWifiAtBoot = doc["enableWifiAtBoot"];
    if (doc["enableDebugLogging"].is<bool>()) s.enableDebugLogging = doc["enableDebugLogging"];
    if (doc["logAlerts"].is<bool>()) s.logAlerts = doc["logAlerts"];
    if (doc["logWifi"].is<bool>()) s.logWifi = doc["logWifi"];
    if (doc["logBle"].is<bool>()) s.logBle = doc["logBle"];
    if (doc["logGps"].is<bool>()) s.logGps = doc["logGps"];
    if (doc["logObd"].is<bool>()) s.logObd = doc["logObd"];
    if (doc["logSystem"].is<bool>()) s.logSystem = doc["logSystem"];
    if (doc["logDisplay"].is<bool>()) s.logDisplay = doc["logDisplay"];
    if (doc["logPerfMetrics"].is<bool>()) s.logPerfMetrics = doc["logPerfMetrics"];
    if (doc["logAudio"].is<bool>()) s.logAudio = doc["logAudio"];
    if (doc["logCamera"].is<bool>()) s.logCamera = doc["logCamera"];
    if (doc["logLockout"].is<bool>()) s.logLockout = doc["logLockout"];
    if (doc["logTouch"].is<bool>()) s.logTouch = doc["logTouch"];
    
    // WiFi client settings
    if (doc["wifiMode"].is<int>()) s.wifiMode = (WiFiModeSetting)doc["wifiMode"].as<int>();
    if (doc["wifiClientEnabled"].is<bool>()) s.wifiClientEnabled = doc["wifiClientEnabled"];
    if (doc["wifiClientSSID"].is<const char*>()) s.wifiClientSSID = doc["wifiClientSSID"].as<String>();
    
    // GPS settings
    if (doc["gpsEnabled"].is<bool>()) s.gpsEnabled = doc["gpsEnabled"];
    
    // OBD settings
    if (doc["obdEnabled"].is<bool>()) s.obdEnabled = doc["obdEnabled"];
    if (doc["obdDeviceAddress"].is<const char*>()) s.obdDeviceAddress = doc["obdDeviceAddress"].as<String>();
    if (doc["obdDeviceName"].is<const char*>()) s.obdDeviceName = doc["obdDeviceName"].as<String>();
    if (doc["obdPin"].is<const char*>()) s.obdPin = doc["obdPin"].as<String>();
    
    // Auto-lockout settings
    if (doc["lockoutEnabled"].is<bool>()) s.lockoutEnabled = doc["lockoutEnabled"];
    if (doc["lockoutKaProtection"].is<bool>()) s.lockoutKaProtection = doc["lockoutKaProtection"];
    if (doc["lockoutDirectionalUnlearn"].is<bool>()) s.lockoutDirectionalUnlearn = doc["lockoutDirectionalUnlearn"];
    if (doc["lockoutFreqToleranceMHz"].is<int>()) s.lockoutFreqToleranceMHz = doc["lockoutFreqToleranceMHz"];
    if (doc["lockoutLearnCount"].is<int>()) s.lockoutLearnCount = doc["lockoutLearnCount"];
    if (doc["lockoutUnlearnCount"].is<int>()) s.lockoutUnlearnCount = doc["lockoutUnlearnCount"];
    if (doc["lockoutManualDeleteCount"].is<int>()) s.lockoutManualDeleteCount = doc["lockoutManualDeleteCount"];
    if (doc["lockoutLearnIntervalHours"].is<int>()) s.lockoutLearnIntervalHours = doc["lockoutLearnIntervalHours"];
    if (doc["lockoutUnlearnIntervalHours"].is<int>()) s.lockoutUnlearnIntervalHours = doc["lockoutUnlearnIntervalHours"];
    if (doc["lockoutMaxSignalStrength"].is<int>()) s.lockoutMaxSignalStrength = doc["lockoutMaxSignalStrength"];
    if (doc["lockoutMaxDistanceM"].is<int>()) s.lockoutMaxDistanceM = doc["lockoutMaxDistanceM"];
    
    // Camera alert settings
    if (doc["cameraAlertsEnabled"].is<bool>()) s.cameraAlertsEnabled = doc["cameraAlertsEnabled"];
    if (doc["cameraAlertDistanceM"].is<int>()) s.cameraAlertDistanceM = doc["cameraAlertDistanceM"];
    if (doc["cameraAlertRedLight"].is<bool>()) s.cameraAlertRedLight = doc["cameraAlertRedLight"];
    if (doc["cameraAlertSpeed"].is<bool>()) s.cameraAlertSpeed = doc["cameraAlertSpeed"];
    if (doc["cameraAlertALPR"].is<bool>()) s.cameraAlertALPR = doc["cameraAlertALPR"];
    if (doc["cameraAudioEnabled"].is<bool>()) s.cameraAudioEnabled = doc["cameraAudioEnabled"];
    
    // Auto power-off
    if (doc["autoPowerOffMinutes"].is<int>()) s.autoPowerOffMinutes = doc["autoPowerOffMinutes"];
    
    // Voice settings
    if (doc["voiceAlertMode"].is<int>()) s.voiceAlertMode = (VoiceAlertMode)doc["voiceAlertMode"].as<int>();
    if (doc["voiceDirectionEnabled"].is<bool>()) s.voiceDirectionEnabled = doc["voiceDirectionEnabled"];
    if (doc["announceBogeyCount"].is<bool>()) s.announceBogeyCount = doc["announceBogeyCount"];
    if (doc["muteVoiceIfVolZero"].is<bool>()) s.muteVoiceIfVolZero = doc["muteVoiceIfVolZero"];
    if (doc["voiceVolume"].is<int>()) s.voiceVolume = doc["voiceVolume"];
    if (doc["announceSecondaryAlerts"].is<bool>()) s.announceSecondaryAlerts = doc["announceSecondaryAlerts"];
    if (doc["secondaryLaser"].is<bool>()) s.secondaryLaser = doc["secondaryLaser"];
    if (doc["secondaryKa"].is<bool>()) s.secondaryKa = doc["secondaryKa"];
    if (doc["secondaryK"].is<bool>()) s.secondaryK = doc["secondaryK"];
    if (doc["secondaryX"].is<bool>()) s.secondaryX = doc["secondaryX"];
    
    // Auto-push slot settings
    if (doc["autoPushEnabled"].is<bool>()) s.autoPushEnabled = doc["autoPushEnabled"];
    if (doc["activeSlot"].is<int>()) s.activeSlot = doc["activeSlot"];
    if (doc["slot0Name"].is<const char*>()) s.slot0Name = doc["slot0Name"].as<String>();
    if (doc["slot1Name"].is<const char*>()) s.slot1Name = doc["slot1Name"].as<String>();
    if (doc["slot2Name"].is<const char*>()) s.slot2Name = doc["slot2Name"].as<String>();
    if (doc["slot0Color"].is<int>()) s.slot0Color = doc["slot0Color"];
    if (doc["slot1Color"].is<int>()) s.slot1Color = doc["slot1Color"];
    if (doc["slot2Color"].is<int>()) s.slot2Color = doc["slot2Color"];
    if (doc["slot0Volume"].is<int>()) s.slot0Volume = doc["slot0Volume"];
    if (doc["slot1Volume"].is<int>()) s.slot1Volume = doc["slot1Volume"];
    if (doc["slot2Volume"].is<int>()) s.slot2Volume = doc["slot2Volume"];
    if (doc["slot0MuteVolume"].is<int>()) s.slot0MuteVolume = doc["slot0MuteVolume"];
    if (doc["slot1MuteVolume"].is<int>()) s.slot1MuteVolume = doc["slot1MuteVolume"];
    if (doc["slot2MuteVolume"].is<int>()) s.slot2MuteVolume = doc["slot2MuteVolume"];
    if (doc["slot0DarkMode"].is<bool>()) s.slot0DarkMode = doc["slot0DarkMode"];
    if (doc["slot1DarkMode"].is<bool>()) s.slot1DarkMode = doc["slot1DarkMode"];
    if (doc["slot2DarkMode"].is<bool>()) s.slot2DarkMode = doc["slot2DarkMode"];
    if (doc["slot0MuteToZero"].is<bool>()) s.slot0MuteToZero = doc["slot0MuteToZero"];
    if (doc["slot1MuteToZero"].is<bool>()) s.slot1MuteToZero = doc["slot1MuteToZero"];
    if (doc["slot2MuteToZero"].is<bool>()) s.slot2MuteToZero = doc["slot2MuteToZero"];
    if (doc["slot0AlertPersist"].is<int>()) s.slot0AlertPersist = doc["slot0AlertPersist"];
    if (doc["slot1AlertPersist"].is<int>()) s.slot1AlertPersist = doc["slot1AlertPersist"];
    if (doc["slot2AlertPersist"].is<int>()) s.slot2AlertPersist = doc["slot2AlertPersist"];
    if (doc["slot0PriorityArrow"].is<bool>()) s.slot0PriorityArrow = doc["slot0PriorityArrow"];
    if (doc["slot1PriorityArrow"].is<bool>()) s.slot1PriorityArrow = doc["slot1PriorityArrow"];
    if (doc["slot2PriorityArrow"].is<bool>()) s.slot2PriorityArrow = doc["slot2PriorityArrow"];
    if (doc["slot0ProfileName"].is<const char*>()) s.slot0_default.profileName = doc["slot0ProfileName"].as<String>();
    if (doc["slot0Mode"].is<int>()) s.slot0_default.mode = static_cast<V1Mode>(doc["slot0Mode"].as<int>());
    if (doc["slot1ProfileName"].is<const char*>()) s.slot1_highway.profileName = doc["slot1ProfileName"].as<String>();
    if (doc["slot1Mode"].is<int>()) s.slot1_highway.mode = static_cast<V1Mode>(doc["slot1Mode"].as<int>());
    if (doc["slot2ProfileName"].is<const char*>()) s.slot2_comfort.profileName = doc["slot2ProfileName"].as<String>();
    if (doc["slot2Mode"].is<int>()) s.slot2_comfort.mode = static_cast<V1Mode>(doc["slot2Mode"].as<int>());
    
    // Restore V1 profiles if present
    int profilesRestored = 0;
    if (doc["profiles"].is<JsonArray>()) {
        JsonArray profilesArr = doc["profiles"].as<JsonArray>();
        for (JsonObject p : profilesArr) {
            if (!p["name"].is<const char*>() || !p["bytes"].is<JsonArray>()) {
                continue;  // Skip invalid profile entries
            }
            
            V1Profile profile;
            profile.name = p["name"].as<String>();
            if (p["description"].is<const char*>()) profile.description = p["description"].as<String>();
            if (p["displayOn"].is<bool>()) profile.displayOn = p["displayOn"];
            if (p["mainVolume"].is<int>()) profile.mainVolume = p["mainVolume"];
            if (p["mutedVolume"].is<int>()) profile.mutedVolume = p["mutedVolume"];
            
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

    // Re-apply debug logging runtime state based on restored settings
    applyDebugLogFilterFromSettings();
    debugLogger.setEnabled(settingsManager.get().enableDebugLogging);
    if (debugLogger.isEnabled()) {
        debugLogger.log(DebugLogCategory::System, "Debug logging enabled via settings restore");
    }
    
    Serial.printf("[Settings] Restored from uploaded backup (%d profiles)\n", profilesRestored);
    
    // Build response with profile count
    String response = "{\"success\":true,\"message\":\"Settings restored successfully";
    if (profilesRestored > 0) {
        response += " (" + String(profilesRestored) + " profiles)";
    }
    response += "\"}";
    server.send(200, "application/json", response);
}

// ============== OBD-II API Handlers ==============

void WiFiManager::handleObdStatus() {
    JsonDocument doc;
    
    doc["enabled"] = settingsManager.isObdEnabled();
    doc["state"] = obdHandler.getStateString();
    doc["connected"] = obdHandler.isConnected();
    doc["scanning"] = obdHandler.isScanActive();
    doc["moduleDetected"] = obdHandler.isModuleDetected();
    doc["deviceName"] = obdHandler.getConnectedDeviceName();
    doc["savedDeviceAddress"] = settingsManager.getObdDeviceAddress();
    doc["savedDeviceName"] = settingsManager.getObdDeviceName();
    doc["pin"] = settingsManager.getObdPin();
    
    if (obdHandler.hasValidData()) {
        OBDData data = obdHandler.getData();
        doc["speedMph"] = data.speed_mph;
        doc["speedKph"] = data.speed_kph;
        doc["rpm"] = data.rpm;
        doc["voltage"] = data.voltage;
    }
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void WiFiManager::handleObdScan() {
    if (!checkRateLimit()) return;
    
    if (!settingsManager.isObdEnabled()) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"OBD not enabled\"}");
        return;
    }
    
    obdHandler.startScan();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Scan started\"}");
}

void WiFiManager::handleObdScanStop() {
    if (!checkRateLimit()) return;
    
    if (!settingsManager.isObdEnabled()) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"OBD not enabled\"}");
        return;
    }
    
    obdHandler.stopScan();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Scan stopped\"}");
}

void WiFiManager::handleObdDevices() {
    JsonDocument doc;
    JsonArray devices = doc["devices"].to<JsonArray>();
    
    for (const auto& device : obdHandler.getFoundDevices()) {
        JsonObject d = devices.add<JsonObject>();
        d["address"] = device.address;
        d["name"] = device.name;
        d["rssi"] = device.rssi;
    }
    
    doc["scanning"] = obdHandler.isScanActive();
    doc["count"] = obdHandler.getFoundDevices().size();
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void WiFiManager::handleObdConnect() {
    if (!checkRateLimit()) return;
    
    if (!server.hasArg("address")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing address\"}");
        return;
    }
    
    String address = server.arg("address");
    String name = server.hasArg("name") ? server.arg("name") : "";
    
    // Save PIN if provided
    if (server.hasArg("pin")) {
        settingsManager.setObdPin(server.arg("pin"));
    }
    
    // Save device selection
    settingsManager.setObdDevice(address, name);
    
    // Initiate connection
    bool started = obdHandler.connectToAddress(address, name);
    
    if (started) {
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Connecting to device\"}");
    } else {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Failed to start connection\"}");
    }
}

void WiFiManager::handleObdDevicesClear() {
    if (!checkRateLimit()) return;
    
    obdHandler.clearFoundDevices();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Scan results cleared\"}");
}

void WiFiManager::handleObdForget() {
    if (!checkRateLimit()) return;
    
    // Clear the saved device from settings
    settingsManager.setObdDevice("", "");
    
    // Disconnect if currently connected
    obdHandler.disconnect();
    
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Saved device forgotten\"}");
}

void WiFiManager::handleGpsStatus() {
    markUiActivity();
    
    if (!getGpsStatusJson) {
        server.send(503, "application/json", "{\"error\":\"GPS handler not available\"}");
        return;
    }
    
    server.send(200, "application/json", getGpsStatusJson());
}

void WiFiManager::handleGpsReset() {
    if (!checkRateLimit()) return;
    markUiActivity();
    
    if (!gpsResetCallback) {
        server.send(503, "application/json", "{\"error\":\"GPS handler not available\"}");
        return;
    }
    
    Serial.println("[HTTP] POST /api/gps/reset - power cycling GPS module");
    gpsResetCallback();
    
    server.send(200, "application/json", "{\"success\":true,\"message\":\"GPS module reset initiated\"}");
}

void WiFiManager::handleCameraStatus() {
    markUiActivity();
    
    if (!getCameraStatusJson) {
        // Return empty status if camera manager not available
        server.send(200, "application/json", "{\"loaded\":false,\"count\":0}");
        return;
    }
    
    server.send(200, "application/json", getCameraStatusJson());
}

void WiFiManager::handleCameraReload() {
    if (!checkRateLimit()) return;
    markUiActivity();
    
    if (!cameraReloadCallback) {
        server.send(503, "application/json", "{\"error\":\"Camera manager not available\"}");
        return;
    }
    
    Serial.println("[HTTP] POST /api/cameras/reload - reloading camera database");
    bool success = cameraReloadCallback();
    
    if (success) {
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Camera database reloaded\"}");
    } else {
        server.send(200, "application/json", "{\"success\":false,\"message\":\"No camera database found on SD card\"}");
    }
}

void WiFiManager::handleCameraUpload() {
    if (!checkRateLimit()) return;
    markUiActivity();
    
    // Get filesystem for saving
    fs::FS* fs = getFilesystem ? getFilesystem() : nullptr;
    if (!fs) {
        server.send(503, "application/json", "{\"error\":\"SD card not available\"}");
        return;
    }
    
    // Get POST body (NDJSON data)
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No data provided\"}");
        return;
    }
    
    String body = server.arg("plain");
    if (body.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"Empty data\"}");
        return;
    }
    
    Serial.printf("[HTTP] POST /api/cameras/upload - received %d bytes\n", body.length());
    
    // Save to SD card as ALPR database
    const char* filename = "/alpr_osm.json";
    File file = fs->open(filename, "w");
    if (!file) {
        server.send(500, "application/json", "{\"error\":\"Failed to create file on SD\"}");
        return;
    }
    
    size_t written = file.print(body);
    file.close();
    
    Serial.printf("[HTTP] Saved %d bytes to %s\n", written, filename);
    
    // If we have an upload callback (to trigger reload), call it
    if (cameraUploadCallback) {
        cameraUploadCallback(String(filename));
    }
    
    // Try to reload the camera database
    bool reloaded = cameraReloadCallback ? cameraReloadCallback() : false;
    
    char response[256];
    snprintf(response, sizeof(response), 
             "{\"success\":true,\"bytes\":%d,\"file\":\"%s\",\"reloaded\":%s}",
             written, filename, reloaded ? "true" : "false");
    
    server.send(200, "application/json", response);
}

void WiFiManager::handleCameraTest() {
    if (!checkRateLimit()) return;
    markUiActivity();
    
    // Get camera type from query param (default to 0 = red light)
    int cameraType = 0;
    if (server.hasArg("type")) {
        cameraType = server.arg("type").toInt();
    }
    
    Serial.printf("[HTTP] POST /api/cameras/test - type=%d\n", cameraType);
    
    // Call the test callback to trigger display + voice
    if (cameraTestCallback) {
        cameraTestCallback(cameraType);
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Camera test triggered\"}");
    } else {
        server.send(503, "application/json", "{\"success\":false,\"message\":\"Test callback not configured\"}");
    }
}

void WiFiManager::handleCameraSyncOsm() {
    if (!checkRateLimit()) return;
    markUiActivity();
    
    Serial.println("[HTTP] POST /api/cameras/sync-osm - Starting OSM sync");
    
    // Check if WiFi STA is connected to internet
    if (wifiClientState != WIFI_CLIENT_CONNECTED) {
        server.send(400, "application/json", 
            "{\"success\":false,\"error\":\"Not connected to external WiFi. Connect to a network first.\"}");
        return;
    }
    
    // Get filesystem for saving
    fs::FS* fs = getFilesystem ? getFilesystem() : nullptr;
    if (!fs) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"SD card not available\"}");
        return;
    }
    
    // Overpass query for ALPR cameras in US
    // - maxsize:1048576 limits response to 1MB to prevent OOM
    // - timeout:60 reduces server-side timeout
    // - out center qt 2000 limits to 2000 elements
    const char* overpassQuery = 
        "[out:json][timeout:60][maxsize:1048576];"
        "area[\"ISO3166-1\"=\"US\"]->.usa;"
        "(node[\"surveillance:type\"=\"ALPR\"](area.usa);"
        "way[\"surveillance:type\"=\"ALPR\"](area.usa););"
        "out center qt 2000;";
    
    HTTPClient http;
    http.setTimeout(60000);   // 1 minute timeout (max ~65s for uint16_t)
    http.setConnectTimeout(30000);  // 30s connect timeout
    
    // Use POST to Overpass API
    const char* overpassUrl = "https://overpass-api.de/api/interpreter";
    
    Serial.println("[OSM] Connecting to Overpass API...");
    
    if (!http.begin(overpassUrl)) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Failed to connect to Overpass API\"}");
        return;
    }
    
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    
    String postData = "data=" + String(overpassQuery);
    postData.replace(" ", "%20");
    postData.replace("\"", "%22");
    
    Serial.printf("[OSM] Sending query (%d bytes)...\n", postData.length());
    
    int httpCode = http.POST(postData);
    
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[OSM] HTTP error: %d\n", httpCode);
        http.end();
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), 
            "{\"success\":false,\"error\":\"Overpass API returned %d\"}", httpCode);
        server.send(502, "application/json", errBuf);
        return;
    }
    
    // Get response size
    int contentLength = http.getSize();
    Serial.printf("[OSM] Response size: %d bytes\n", contentLength);
    
    // Check response size to prevent OOM (1MB limit)
    constexpr int MAX_RESPONSE_SIZE = 1048576;  // 1MB
    if (contentLength > MAX_RESPONSE_SIZE) {
        Serial.printf("[OSM] Response too large: %d bytes (max %d)\n", contentLength, MAX_RESPONSE_SIZE);
        http.end();
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), 
            "{\"success\":false,\"error\":\"Response too large (%d KB, max 1MB)\"}", contentLength / 1024);
        server.send(413, "application/json", errBuf);
        return;
    }
    
    // Stream the response to parse JSON
    WiFiClient* stream = http.getStreamPtr();
    
    // Use JSON filter to only parse needed fields (reduces memory usage)
    JsonDocument filter;
    filter["elements"][0]["type"] = true;
    filter["elements"][0]["lat"] = true;
    filter["elements"][0]["lon"] = true;
    filter["elements"][0]["center"]["lat"] = true;
    filter["elements"][0]["center"]["lon"] = true;
    
    // Parse with filter and size limit
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *stream, 
        DeserializationOption::Filter(filter),
        DeserializationOption::NestingLimit(10));
    http.end();
    
    if (error) {
        Serial.printf("[OSM] JSON parse error: %s\n", error.c_str());
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), 
            "{\"success\":false,\"error\":\"JSON parse failed: %s\"}", error.c_str());
        server.send(500, "application/json", errBuf);
        return;
    }
    
    // Extract elements
    JsonArray elements = doc["elements"];
    int count = elements.size();
    Serial.printf("[OSM] Found %d ALPR cameras\n", count);
    
    if (count == 0) {
        server.send(200, "application/json", 
            "{\"success\":true,\"count\":0,\"message\":\"No cameras found\"}");
        return;
    }
    
    // Convert to NDJSON and save
    const char* filename = "/alpr_osm.json";
    File file = fs->open(filename, "w");
    if (!file) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Failed to create file\"}");
        return;
    }
    
    // Write metadata header with date from GPS or compile time
    char dateBuf[32];
    if (gpsHandler.hasValidTime()) {
        // Use GPS time (most accurate)
        GPSFix fix = gpsHandler.getFix();
        snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", 2000 + fix.year, fix.month, fix.day);
    } else {
        // Fall back to compile date (__DATE__ = "Jan 27 2026")
        // Parse __DATE__ which is "Mmm DD YYYY" format
        const char* compileDate = __DATE__;  // e.g., "Jan 27 2026"
        int year = 0, day = 0;
        char monthStr[4] = {0};
        sscanf(compileDate, "%3s %d %d", monthStr, &day, &year);
        const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        int month = (strstr(months, monthStr) - months) / 3 + 1;
        snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", year, month, day);
    }
    file.printf("{\"_meta\":{\"name\":\"OSM ALPR (US)\",\"date\":\"%s\"}}\n", dateBuf);
    
    int written = 0;
    for (JsonObject el : elements) {
        float lat = 0, lon = 0;
        
        if (el["type"] == "node") {
            lat = el["lat"];
            lon = el["lon"];
        } else if (el["type"] == "way" && el["center"].is<JsonObject>()) {
            lat = el["center"]["lat"];
            lon = el["center"]["lon"];
        } else {
            continue;
        }
        
        // Write NDJSON record: {"lat":...,"lon":...,"flg":4}
        file.printf("{\"lat\":%.6f,\"lon\":%.6f,\"flg\":4}\n", lat, lon);
        written++;
    }
    
    file.close();
    Serial.printf("[OSM] Saved %d cameras to %s\n", written, filename);
    
    // Trigger camera reload
    bool reloaded = cameraReloadCallback ? cameraReloadCallback() : false;
    
    char response[256];
    snprintf(response, sizeof(response), 
        "{\"success\":true,\"count\":%d,\"file\":\"%s\",\"reloaded\":%s}",
        written, filename, reloaded ? "true" : "false");
    
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
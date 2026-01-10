/**
 * WiFi Manager for V1 Gen2 Display
 * AP-only: always-on access point serving the local UI/API.
 */

#include "wifi_manager.h"
#include "settings.h"
#include "display.h"
#include "storage_manager.h"
#include "v1_profiles.h"
#include "ble_client.h"
#include "perf_metrics.h"
#include "event_ring.h"
#include "../include/config.h"
#include "../include/color_themes.h"
#include <algorithm>
#include <ArduinoJson.h>
#include <LittleFS.h>

// External BLE client for V1 commands
extern V1BLEClient bleClient;
// Preview hold helper to keep color demo visible briefly
extern void requestColorPreviewHold(uint32_t durationMs);
extern bool isColorPreviewRunning();
extern void cancelColorPreview();

// Enable to dump LittleFS root on WiFi start (debug only); keep false for release
static constexpr bool WIFI_DEBUG_FS_DUMP = false;

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

    WiFi.mode(WIFI_AP);

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
    // Use a constant, predictable SSID for Setup Mode
    const char* apSSID = "V1-Simple";
    const char* apPass = "setupv1g2";  // Simple password (unchanged)
    
    Serial.printf("[SetupMode] Starting AP: %s (pass: %s)\n", apSSID, apPass);
    
    // Configure AP IP
    IPAddress apIP(192, 168, 35, 5);
    IPAddress gateway(192, 168, 35, 5);
    IPAddress subnet(255, 255, 255, 0);
    
    if (!WiFi.softAPConfig(apIP, gateway, subnet)) {
        Serial.println("[SetupMode] softAPConfig failed!");
    }
    
    if (!WiFi.softAP(apSSID, apPass)) {
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
        if (isColorPreviewRunning()) {
            Serial.println("[HTTP] POST /api/displaycolors/preview - toggling off");
            cancelColorPreview();
            display.showResting();
            server.send(200, "application/json", "{\"success\":true,\"active\":false}");
        } else {
            Serial.println("[HTTP] POST /api/displaycolors/preview - starting");
            display.showDemo();
            requestColorPreviewHold(2200);
            server.send(200, "application/json", "{\"success\":true,\"active\":true}");
        }
    });
    server.on("/api/displaycolors/clear", HTTP_POST, [this]() { 
        Serial.println("[HTTP] POST /api/displaycolors/clear - returning to scanning");
        cancelColorPreview();
        display.showResting();  // Return to normal scanning state
        server.send(200, "application/json", "{\"success\":true,\"active\":false}");
    });
    
    // Debug API routes (performance metrics and event ring)
    server.on("/api/debug/metrics", HTTP_GET, [this]() { handleDebugMetrics(); });
    server.on("/api/debug/events", HTTP_GET, [this]() { handleDebugEvents(); });
    server.on("/api/debug/events/clear", HTTP_POST, [this]() { handleDebugEventsClear(); });
    server.on("/api/debug/enable", HTTP_POST, [this]() { handleDebugEnable(); });
    
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
}

String WiFiManager::getAPIPAddress() const {
    if (setupModeState == SETUP_MODE_AP_ON) {
        return WiFi.softAPIP().toString();
    }
    return "";
}

void WiFiManager::handleStatus() {
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
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// ==================== API Endpoints ====================

void WiFiManager::handleApiProfilePush() {
    // Queue profile push action (non-blocking)
    // This endpoint triggers the push executor to apply active profile
    
    if (!checkRateLimit()) return;
    
    // Check if V1 is connected
    if (!bleClient.isConnected()) {
        server.send(503, "application/json", 
                   "{\"ok\":false,\"error\":\"V1 not connected\"}");
        return;
    }
    
    // Profile push is handled by main loop's processAutoPush() state machine
    Serial.println("[API] Profile push requested");
    
    JsonDocument doc;
    doc["ok"] = true;
    doc["message"] = "Profile push queued - check display for progress";
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
    
    // Note: Actual push execution is handled by main.cpp's processAutoPush()
    // via the startAutoPush() mechanism, not directly in this handler
}

void WiFiManager::handleSettingsApi() {
    const V1Settings& settings = settingsManager.get();
    
    JsonDocument doc;
    doc["ap_ssid"] = settings.apSSID;
    doc["ap_password"] = "********";  // Don't send actual password
    doc["proxy_ble"] = settings.proxyBLE;
    doc["proxy_name"] = settings.proxyName;
    doc["displayStyle"] = static_cast<int>(settings.displayStyle);
    doc["priorityArrowOnly"] = settings.priorityArrowOnly;
    
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
            server.send(400, "text/plain", "AP SSID required and password must be at least 8 characters");
            return;
        }
        settingsManager.updateAPCredentials(apSsid, apPass);
    }
    
    if (server.hasArg("brightness")) {
        int brightness = server.arg("brightness").toInt();
        brightness = std::max(0, std::min(brightness, 255));
        settingsManager.updateBrightness((uint8_t)brightness);
    }
    
    if (server.hasArg("color_theme")) {
        int theme = server.arg("color_theme").toInt();
        theme = std::max(0, std::min(theme, 2));  // Clamp to valid theme range
        settingsManager.updateColorTheme(static_cast<ColorTheme>(theme));
    }

    // BLE proxy settings
    if (server.hasArg("proxy_ble")) {
        bool proxyEnabled = server.arg("proxy_ble") == "true" || server.arg("proxy_ble") == "1";
        settingsManager.setProxyBLE(proxyEnabled);
    }
    if (server.hasArg("proxy_name")) {
        settingsManager.setProxyName(server.arg("proxy_name"));
    }

    // Display style setting
    if (server.hasArg("displayStyle")) {
        int style = server.arg("displayStyle").toInt();
        style = std::max(0, std::min(style, 1));  // Clamp to valid range (0=Classic, 1=Modern)
        settingsManager.updateDisplayStyle(static_cast<DisplayStyle>(style));
    }
    
    // Priority arrow only setting
    if (server.hasArg("priorityArrowOnly")) {
        bool prioOnly = server.arg("priorityArrowOnly") == "true" || server.arg("priorityArrowOnly") == "1";
        settingsManager.setPriorityArrowOnly(prioOnly);
    }
    
    // All changes are queued in the settingsManager instance. Now, save them all at once.
    Serial.println("--- Calling settingsManager.save() ---");
    settingsManager.save();
    
    // The settingsManager instance is already up-to-date, no need to reload.
    // We can directly apply any changes that need to take immediate effect.
    if (server.hasArg("color_theme")) {
        display.updateColorTheme();
        Serial.println("Display color theme updated");
    }
    
    server.sendHeader("Location", "/settings?saved=1");
    server.send(302);
}

void WiFiManager::handleDarkMode() {
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
    deserializeJson(doc, body);
    
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
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void WiFiManager::handleAutoPushSlotSave() {
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
    
    settingsManager.setSlot(slot, profile, static_cast<V1Mode>(mode));
    
    // If this is the currently active slot, update the display immediately
    if (slot == settingsManager.get().activeSlot) {
        display.drawProfileIndicator(slot);
    }
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleAutoPushActivate() {
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
    
    bleClient.setDisplayOn(profile.displayOn);
    
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
    
    // Handle WiFi icon color separately if provided
    if (server.hasArg("wifiIcon")) {
        uint16_t wifiIcon = server.arg("wifiIcon").toInt();
        settingsManager.setWiFiIconColor(wifiIcon);
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
    
    // Handle persisted color if provided
    if (server.hasArg("persisted")) {
        uint16_t persistedColor = server.arg("persisted").toInt();
        settingsManager.setPersistedColor(persistedColor);
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
    if (server.hasArg("hideBleIcon")) {
        settingsManager.setHideBleIcon(server.arg("hideBleIcon") == "true" || server.arg("hideBleIcon") == "1");
    }

    // Persist all color/visibility changes
    settingsManager.save();
    
    // Trigger immediate display preview to show new colors
    display.showDemo();
    requestColorPreviewHold(2200);  // Hold ~2.2s and cycle bands during preview
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleDisplayColorsReset() {
    // Reset to default colors: Bogey/Freq=Red, Front/Side/Rear=Red, L/K=Blue, Ka=Red, X=Green, WiFi=Cyan
    settingsManager.setDisplayColors(0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0x001F, 0xF800, 0x001F, 0x07E0);
    settingsManager.setWiFiIconColor(0x07FF);  // Cyan
    // Reset bar colors: Green, Green, Yellow, Yellow, Red, Red
    settingsManager.setSignalBarColors(0x07E0, 0x07E0, 0xFFE0, 0xFFE0, 0xF800, 0xF800);
    // Reset muted color to default dark grey
    settingsManager.setMutedColor(0x3186);
    // Reset persisted color to darker grey
    settingsManager.setPersistedColor(0x18C3);
    
    // Trigger immediate display preview to show reset colors
    display.showDemo();
    requestColorPreviewHold(2200);
    
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
    doc["wifiIcon"] = s.colorWiFiIcon;
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
    doc["hideWifiIcon"] = s.hideWifiIcon;
    doc["hideProfileIndicator"] = s.hideProfileIndicator;
    doc["hideBatteryIcon"] = s.hideBatteryIcon;
    doc["hideBleIcon"] = s.hideBleIcon;
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// ============= Debug API Handlers =============

void WiFiManager::handleDebugMetrics() {
    String json = perfMetricsToJson();
    server.send(200, "application/json", json);
}

void WiFiManager::handleDebugEvents() {
    String json = eventRingToJson();
    server.send(200, "application/json", json);
}

void WiFiManager::handleDebugEventsClear() {
    eventRingClear();
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleDebugEnable() {
    bool enable = true;
    if (server.hasArg("enable")) {
        enable = (server.arg("enable") == "true" || server.arg("enable") == "1");
    }
    perfMetricsSetDebug(enable);
    server.send(200, "application/json", "{\"success\":true,\"debugEnabled\":" + String(enable ? "true" : "false") + "}");
}

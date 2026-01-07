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

bool WiFiManager::startSetupMode() {
    // Always-on AP; idempotent start
    if (setupModeState == SETUP_MODE_AP_ON) {
        Serial.println("[SetupMode] Already active");
        return true;
    }

    Serial.println("[SetupMode] Starting AP (always-on mode)...");
    setupModeStartTime = millis();

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
    Serial.println("[SetupMode] AP will remain on (no timeout)");

    return true;
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
    // Dump LittleFS root for diagnostics
    dumpLittleFSRoot();
    
    // New UI served from LittleFS
    // Redirect /ui to root for backward compatibility
    server.on("/ui", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    
    // Serve static assets from _app directory
    server.on("/_app/env.js", HTTP_GET, [this]() { serveLittleFSFile("/_app/env.js", "application/javascript"); });
    server.on("/_app/version.json", HTTP_GET, [this]() { serveLittleFSFile("/_app/version.json", "application/json"); });
    
    // Root tries to serve /index.html (Svelte), falls back to inline failsafe dashboard
    server.on("/", HTTP_GET, [this]() { 
        markUiActivity();  // Track UI activity
        // Try Svelte index.html first
        if (serveLittleFSFile("/index.html", "text/html")) {
            Serial.printf("[HTTP] 200 / -> /index.html\n");
            return;
        }
        // Fall back to inline dashboard
        Serial.println("[HTTP] / -> inline failsafe dashboard");
        handleFailsafeUI(); 
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
    
    // V1 Device Cache routes (fast reconnect)
    server.on("/api/v1/devices", HTTP_GET, [this]() { handleV1DevicesApi(); });
    server.on("/api/v1/devices/name", HTTP_POST, [this]() { handleV1DeviceNameSave(); });
    server.on("/api/v1/devices/profile", HTTP_POST, [this]() { handleV1DeviceProfileSave(); });
    server.on("/api/v1/devices/delete", HTTP_POST, [this]() { handleV1DeviceDelete(); });
    
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

void WiFiManager::process() {
    if (setupModeState != SETUP_MODE_AP_ON) {
        return;  // No WiFi processing when Setup Mode is off
    }
    
    // Handle web requests
    server.handleClient();
}

String WiFiManager::getAPIPAddress() const {
    if (setupModeState == SETUP_MODE_AP_ON) {
        return WiFi.softAPIP().toString();
    }
    return "";
}

void WiFiManager::handleStatus() {
    const V1Settings& settings = settingsManager.get();
    
    String json = "{";
    // WiFi info (matches Svelte dashboard expectations)
    bool staConnected = false;  // AP-only
    long rssi = 0;
    IPAddress staIp;  // empty
    json += "\"wifi\":{";
    json += "\"setup_mode\":" + String(setupModeState == SETUP_MODE_AP_ON ? "true" : "false") + ",";
    json += "\"ap_active\":" + String(setupModeState == SETUP_MODE_AP_ON ? "true" : "false") + ",";
    json += "\"sta_connected\":" + String(staConnected ? "true" : "false") + ",";
    json += "\"sta_ip\":\"" + staIp.toString() + "\",";
    json += "\"ap_ip\":\"" + getAPIPAddress() + "\",";
    json += "\"ssid\":\"" + settings.apSSID + "\",";
    json += "\"rssi\":" + String(rssi);
    json += "},";
    
    // Device info
    json += "\"device\":{";
    json += "\"uptime\":" + String(millis() / 1000) + ",";
    json += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"hostname\":\"v1g2\"";
    json += "},";
    
    // BLE/V1 connection state
    json += "\"v1_connected\":" + String(bleClient.isConnected() ? "true" : "false");
    
    if (getStatusJson) {
        json += "," + getStatusJson();  // append additional status if provided
    }
    
    // Add alert info if callback is set
    if (getAlertJson) {
        json += ",\"alert\":" + getAlertJson();
    }
    
    json += "}";
    
    server.send(200, "application/json", json);
}

// ==================== Failsafe API Endpoints (PHASE A) ====================

void WiFiManager::handleFailsafeUI() {
    // Minimal always-on dashboard
    String html = "<!DOCTYPE html><html>";
    html += "<head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>V1 Gen2 AP</title>";
    html += "<style>";
    html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0f172a; color: #e2e8f0; margin:0; padding:24px; }";
    html += ".card { max-width: 520px; margin: 0 auto; background:#111827; border:1px solid #1f2937; border-radius:14px; padding:24px; box-shadow:0 12px 30px rgba(0,0,0,0.35); }";
    html += "h1 { font-size: 22px; margin: 0 0 12px; letter-spacing:0.3px; }";
    html += "p { margin: 6px 0; color:#cbd5e1; }";
    html += ".pill { display:inline-block; padding:6px 10px; border-radius:999px; background:#0ea5e9; color:#0b1220; font-weight:700; font-size:12px; letter-spacing:0.3px; }";
    html += ".grid { display:grid; grid-template-columns: repeat(auto-fit, minmax(160px,1fr)); gap:12px; margin-top:16px; }";
    html += ".tile { background:#0b1220; border:1px solid #1f2937; border-radius:10px; padding:12px; }";
    html += ".label { color:#94a3b8; font-size:12px; text-transform:uppercase; letter-spacing:0.4px; }";
    html += ".value { color:#e2e8f0; font-size:16px; font-weight:700; margin-top:4px; }";
    html += ".actions { margin-top:16px; display:flex; gap:10px; flex-wrap:wrap; }";
    html += ".btn { padding:10px 14px; border:none; border-radius:10px; background:#0ea5e9; color:#0b1220; font-weight:700; cursor:pointer; box-shadow:0 8px 18px rgba(14,165,233,0.35); }";
    html += ".btn.secondary { background:#1f2937; color:#e2e8f0; box-shadow:none; }";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='card'>";
    html += "<div style='display:flex; align-items:center; justify-content:space-between;'>";
    html += "<h1>V1 Gen2 Access Point</h1><span class='pill'>Always On</span>";
    html += "</div>";
    html += "<p>Connect to <strong>V1-Simple</strong> at <strong>192.168.35.5</strong>. The web UI and APIs remain available; no session timeout.</p>";
    html += "<div class='grid'>";
    html += "  <div class='tile'><div class='label'>BLE</div><div class='value'>";
    html += bleClient.isConnected() ? "Connected" : "Disconnected";
    html += "</div></div>";
    html += "  <div class='tile'><div class='label'>AP IP</div><div class='value'>" + getAPIPAddress() + "</div></div>";
    html += "  <div class='tile'><div class='label'>Heap Free</div><div class='value'>" + String(ESP.getFreeHeap() / 1024) + " KB</div></div>";
    html += "</div>";
    html += "<div class='actions'>";
    html += "  <button class='btn secondary' onclick=\"fetch('/darkmode?state=1',{method:'POST'}).then(r=>r.json()).then(d=>alert('Dark mode: '+(d.darkMode?'ON':'OFF')));\">Toggle Dark Mode</button>";
    html += "  <button class='btn secondary' onclick=\"fetch('/mute?state=1',{method:'POST'}).then(r=>r.json()).then(d=>alert('Mute: '+(d.muted?'ON':'OFF')));\">Toggle Mute</button>";
    html += "</div>";
    html += "</div>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
}

void WiFiManager::handleApiStatus() {
    // Enhanced status endpoint for failsafe UI
    // Returns BLE state, proxy metrics, heap, and AP status
    
    const V1Settings& settings = settingsManager.get();
    
    String json = "{";
    
    // BLE state
    json += "\"ble_state\":\"" + String(bleStateToString(bleClient.getBLEState())) + "\",";
    json += "\"ble_connected\":" + String(bleClient.isConnected() ? "true" : "false") + ",";
    
    // Proxy metrics
    const ProxyMetrics& pm = bleClient.getProxyMetrics();
    json += "\"proxy_connected\":" + String(bleClient.isProxyClientConnected() ? "true" : "false") + ",";
    
    // Calculate sends per second
    unsigned long uptime = (millis() - pm.lastResetMs) / 1000;
    uint32_t sendsPerSec = (uptime > 0) ? (pm.sendCount / uptime) : 0;
    json += "\"proxy_sends_per_sec\":" + String(sendsPerSec) + ",";
    json += "\"proxy_queue_hw\":" + String(pm.queueHighWater) + ",";
    json += "\"proxy_drops\":" + String(pm.dropCount) + ",";
    
    // Heap
    json += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"heap_min\":" + String(ESP.getMinFreeHeap()) + ",";
    
    // WiFi state (AP-only)
    json += "\"setup_mode\":" + String(setupModeState == SETUP_MODE_AP_ON ? "true" : "false") + ",";
    
    // Uptime
    json += "\"uptime_sec\":" + String(millis() / 1000);
    
    json += "}";
    
    server.send(200, "application/json", json);
}

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
    
    String json = "{";
    json += "\"ok\":true,";
    json += "\"message\":\"Profile push queued - check display for progress\"";
    json += "}";
    
    server.send(200, "application/json", json);
    
    // Note: Actual push execution is handled by main.cpp's processAutoPush()
    // via the startAutoPush() mechanism, not directly in this handler
}

void WiFiManager::handleSettingsApi() {
    const V1Settings& settings = settingsManager.get();
    
    String json = "{";
    json += "\"ap_ssid\":\"" + settings.apSSID + "\",";
    json += "\"ap_password\":\"********\",";  // Don't send actual password
    json += "\"proxy_ble\":" + String(settings.proxyBLE ? "true" : "false") + ",";
    json += "\"proxy_name\":\"" + settings.proxyName + "\"";
    json += "}";
    
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
    
    // Force AP-only mode
    settingsManager.updateWiFiMode(V1_WIFI_AP);
    
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
    
    String json = "{\"success\":" + String(success ? "true" : "false") + 
                  ",\"darkMode\":" + String(darkMode ? "true" : "false") + "}";
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
    
    String json = "{\"success\":" + String(success ? "true" : "false") + 
                  ",\"muted\":" + String(muted ? "true" : "false") + "}";
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
    String json = "{";
    json += "\"connected\":" + String(bleClient.isConnected() ? "true" : "false");
    
    if (!v1ProfileManager.hasCurrentSettings()) {
        json += ",\"available\":false}";
        server.send(200, "application/json", json);
        return;
    }
    
    json += ",\"available\":true,\"settings\":";
    json += v1ProfileManager.settingsToJson(v1ProfileManager.getCurrentSettings());
    json += "}";
    server.send(200, "application/json", json);
}

void WiFiManager::handleV1DevicesApi() {
    if (!getFilesystem) {
        server.send(503, "application/json", "{\"error\":\"Filesystem not available\"}");
        return;
    }
    
    fs::FS* fs = getFilesystem();
    if (!fs) {
        server.send(503, "application/json", "{\"error\":\"SD card not ready\"}");
        return;
    }
    
    // Pre-load profiles into a map for efficient lookup
    std::vector<std::pair<String, int>> profileMap;
    File profileFile = fs->open("/known_v1_profiles.txt", FILE_READ);
    if (profileFile) {
        while (profileFile.available()) {
            String line = profileFile.readStringUntil('\n');
            line.trim();
            int sep = line.indexOf('|');
            if (sep > 0) {
                String addr = line.substring(0, sep);
                int profile = line.substring(sep + 1).toInt();
                profileMap.push_back({addr, profile});
            }
        }
        profileFile.close();
    }
    
    String json = "{\"devices\":[";
    
    // Read known_v1.txt for addresses
    File addrFile = fs->open("/known_v1.txt", FILE_READ);
    if (addrFile) {
        bool first = true;
        while (addrFile.available()) {
            String addr = addrFile.readStringUntil('\n');
            addr.trim();
            if (addr.length() == 17) {  // Valid MAC address format
                if (!first) json += ",";
                first = false;
                
                // Look for custom name in known_v1_names.txt
                String name = "";
                File nameFile = fs->open("/known_v1_names.txt", FILE_READ);
                if (nameFile) {
                    while (nameFile.available()) {
                        String line = nameFile.readStringUntil('\n');
                        line.trim();
                        int sep = line.indexOf('|');
                        if (sep > 0) {
                            String lineAddr = line.substring(0, sep);
                            if (lineAddr == addr) {
                                name = line.substring(sep + 1);
                                break;
                            }
                        }
                    }
                    nameFile.close();
                }
                
                // Look for default profile
                int defaultProfile = 0;
                for (const auto& pm : profileMap) {
                    if (pm.first == addr) {
                        defaultProfile = pm.second;
                        break;
                    }
                }
                
                json += "{\"address\":\"" + addr + "\",\"name\":\"" + name + "\",\"defaultProfile\":" + String(defaultProfile) + "}";
            }
        }
        addrFile.close();
    }
    
    json += "]}";
    server.send(200, "application/json", json);
}

void WiFiManager::handleV1DeviceNameSave() {
    if (!getFilesystem) {
        server.send(503, "application/json", "{\"error\":\"Filesystem not available\"}");
        return;
    }
    
    if (!server.hasArg("address") || !server.hasArg("name")) {
        server.send(400, "application/json", "{\"error\":\"Missing address or name\"}");
        return;
    }
    
    String address = server.arg("address");
    String name = server.arg("name");
    
    fs::FS* fs = getFilesystem();
    if (!fs) {
        server.send(503, "application/json", "{\"error\":\"SD card not ready\"}");
        return;
    }
    
    // Read existing names, update or add the new one
    std::vector<String> lines;
    bool found = false;
    
    File readFile = fs->open("/known_v1_names.txt", FILE_READ);
    if (readFile) {
        while (readFile.available()) {
            String line = readFile.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) {
                int sep = line.indexOf('|');
                if (sep > 0 && line.substring(0, sep) == address) {
                    // Update existing entry
                    if (name.length() > 0) {
                        lines.push_back(address + "|" + name);
                    }
                    // If name is empty, we skip adding it (delete)
                    found = true;
                } else {
                    lines.push_back(line);
                }
            }
        }
        readFile.close();
    }
    
    // Add new entry if not found and name is not empty
    if (!found && name.length() > 0) {
        lines.push_back(address + "|" + name);
    }
    
    // Write back
    File writeFile = fs->open("/known_v1_names.txt", FILE_WRITE);
    if (writeFile) {
        for (const auto& line : lines) {
            writeFile.println(line);
        }
        writeFile.close();
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to write file\"}");
    }
}

void WiFiManager::handleV1DeviceDelete() {
    if (!getFilesystem) {
        server.send(503, "application/json", "{\"error\":\"Filesystem not available\"}");
        return;
    }
    
    if (!server.hasArg("address")) {
        server.send(400, "application/json", "{\"error\":\"Missing address\"}");
        return;
    }
    
    String address = server.arg("address");
    
    fs::FS* fs = getFilesystem();
    if (!fs) {
        server.send(503, "application/json", "{\"error\":\"SD card not ready\"}");
        return;
    }
    
    // Remove from known_v1.txt
    std::vector<String> addresses;
    File readFile = fs->open("/known_v1.txt", FILE_READ);
    if (readFile) {
        while (readFile.available()) {
            String line = readFile.readStringUntil('\n');
            line.trim();
            if (line.length() > 0 && line != address) {
                addresses.push_back(line);
            }
        }
        readFile.close();
    }
    
    File writeFile = fs->open("/known_v1.txt", FILE_WRITE);
    if (writeFile) {
        for (const auto& addr : addresses) {
            writeFile.println(addr);
        }
        writeFile.close();
    }
    
    // Also remove from names file
    std::vector<String> names;
    readFile = fs->open("/known_v1_names.txt", FILE_READ);
    if (readFile) {
        while (readFile.available()) {
            String line = readFile.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) {
                int sep = line.indexOf('|');
                if (sep > 0 && line.substring(0, sep) != address) {
                    names.push_back(line);
                }
            }
        }
        readFile.close();
    }
    
    writeFile = fs->open("/known_v1_names.txt", FILE_WRITE);
    if (writeFile) {
        for (const auto& n : names) {
            writeFile.println(n);
        }
        writeFile.close();
    }
    
    // Also remove from profiles file
    std::vector<String> profiles;
    readFile = fs->open("/known_v1_profiles.txt", FILE_READ);
    if (readFile) {
        while (readFile.available()) {
            String line = readFile.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) {
                int sep = line.indexOf('|');
                if (sep > 0 && line.substring(0, sep) != address) {
                    profiles.push_back(line);
                }
            }
        }
        readFile.close();
    }
    
    writeFile = fs->open("/known_v1_profiles.txt", FILE_WRITE);
    if (writeFile) {
        for (const auto& p : profiles) {
            writeFile.println(p);
        }
        writeFile.close();
    }
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleV1DeviceProfileSave() {
    if (!getFilesystem) {
        server.send(503, "application/json", "{\"error\":\"Filesystem not available\"}");
        return;
    }
    
    if (!server.hasArg("address") || !server.hasArg("profile")) {
        server.send(400, "application/json", "{\"error\":\"Missing address or profile\"}");
        return;
    }
    
    String address = server.arg("address");
    int profile = server.arg("profile").toInt();
    
    fs::FS* fs = getFilesystem();
    if (!fs) {
        server.send(503, "application/json", "{\"error\":\"SD card not ready\"}");
        return;
    }
    
    // Read existing profiles, update or add the new one
    std::vector<String> lines;
    bool found = false;
    
    File readFile = fs->open("/known_v1_profiles.txt", FILE_READ);
    if (readFile) {
        while (readFile.available()) {
            String line = readFile.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) {
                int sep = line.indexOf('|');
                if (sep > 0 && line.substring(0, sep) == address) {
                    // Update existing entry
                    if (profile > 0) {
                        lines.push_back(address + "|" + String(profile));
                    }
                    // If profile is 0, we skip adding it (delete/none)
                    found = true;
                } else {
                    lines.push_back(line);
                }
            }
        }
        readFile.close();
    }
    
    // Add new entry if not found and profile is not 0
    if (!found && profile > 0) {
        lines.push_back(address + "|" + String(profile));
    }
    
    // Write back
    File writeFile = fs->open("/known_v1_profiles.txt", FILE_WRITE);
    if (writeFile) {
        for (const auto& line : lines) {
            writeFile.println(line);
        }
        writeFile.close();
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to write file\"}");
    }
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
    
    String json = "{";
    json += "\"enabled\":" + String(s.autoPushEnabled ? "true" : "false") + ",";
    json += "\"activeSlot\":" + String(s.activeSlot) + ",";
    json += "\"slots\":[";
    
    // Slot 0
    json += "{\"name\":\"" + s.slot0Name + "\",";
    json += "\"profile\":\"" + s.slot0_default.profileName + "\",";
    json += "\"mode\":" + String(s.slot0_default.mode) + ",";
    json += "\"color\":" + String(s.slot0Color) + ",";
    json += "\"volume\":" + String(s.slot0Volume) + ",";
    json += "\"muteVolume\":" + String(s.slot0MuteVolume) + ",";
    json += "\"darkMode\":" + String(s.slot0DarkMode ? "true" : "false") + ",";
    json += "\"muteToZero\":" + String(s.slot0MuteToZero ? "true" : "false") + ",";
    json += "\"alertPersist\":" + String(s.slot0AlertPersist) + "},";
    
    // Slot 1
    json += "{\"name\":\"" + s.slot1Name + "\",";
    json += "\"profile\":\"" + s.slot1_highway.profileName + "\",";
    json += "\"mode\":" + String(s.slot1_highway.mode) + ",";
    json += "\"color\":" + String(s.slot1Color) + ",";
    json += "\"volume\":" + String(s.slot1Volume) + ",";
    json += "\"muteVolume\":" + String(s.slot1MuteVolume) + ",";
    json += "\"darkMode\":" + String(s.slot1DarkMode ? "true" : "false") + ",";
    json += "\"muteToZero\":" + String(s.slot1MuteToZero ? "true" : "false") + ",";
    json += "\"alertPersist\":" + String(s.slot1AlertPersist) + "},";
    
    // Slot 2
    json += "{\"name\":\"" + s.slot2Name + "\",";
    json += "\"profile\":\"" + s.slot2_comfort.profileName + "\",";
    json += "\"mode\":" + String(s.slot2_comfort.mode) + ",";
    json += "\"color\":" + String(s.slot2Color) + ",";
    json += "\"volume\":" + String(s.slot2Volume) + ",";
    json += "\"muteVolume\":" + String(s.slot2MuteVolume) + ",";
    json += "\"darkMode\":" + String(s.slot2DarkMode ? "true" : "false") + ",";
    json += "\"muteToZero\":" + String(s.slot2MuteToZero ? "true" : "false") + ",";
    json += "\"alertPersist\":" + String(s.slot2AlertPersist) + "}";
    
    json += "]}";
    
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
    
    // Trigger immediate display preview to show reset colors
    display.showDemo();
    requestColorPreviewHold(2200);
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleDisplayColorsApi() {
    const V1Settings& s = settingsManager.get();
    
    String json = "{";
    json += "\"bogey\":" + String(s.colorBogey) + ",";
    json += "\"freq\":" + String(s.colorFrequency) + ",";
    json += "\"arrowFront\":" + String(s.colorArrowFront) + ",";
    json += "\"arrowSide\":" + String(s.colorArrowSide) + ",";
    json += "\"arrowRear\":" + String(s.colorArrowRear) + ",";
    json += "\"bandL\":" + String(s.colorBandL) + ",";
    json += "\"bandKa\":" + String(s.colorBandKa) + ",";
    json += "\"bandK\":" + String(s.colorBandK) + ",";
    json += "\"bandX\":" + String(s.colorBandX) + ",";
    json += "\"wifiIcon\":" + String(s.colorWiFiIcon) + ",";
    json += "\"bleConnected\":" + String(s.colorBleConnected) + ",";
    json += "\"bleDisconnected\":" + String(s.colorBleDisconnected) + ",";
    json += "\"bar1\":" + String(s.colorBar1) + ",";
    json += "\"bar2\":" + String(s.colorBar2) + ",";
    json += "\"bar3\":" + String(s.colorBar3) + ",";
    json += "\"bar4\":" + String(s.colorBar4) + ",";
    json += "\"bar5\":" + String(s.colorBar5) + ",";
    json += "\"bar6\":" + String(s.colorBar6) + ",";
    json += "\"hideWifiIcon\":" + String(s.hideWifiIcon ? "true" : "false") + ",";
    json += "\"hideProfileIndicator\":" + String(s.hideProfileIndicator ? "true" : "false") + ",";
    json += "\"hideBatteryIcon\":" + String(s.hideBatteryIcon ? "true" : "false") + ",";
    json += "\"hideBleIcon\":" + String(s.hideBleIcon ? "true" : "false");
    json += "}";
    
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

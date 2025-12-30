/**
 * WiFi Manager for V1 Gen2 Display
 * Handles WiFi AP/STA modes and web server
 */

#include "wifi_manager.h"
#include "settings.h"
#include "display.h"
#include "alert_logger.h"
#include "alert_db.h"
#include "serial_logger.h"
#include "v1_profiles.h"
#include "ble_client.h"
#include "time_manager.h"
#include "../include/config.h"
#include "../include/color_themes.h"
#include <algorithm>
#include <ArduinoJson.h>
#include "esp_netif.h"
#include <ESPmDNS.h>
#include <LittleFS.h>

// External BLE client for V1 commands
extern V1BLEClient bleClient;

// HTML entity encoding for safe HTML generation
static String htmlEscape(const String& input) {
    String output;
    output.reserve(input.length() + 20);  // Reserve extra space for escape sequences
    for (size_t i = 0; i < input.length(); i++) {
        char c = input.charAt(i);
        switch (c) {
            case '&':  output += "&amp;";  break;
            case '<':  output += "&lt;";   break;
            case '>':  output += "&gt;";   break;
            case '"':  output += "&quot;"; break;
            case '\'': output += "&#39;";  break;
            default:   output += c;        break;
        }
    }
    return output;
}

// JSON string escaping
static String jsonEscape(const String& input) {
    String output;
    output.reserve(input.length() + 10);
    for (size_t i = 0; i < input.length(); i++) {
        char c = input.charAt(i);
        switch (c) {
            case '"':  output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n";  break;
            case '\r': output += "\\r";  break;
            case '\t': output += "\\t";  break;
            default:
                if (c < 0x20) {
                    // Skip other control characters
                } else {
                    output += c;
                }
                break;
        }
    }
    return output;
}

// CSV split helper for fixed column count
namespace {
bool splitCsvFixed(const String& line, String parts[], size_t expected) {
    int start = 0;
    size_t idx = 0;
    while (idx < expected) {
        int sep = line.indexOf(',', start);
        if (sep == -1) {
            parts[idx++] = line.substring(start);
            break;
        }
        parts[idx++] = line.substring(start, sep);
        start = sep + 1;
    }
    return idx == expected;
}
} // namespace

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
        return false;
    }
    server.sendHeader("Cache-Control", "max-age=86400");
    server.streamFile(file, contentType);
    file.close();
    return true;
}

// Global instance
WiFiManager wifiManager;

WiFiManager::WiFiManager() : server(80), apActive(false), staConnected(false), staEnabledByConfig(false), natEnabled(false), lastStaRetry(0), timeInitialized(false) {
}

bool WiFiManager::begin() {
    const V1Settings& settings = settingsManager.get();
    
    if (!settings.enableWifi) {
        SerialLog.println("WiFi disabled in settings");
        return false;
    }
    
    SerialLog.println("Starting WiFi...");

    bool wantAP = settings.wifiMode == V1_WIFI_AP || settings.wifiMode == V1_WIFI_APSTA;
    bool wantSTA = settings.wifiMode == V1_WIFI_STA || settings.wifiMode == V1_WIFI_APSTA;
    staEnabledByConfig = wantSTA;  // Track if STA is enabled by config

    // If networks are configured but STA is disabled, auto-enable STA alongside AP
    int networksConfigured = 0;
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (settings.wifiNetworks[i].isValid()) networksConfigured++;
    }
    if (networksConfigured == 0 && settings.staSSID.length() > 0) networksConfigured++;
    if (!wantSTA && networksConfigured > 0) {
        SerialLog.println("[WiFi] STA disabled but networks configured; enabling AP+STA for connectivity");
        wantSTA = true;
        wantAP = true;
        staEnabledByConfig = true;
    }

    if (wantAP && wantSTA) {
        WiFi.mode(WIFI_AP_STA);
    } else if (wantAP) {
        WiFi.mode(WIFI_AP);
    } else {
        WiFi.mode(WIFI_STA);
    }

    if (wantAP) {
        setupAP();
    }
    
    if (wantSTA) {
        setupSTA();
    } else {
        SerialLog.println("[WiFi] STA mode disabled in settings");
    }
    
    setupWebServer();
    
    // Collect Accept-Encoding header for GZIP support
    const char* headerKeys[] = {"Accept-Encoding"};
    server.collectHeaders(headerKeys, 1);
    
    server.begin();
    SerialLog.println("Web server started on port 80");
    
    return true;
}

void WiFiManager::setupAP() {
    const V1Settings& settings = settingsManager.get();
    bool wantSTA = settings.wifiMode == V1_WIFI_APSTA;
    
    String apSSID = settings.apSSID.length() > 0 ? settings.apSSID : "V1-Display";
    String apPass = settings.apPassword.length() >= 8 ? settings.apPassword : "valentine1";
    
    SerialLog.printf("Starting AP: %s\n", apSSID.c_str());
    
    // Ensure correct mode for AP operation
    WiFi.mode(wantSTA ? WIFI_AP_STA : WIFI_AP);
    
    // Configure AP IP BEFORE starting softAP
    IPAddress apIP(192, 168, 35, 5);      // AP IP address for web UI access
    IPAddress gateway(192, 168, 35, 5);   // Gateway = AP IP so clients route through us
    IPAddress subnet(255, 255, 255, 0);
    
    // Configure IP address first (must be before softAP)
    // Note: softAPConfig args are (local_ip, gateway, subnet, dhcp_lease_start, dns_server)
    // We must provide dhcp_lease_start if we want to provide dns_server
    IPAddress dhcpStart(192, 168, 35, 100);
    
    // Use the upstream DNS if available, otherwise fallback to Google
    IPAddress dns = WiFi.dnsIP(0);
    if (dns == IPAddress(0,0,0,0)) dns = IPAddress(8,8,8,8);
    
    if (!WiFi.softAPConfig(apIP, gateway, subnet, dhcpStart, dns)) {
        SerialLog.println("[WiFi] softAPConfig failed!");
    }
    
    // Now start the AP
    if (!WiFi.softAP(apSSID.c_str(), apPass.c_str())) {
        SerialLog.println("[WiFi] softAP failed!");
    }
    
    apActive = true;
    
    IPAddress ip = WiFi.softAPIP();
    SerialLog.printf("AP IP address: %s\n", ip.toString().c_str());
    SerialLog.printf("AP Gateway: %s\n", gateway.toString().c_str());
    SerialLog.printf("AP Subnet: %s\n", subnet.toString().c_str());
    
    // Ensure DHCP server is properly configured for AP
    // Get AP network interface and verify DHCP
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        // Note: WiFi.softAPConfig already configured DHCP with DNS
        // We just verify it's running
        esp_netif_dhcp_status_t status;
        esp_netif_dhcps_get_status(ap_netif, &status);
        if (status == ESP_NETIF_DHCP_STOPPED) {
             esp_netif_dhcps_start(ap_netif);
             SerialLog.println("[WiFi] AP DHCP server started (was stopped)");
        } else {
             SerialLog.println("[WiFi] AP DHCP server is running");
        }
    } else {
        SerialLog.println("[WiFi] WARNING: Could not get AP netif for DHCP");
    }
}

void WiFiManager::setupSTA() {
    SerialLog.println("[WiFi] Setting up STA mode...");
    int networksAdded = populateStaNetworks();
    if (networksAdded == 0) {
        SerialLog.println("[WiFi] No networks configured for STA");
        return;
    }
    SerialLog.printf("[WiFi] Added %d network(s) to WiFiMulti\n", networksAdded);
}

int WiFiManager::populateStaNetworks() {
    wifiMulti = WiFiMulti();  // Reset network list before re-populating
    const V1Settings& settings = settingsManager.get();
    int networksAdded = 0;

    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (settings.wifiNetworks[i].isValid()) {
            wifiMulti.addAP(settings.wifiNetworks[i].ssid.c_str(), 
                          settings.wifiNetworks[i].password.c_str());
            SerialLog.printf("[WiFi] Added network %d: %s\n", i, settings.wifiNetworks[i].ssid.c_str());
            networksAdded++;
        }
    }

    if (networksAdded == 0 && settings.staSSID.length() > 0) {
        wifiMulti.addAP(settings.staSSID.c_str(), settings.staPassword.c_str());
        SerialLog.printf("[WiFi] Added legacy network: %s\n", settings.staSSID.c_str());
        networksAdded++;
    }

    return networksAdded;
}

void WiFiManager::checkSTAConnection() {
    
    if (!staEnabledByConfig) {
        if (staConnected) {
            staConnected = false;
            natEnabled = false;
            connectedSSID = "";
        }
        return;
    }
    
    // Try connecting every STA_CONNECTION_RETRY_INTERVAL_MS
    if (millis() - lastStaRetry < STA_CONNECTION_RETRY_INTERVAL_MS) {
        return;
    }
    lastStaRetry = millis();

    if (wifiMulti.run(STA_CONNECTION_TIMEOUT_MS) == WL_CONNECTED) {
        if (!staConnected) {
            staConnected = true;
            connectedSSID = WiFi.SSID();
            SerialLog.println("\n=== WiFi Connected ===");
            SerialLog.printf("SSID: %s\n", connectedSSID.c_str());
            SerialLog.printf("IP: %s\n", WiFi.localIP().toString().c_str());
            SerialLog.printf("Signal: %d dBm\n", WiFi.RSSI());
            
            // Start mDNS so device is accessible as v1g2.local from network
            if (MDNS.begin("v1g2")) {
                MDNS.addService("http", "tcp", 80);
                SerialLog.println("mDNS: http://v1g2.local");
            }
            
            if (apActive) {
                enableNAT();
            }
            
            if (!timeInitialized) {
                initializeTime();
            }
        }
    } else {
        if (staConnected) {
            SerialLog.println("[WiFi] Disconnected");
            staConnected = false;
            connectedSSID = "";
        }
    }
}

void WiFiManager::initializeTime() {
    SerialLog.println("Initializing NTP time sync...");
    // Configure NTP with UTC timezone (0 offset, 0 DST)
    // All times stored and displayed in UTC
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    
    // Wait briefly for time to sync
    struct tm timeinfo;
    int retries = 0;
    while (!getLocalTime(&timeinfo) && retries < NTP_SYNC_RETRY_COUNT) {
        delay(NTP_SYNC_RETRY_DELAY_MS);
        retries++;
    }
    
    if (retries < NTP_SYNC_RETRY_COUNT) {
        SerialLog.println("Time synchronized via NTP!");
        SerialLog.printf("  Current time (UTC): %04d-%02d-%02dT%02d:%02d:%02dZ\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        timeInitialized = true;
        
        // Update alert loggers with real timestamp
        time_t now = time(nullptr);
        alertLogger.setTimestampUTC((uint32_t)now);
        alertDB.setTimestampUTC((uint32_t)now);
        SerialLog.printf("[WiFi] Alert timestamps set: %lu\n", (uint32_t)now);
    } else {
        SerialLog.println("Failed to sync time via NTP");
    }
}

void WiFiManager::enableNAT() {
    // NAT setup: Forward traffic from AP clients through STA connection
    
    if (natEnabled) {
        return;  // Already enabled
    }
    
    SerialLog.println("[WiFi] Enabling NAT/NAPT...");

    // 1. Enable NAPT on AP interface (The LAN interface)
    // This tells LwIP to perform NAT on packets arriving on this interface
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_err_t err = esp_netif_napt_enable(ap_netif);
        if (err == ESP_OK) {
            SerialLog.println("[WiFi] NAPT enabled on AP interface (LAN)");
        } else {
            SerialLog.printf("[WiFi] Failed to enable NAPT on AP: %d\n", err);
        }
    }

    // 2. Ensure IP Forwarding is enabled (should be by build flag)
    // The build flag CONFIG_LWIP_IP_FORWARD=1 handles this.
    
    natEnabled = true;
    SerialLog.printf("[WiFi] NAT: Enabled - STA IP: %s, AP IP: %s\n", 
                  WiFi.localIP().toString().c_str(), WiFi.softAPIP().toString().c_str());
    SerialLog.println("[WiFi] NAT: Clients should now have internet access");
}

void WiFiManager::setupWebServer() {
    // Initialize LittleFS for serving web UI files
    if (!LittleFS.begin(true)) {  // true = format if mount fails
        SerialLog.println("[WiFi] LittleFS mount failed!");
    } else {
        SerialLog.println("[WiFi] LittleFS mounted successfully");
        // List files in LittleFS for debugging
        File root = LittleFS.open("/");
        File file = root.openNextFile();
        while (file) {
            SerialLog.printf("[WiFi] LittleFS file: %s (%d bytes)\n", file.name(), file.size());
            file = root.openNextFile();
        }
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
    
    // Catch-all for _app/immutable/* files
    server.onNotFound([this]() {
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
    
    server.on("/", HTTP_GET, [this]() { serveLittleFSFile("/index.html", "text/html"); });  // Root serves new SvelteKit UI
    server.on("/status", HTTP_GET, [this]() { handleStatus(); });
    server.on("/api/status", HTTP_GET, [this]() { handleStatus(); });  // API version for new UI
    server.on("/api/settings", HTTP_GET, [this]() { handleSettingsApi(); });  // JSON settings for new UI
    server.on("/api/alerts", HTTP_GET, [this]() { handleLogsData(); });  // Alias for new UI
    server.on("/api/alerts/clear", HTTP_POST, [this]() { handleLogsClear(); });  // Alias for new UI
    
    // Legacy HTML page routes - redirect to root (SvelteKit handles routing)
    server.on("/settings", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    server.on("/settings", HTTP_POST, [this]() { handleSettingsSave(); });
    server.on("/time", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    server.on("/time", HTTP_POST, [this]() { handleTimeSettingsSave(); });
    server.on("/darkmode", HTTP_POST, [this]() { handleDarkMode(); });
    server.on("/mute", HTTP_POST, [this]() { handleMute(); });
    server.on("/logs", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    server.on("/api/logs", HTTP_GET, [this]() { handleLogsData(); });
    server.on("/api/logs/clear", HTTP_POST, [this]() { handleLogsClear(); });
    
    // Serial log endpoints for debugging
    server.on("/seriallog", HTTP_GET, [this]() { 
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "Redirecting to /");
    });
    server.on("/serial_log.txt", HTTP_GET, [this]() { handleSerialLog(); });
    server.on("/api/serial_log/clear", HTTP_POST, [this]() { handleSerialLogClear(); });

    // Lightweight health and captive-portal helpers
    server.on("/ping", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /ping");
        server.send(200, "text/plain", "OK");
    });
    // Android/ChromeOS captive portal probes
    server.on("/generate_204", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /generate_204");
        server.send(204, "text/plain", "");
    });
    server.on("/gen_204", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /gen_204");
        server.send(204, "text/plain", "");
    });
    // iOS/macOS captive portal
    server.on("/hotspot-detect.html", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /hotspot-detect.html");
        server.sendHeader("Location", "/settings", true);
        server.send(302, "text/html", "");
    });
    // Windows captive portal variants
    server.on("/fwlink", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /fwlink");
        server.sendHeader("Location", "/settings", true);
        server.send(302, "text/html", "");
    });
    server.on("/ncsi.txt", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /ncsi.txt");
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
        display.showDemo();
        server.send(200, "application/json", "{\"success\":true}");
    });
    server.on("/api/displaycolors/clear", HTTP_POST, [this]() { 
        SerialLog.println("[HTTP] POST /api/displaycolors/clear - returning to scanning");
        display.showScanning();  // Return to normal scanning state
        server.send(200, "application/json", "{\"success\":true}");
    });
    
    // Time settings and serial log API routes
    server.on("/api/timesettings", HTTP_GET, [this]() { handleTimeSettingsApi(); });
    server.on("/api/seriallog", HTTP_GET, [this]() { handleSerialLogApi(); });
    server.on("/api/seriallog/toggle", HTTP_POST, [this]() { handleSerialLogToggle(); });
    server.on("/api/seriallog/content", HTTP_GET, [this]() { handleSerialLogContent(); });
    
    // Note: onNotFound is set earlier to handle LittleFS static files
}

void WiFiManager::process() {
    server.handleClient();
    
    if (staEnabledByConfig) {
        checkSTAConnection();
    }
}

void WiFiManager::stop() {
    server.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    apActive = false;
    staConnected = false;
    SerialLog.println("WiFi stopped");
}

bool WiFiManager::isConnected() const {
    return staConnected;
}

bool WiFiManager::isAPActive() const {
    return apActive;
}

String WiFiManager::getIPAddress() const {
    if (staConnected) {
        return WiFi.localIP().toString();
    }
    return "";
}

String WiFiManager::getAPIPAddress() const {
    if (apActive) {
        return WiFi.softAPIP().toString();
    }
    return "";
}

void WiFiManager::handleStatus() {
    const V1Settings& settings = settingsManager.get();
    
    String json = "{";
    json += "\"wifi\":{";
    json += "\"sta_connected\":" + String(staConnected ? "true" : "false") + ",";
    json += "\"ap_active\":" + String(apActive ? "true" : "false") + ",";
    json += "\"sta_ip\":\"" + getIPAddress() + "\",";
    json += "\"ap_ip\":\"" + getAPIPAddress() + "\",";
    json += "\"ssid\":\"" + connectedSSID + "\",";
    json += "\"rssi\":" + String(staConnected ? WiFi.RSSI() : 0);
    json += "},";
    
    // Device info
    json += "\"device\":{";
    json += "\"uptime\":" + String(millis() / 1000) + ",";
    json += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"hostname\":\"v1g2\"";
    json += "}";
    
    if (getStatusJson) {
        json += "," + getStatusJson();
    }
    
    // Add alert info if callback is set
    if (getAlertJson) {
        json += ",\"alert\":" + getAlertJson();
    }
    
    json += "}";
    
    server.send(200, "application/json", json);
}

void WiFiManager::handleSettingsApi() {
    const V1Settings& settings = settingsManager.get();
    
    String json = "{";
    json += "\"ssid\":\"" + settings.ssid + "\",";
    json += "\"password\":\"********\",";  // Don't send actual password
    json += "\"ap_ssid\":\"" + settings.apSSID + "\",";
    json += "\"ap_password\":\"********\",";  // Don't send actual password
    json += "\"wifi_mode\":" + String(settings.wifiMode) + ",";
    json += "\"proxy_ble\":" + String(settings.proxyBLE ? "true" : "false") + ",";
    json += "\"proxy_name\":\"" + settings.proxyName + "\"";
    json += "}";
    
    server.send(200, "application/json", json);
}

void WiFiManager::handleSettingsSave() {
    SerialLog.println("=== handleSettingsSave() called ===");
    
    // Track if WiFi settings changed (these require restart)
    bool wifiChanged = false;
    const V1Settings& currentSettings = settingsManager.get();

    if (server.hasArg("ssid")) {
        String ssid = server.arg("ssid");
        String pass = server.arg("password");
        
        // Preserve existing password if placeholder sent
        if (pass == "********") {
            pass = currentSettings.password;
        }
        if (ssid != currentSettings.ssid || pass != currentSettings.password) {
            wifiChanged = true;
        }
        settingsManager.updatePrimaryWiFi(ssid, pass);
    }
    
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
        if (apSsid != currentSettings.apSSID || apPass != currentSettings.apPassword) {
            wifiChanged = true;
        }
        settingsManager.updateAPCredentials(apSsid, apPass);
    }
    
    // WiFi mode (AP, STA, AP+STA)
    WiFiModeSetting mode = V1_WIFI_AP;
    if (server.hasArg("wifi_mode")) {
        mode = static_cast<WiFiModeSetting>(server.arg("wifi_mode").toInt());
        mode = static_cast<WiFiModeSetting>(std::max(0, std::min(3, (int)mode)));
    }
    if (mode != currentSettings.wifiMode) {
        wifiChanged = true;
    }
    settingsManager.updateWiFiMode(mode);
    
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
    
    // All changes are queued in the settingsManager instance. Now, save them all at once.
    SerialLog.println("--- Calling settingsManager.save() ---");
    settingsManager.save();
    
    // The settingsManager instance is already up-to-date, no need to reload.
    // We can directly apply any changes that need to take immediate effect.
    if (server.hasArg("color_theme")) {
        display.updateColorTheme();
        SerialLog.println("Display color theme updated");
    }
    
    // Redirect with appropriate message
    String redirect = "/settings?saved=1";
    if (wifiChanged) {
        redirect += "&wifi=1";
    }
    server.sendHeader("Location", redirect);
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
    
    SerialLog.printf("Dark mode request: %s, success: %s\n", darkMode ? "ON" : "OFF", success ? "yes" : "no");
    
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
    
    SerialLog.printf("Mute request: %s, success: %s\n", muted ? "ON" : "OFF", success ? "yes" : "no");
    
    String json = "{\"success\":" + String(success ? "true" : "false") + 
                  ",\"muted\":" + String(muted ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

void WiFiManager::handleTimeSettingsSave() {
    bool changed = false;
    const V1Settings& settings = settingsManager.get();
    
    // Update timesync enabled flag
    bool enableTime = server.hasArg("enableTimesync");
    if (settings.enableTimesync != enableTime) {
        settingsManager.updateTimesync(enableTime);
        changed = true;
    }
    
    // Update multiple WiFi networks
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        String ssidKey = "wifi" + String(i) + "ssid";
        String pwdKey = "wifi" + String(i) + "pwd";
        
        if (server.hasArg(ssidKey.c_str())) {
            String newSSID = server.arg(ssidKey.c_str());
            String newPwd = server.arg(pwdKey.c_str());
            
            // If password is placeholder, keep existing password  
            if (newPwd == "********") {
                newPwd = settings.wifiNetworks[i].password;
            }
            
            if (settings.wifiNetworks[i].ssid != newSSID || settings.wifiNetworks[i].password != newPwd) {
                settingsManager.updateWiFiNetwork(i, newSSID, newPwd);
                changed = true;
                SerialLog.printf("WiFi network %d updated: %s\n", i+1, newSSID.c_str());
            }
        }
    }
    
    // Keep legacy fields in sync with network[0] for backward compat
    if (settingsManager.get().wifiNetworks[0].isValid()) {
        settingsManager.updateStaSSID(
            settingsManager.get().wifiNetworks[0].ssid, 
            settingsManager.get().wifiNetworks[0].password
        );
    }
    
    // Manual time setting
    if (server.hasArg("timestamp")) {
        time_t timestamp = (time_t)server.arg("timestamp").toInt();
        if (timestamp > 1609459200) {  // Valid if after 2021-01-01
            timeManager.setTime(timestamp);
            SerialLog.printf("Time set manually to: %ld\n", timestamp);
        }
    }
    
    if (changed) {
        settingsManager.save();
        SerialLog.printf("WiFi settings saved. Timesync: %s\n", enableTime ? "enabled" : "disabled");
        
        int networksAdded = populateStaNetworks();
        if (networksAdded > 0) {
            if (!staEnabledByConfig) {
                staEnabledByConfig = true;
                SerialLog.println("[WiFi] STA mode auto-enabled after configuring networks");
            }
            SerialLog.printf("[WiFi] STA network list refreshed (%d entries)\n", networksAdded);
            if (staConnected) {
                WiFi.disconnect(true);
                staConnected = false;
                connectedSSID = "";
                natEnabled = false;
            }
            lastStaRetry = 0;  // Trigger immediate retry with new credentials
        }
    }
    
    String response = changed ? 
        "Settings saved! <a href='/time'>Back to Time Settings</a>" :
        "No changes made. <a href='/time'>Back to Time Settings</a>";
    server.send(200, "text/html", response);
}

void WiFiManager::handleLogsData() {
    if (!alertLogger.isReady()) {
        server.send(503, "application/json", "{\"error\":\"SD card not mounted\"}");
        return;
    }

    // Optional: support /api/logs?n=50
    size_t maxLines = ALERT_LOG_MAX_RECENT;
    if (server.hasArg("n")) {
        int n = server.arg("n").toInt();
        if (n > 0) {
            if (n > 500) n = 500; // hard cap to protect RAM/CPU
            maxLines = (size_t)n;
        }
    }

    fs::FS* fs = alertLogger.getFilesystem();
    if (!fs) {
        server.send(500, "application/json", "{\"error\":\"filesystem unavailable\"}");
        return;
    }

    File file = fs->open(ALERT_LOG_PATH, FILE_READ);
    if (!file) {
        server.send(500, "application/json", "{\"error\":\"failed to open log\"}");
        return;
    }

    // Pass 1: count valid data lines
    size_t total = 0;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("ms,")) continue; // skip header/blank
        total++;
    }
    file.close();

    size_t startIndex = 0;
    if (total > maxLines) startIndex = total - maxLines;

    // Pass 2: stream JSON array of last N lines
    file = fs->open(ALERT_LOG_PATH, FILE_READ);
    if (!file) {
        server.send(500, "application/json", "{\"error\":\"failed to open log\"}");
        return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("[");

    bool first = true;
    size_t idx = 0;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("ms,")) continue;

        if (idx++ < startIndex) continue;

        // CSV columns expected (9):
        // 0=ms,1=event,2=band,3=freq,4=dir,5=front,6=rear,7=count,8=muted
        String cols[9];
        if (!splitCsvFixed(line, cols, 9)) continue;

        if (!first) server.sendContent(",");
        first = false;

        // Stream one object per row.
        // Emit BOTH new keys (ts, dir) and legacy keys (ms, direction).
        // Handle empty numeric fields by defaulting to 0, escape strings for JSON safety
        String obj;
        obj.reserve(250);
        obj += "{\"ts\":";
        obj += cols[0].length() > 0 ? cols[0] : "0";
        obj += ",\"ms\":";
        obj += cols[0].length() > 0 ? cols[0] : "0";
        obj += ",\"utc\":0";
        obj += ",\"event\":\"";
        obj += jsonEscape(cols[1]);
        obj += "\",\"band\":\"";
        obj += jsonEscape(cols[2]);
        obj += "\",\"freq\":";
        obj += cols[3].length() > 0 ? cols[3] : "0";
        obj += ",\"dir\":\"";
        obj += jsonEscape(cols[4]);
        obj += "\",\"direction\":\"";
        obj += jsonEscape(cols[4]);
        obj += "\",\"front\":";
        obj += cols[5].length() > 0 ? cols[5] : "0";
        obj += ",\"rear\":";
        obj += cols[6].length() > 0 ? cols[6] : "0";
        obj += ",\"count\":";
        obj += cols[7].length() > 0 ? cols[7] : "0";
        obj += ",\"muted\":";
        obj += ((cols[8] == "1" || cols[8] == "true") ? "true" : "false");
        obj += "}";

        server.sendContent(obj);
    }

    file.close();
    server.sendContent("]");
}

void WiFiManager::handleLogsClear() {
    if (!alertLogger.isReady()) {
        server.send(503, "application/json", "{\"error\":\"SD card not mounted\"}");
        return;
    }

    bool cleared = alertLogger.clear();
    server.send(cleared ? 200 : 500,
                "application/json",
                String("{\"success\":") + (cleared ? "true" : "false") + "}");
}

void WiFiManager::handleSerialLog() {
    if (!alertLogger.isReady()) {
        server.send(503, "text/plain", "SD card not mounted");
        return;
    }
    
    fs::FS* fs = alertLogger.getFilesystem();
    if (!fs->exists("/serial_log.txt")) {
        server.send(404, "text/plain", "No serial log file found");
        return;
    }
    
    File file = fs->open("/serial_log.txt", FILE_READ);
    if (!file) {
        server.send(500, "text/plain", "Failed to open serial log");
        return;
    }
    
    server.streamFile(file, "text/plain");
    file.close();
}

void WiFiManager::handleSerialLogClear() {
    if (!SerialLog.isEnabled()) {
        server.send(503, "application/json", "{\"error\":\"Serial logging not enabled\"}");
        return;
    }
    
    bool cleared = SerialLog.clear();
    server.send(cleared ? 200 : 500,
                "application/json",
                String("{\"success\":") + (cleared ? "true" : "false") + "}");
}

void WiFiManager::handleV1ProfilesList() {
    std::vector<String> profiles = v1ProfileManager.listProfiles();
    
    String json = "[";
    for (size_t i = 0; i < profiles.size(); i++) {
        if (i > 0) json += ",";
        json += "\"" + profiles[i] + "\"";
    }
    json += "]";
    
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
    SerialLog.printf("[V1Settings] Save request body: %s\n", body.c_str());
    
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
    
    if (v1ProfileManager.saveProfile(profile)) {
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to save profile\"}");
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
    if (!v1ProfileManager.hasCurrentSettings()) {
        server.send(200, "application/json", "{\"available\":false}");
        return;
    }
    
    String json = "{\"available\":true,\"settings\":";
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
    SerialLog.printf("[V1Settings] Push request: %s\n", body.c_str());
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
    
    // Check for bytes array first
    JsonArray bytesArray = doc["bytes"];
    if (bytesArray && bytesArray.size() == 6) {
        for (int i = 0; i < 6; i++) {
            bytes[i] = bytesArray[i].as<uint8_t>();
        }
        SerialLog.println("[V1Settings] Using raw bytes from request");
    } else {
        // Parse from individual settings (already deserialized)
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
        SerialLog.printf("[V1Settings] Built bytes from settings: %02X %02X %02X %02X %02X %02X\n",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    }
    
    // Get displayOn setting (defaults to true if not specified)
    bool displayOn = doc["displayOn"] | true;
    
    bool success = bleClient.writeUserBytes(bytes);
    if (success) {
        // Also set display on/off
        bleClient.setDisplayOn(displayOn);
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to write settings\"}");
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
    json += "\"muteVolume\":" + String(s.slot0MuteVolume) + "},";
    
    // Slot 1
    json += "{\"name\":\"" + s.slot1Name + "\",";
    json += "\"profile\":\"" + s.slot1_highway.profileName + "\",";
    json += "\"mode\":" + String(s.slot1_highway.mode) + ",";
    json += "\"color\":" + String(s.slot1Color) + ",";
    json += "\"volume\":" + String(s.slot1Volume) + ",";
    json += "\"muteVolume\":" + String(s.slot1MuteVolume) + "},";
    
    // Slot 2
    json += "{\"name\":\"" + s.slot2Name + "\",";
    json += "\"profile\":\"" + s.slot2_comfort.profileName + "\",";
    json += "\"mode\":" + String(s.slot2_comfort.mode) + ",";
    json += "\"color\":" + String(s.slot2Color) + ",";
    json += "\"volume\":" + String(s.slot2Volume) + ",";
    json += "\"muteVolume\":" + String(s.slot2MuteVolume) + "}";
    
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
    
    SerialLog.printf("[SaveSlot] Slot %d - volume: %d (was %d), muteVol: %d (was %d)\n", 
                  slot, vol, existingVol, mute, existingMute);
    
    settingsManager.setSlotVolumes(slot, vol, mute);
    
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
    
    SerialLog.printf("[PushNow] Slot %d volumes - main: %d, mute: %d\n", slot, mainVol, muteVol);
    
    if (mainVol != 0xFF || muteVol != 0xFF) {
        delay(100);
        SerialLog.printf("[PushNow] Setting volume - main: %d, muted: %d\n", mainVol, muteVol);
        bleClient.setVolume(mainVol, muteVol);
    } else {
        SerialLog.println("[PushNow] Volume: No change");
    }
    
    // Update active slot and refresh display profile indicator
    settingsManager.setActiveSlot(slot);
    display.drawProfileIndicator(slot);
    
    server.send(200, "application/json", "{\"success\":true}");
}

// ============= Display Colors Handlers =============

void WiFiManager::handleDisplayColorsSave() {
    SerialLog.println("[HTTP] POST /api/displaycolors");
    SerialLog.printf("[HTTP] Args count: %d\n", server.args());
    for (int i = 0; i < server.args(); i++) {
        SerialLog.printf("[HTTP] Arg %s = %s\n", server.argName(i).c_str(), server.arg(i).c_str());
    }
    
    uint16_t bogey = server.hasArg("bogey") ? server.arg("bogey").toInt() : 0xF800;
    uint16_t freq = server.hasArg("freq") ? server.arg("freq").toInt() : 0xF800;
    uint16_t arrow = server.hasArg("arrow") ? server.arg("arrow").toInt() : 0xF800;
    uint16_t bandL = server.hasArg("bandL") ? server.arg("bandL").toInt() : 0x001F;
    uint16_t bandKa = server.hasArg("bandKa") ? server.arg("bandKa").toInt() : 0xF800;
    uint16_t bandK = server.hasArg("bandK") ? server.arg("bandK").toInt() : 0x001F;
    uint16_t bandX = server.hasArg("bandX") ? server.arg("bandX").toInt() : 0x07E0;
    
    SerialLog.printf("[HTTP] Saving colors: bogey=%d freq=%d arrow=%d\n", bogey, freq, arrow);
    
    settingsManager.setDisplayColors(bogey, freq, arrow, bandL, bandKa, bandK, bandX);
    
    // Handle WiFi icon color separately if provided
    if (server.hasArg("wifiIcon")) {
        uint16_t wifiIcon = server.arg("wifiIcon").toInt();
        settingsManager.setWiFiIconColor(wifiIcon);
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
    
    // Trigger immediate display preview to show new colors
    display.showDemo();
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleDisplayColorsReset() {
    // Reset to default colors: Bogey/Freq/Arrow=Red, L/K=Blue, Ka=Red, X=Green, WiFi=Cyan
    settingsManager.setDisplayColors(0xF800, 0xF800, 0xF800, 0x001F, 0xF800, 0x001F, 0x07E0);
    settingsManager.setWiFiIconColor(0x07FF);  // Cyan
    // Reset bar colors: Green, Green, Yellow, Yellow, Red, Red
    settingsManager.setSignalBarColors(0x07E0, 0x07E0, 0xFFE0, 0xFFE0, 0xF800, 0xF800);
    
    // Trigger immediate display preview to show reset colors
    display.showDemo();
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleDisplayColorsApi() {
    const V1Settings& s = settingsManager.get();
    
    String json = "{";
    json += "\"bogey\":" + String(s.colorBogey) + ",";
    json += "\"freq\":" + String(s.colorFrequency) + ",";
    json += "\"arrow\":" + String(s.colorArrow) + ",";
    json += "\"bandL\":" + String(s.colorBandL) + ",";
    json += "\"bandKa\":" + String(s.colorBandKa) + ",";
    json += "\"bandK\":" + String(s.colorBandK) + ",";
    json += "\"bandX\":" + String(s.colorBandX) + ",";
    json += "\"wifiIcon\":" + String(s.colorWiFiIcon) + ",";
    json += "\"bar1\":" + String(s.colorBar1) + ",";
    json += "\"bar2\":" + String(s.colorBar2) + ",";
    json += "\"bar3\":" + String(s.colorBar3) + ",";
    json += "\"bar4\":" + String(s.colorBar4) + ",";
    json += "\"bar5\":" + String(s.colorBar5) + ",";
    json += "\"bar6\":" + String(s.colorBar6) + ",";
    json += "\"hideWifiIcon\":" + String(s.hideWifiIcon ? "true" : "false") + ",";
    json += "\"hideProfileIndicator\":" + String(s.hideProfileIndicator ? "true" : "false");
    json += "}";
    
    server.send(200, "application/json", json);
}

void WiFiManager::handleTimeSettingsApi() {
    const V1Settings& settings = settingsManager.get();
    String currentTime = timeManager.isTimeValid() ? timeManager.getTimestampISO() : "Not Set";
    
    String json = "{";
    json += "\"currentTime\":\"" + currentTime + "\",";
    json += "\"enableTimesync\":" + String(settings.enableTimesync ? "true" : "false") + ",";
    json += "\"wifiNetworks\":[";
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + htmlEscape(settings.wifiNetworks[i].ssid) + "\",";
        json += "\"password\":\"";
        json += (settings.wifiNetworks[i].password.length() > 0 ? "********" : "");
        json += "\"}";
    }
    json += "]}";
    
    server.send(200, "application/json", json);
}

void WiFiManager::handleSerialLogApi() {
    bool sdReady = alertLogger.isReady();
    bool logEnabled = SerialLog.isEnabled();
    size_t logSize = SerialLog.getLogSize();
    
    // Format size for display
    String sizeStr;
    if (logSize < 1024) {
        sizeStr = String(logSize) + " B";
    } else if (logSize < 1024 * 1024) {
        sizeStr = String(logSize / 1024.0, 1) + " KB";
    } else if (logSize < 1024 * 1024 * 1024) {
        sizeStr = String(logSize / (1024.0 * 1024.0), 1) + " MB";
    } else {
        sizeStr = String(logSize / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
    }
    
    String json = "{";
    json += "\"sdReady\":" + String(sdReady ? "true" : "false") + ",";
    json += "\"logEnabled\":" + String(logEnabled ? "true" : "false") + ",";
    json += "\"logSize\":\"" + sizeStr + "\"";
    json += "}";
    
    server.send(200, "application/json", json);
}

void WiFiManager::handleSerialLogToggle() {
    bool enable = server.hasArg("enable") && server.arg("enable") == "true";
    SerialLog.setEnabled(enable);
    
    String json = "{\"success\":true,\"enabled\":";
    json += SerialLog.isEnabled() ? "true" : "false";
    json += "}";
    server.send(200, "application/json", json);
}

void WiFiManager::handleSerialLogContent() {
    if (!alertLogger.isReady()) {
        server.send(503, "text/plain", "SD card not mounted");
        return;
    }
    
    fs::FS* fs = alertLogger.getFilesystem();
    if (!fs->exists("/serial_log.txt")) {
        server.send(200, "text/plain", "");
        return;
    }
    
    // Get optional tail parameter (last N bytes)
    size_t tailBytes = 32768; // Default 32KB
    if (server.hasArg("tail")) {
        tailBytes = server.arg("tail").toInt();
        if (tailBytes > 65536) tailBytes = 65536; // Cap at 64KB
    }
    
    File file = fs->open("/serial_log.txt", FILE_READ);
    if (!file) {
        server.send(500, "text/plain", "Failed to open log");
        return;
    }
    
    size_t fileSize = file.size();
    
    // If file is larger than tailBytes, seek to end - tailBytes
    if (fileSize > tailBytes) {
        file.seek(fileSize - tailBytes);
        // Skip to next newline to avoid partial line
        while (file.available() && file.read() != '\n') {}
    }
    
    // Stream remaining content
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/plain", "");
    
    const size_t bufSize = 1024;
    uint8_t buf[bufSize];
    while (file.available()) {
        size_t bytesRead = file.read(buf, bufSize);
        server.sendContent((const char*)buf, bytesRead);
    }
    file.close();
}

